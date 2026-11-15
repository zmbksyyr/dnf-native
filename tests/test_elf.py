"""Build and check original ELF mapping; this is not service execution acceptance."""

import datetime
import configparser
import hashlib
import json
from pathlib import Path
import platform
import struct
import subprocess
import sys
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from resource_baseline import RESOURCES, verify_baseline

from bootstrap import BUILD

ROOT = Path(__file__).resolve().parents[1]


def invoke(host, path, expected):
    command = [str(host), "--map", str(path), expected]
    result = subprocess.run(command, capture_output=True, text=True, timeout=20)
    return {"command": command, "exit_code": result.returncode,
            "stdout": result.stdout, "stderr": result.stderr}


def main():
    verify_baseline()
    source = configparser.ConfigParser(interpolation=None)
    work = BUILD / "fixtures/service-mapping"
    with zipfile.ZipFile(RESOURCES / "native.zip") as archive:
        source.read_string(archive.read("services.template.ini").decode("utf-16"))
        assets = []
        for index in range(source.getint("services", "count")):
            name = source["services"]["service" + str(index)]
            entry = source[name]
            member = entry["root"] + entry["elf"]
            target = work / member
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(archive.read(member))
            bits = 64 if entry["root"] == "x86_64" else 32
            address = struct.unpack_from("<Q" if bits == 64 else "<I", target.read_bytes(), 24)[0]
            assets.append((name, target, entry["elf_sha256"], bits, address))
    hosts = {32: BUILD / "i686/dnf-host.exe", 64: BUILD / "x86_64/dnf-host.exe"}
    results = []
    for name, target, digest, bits, address in assets:
        row = invoke(hosts[bits], target, digest)
        assert row["exit_code"] == 0, row
        mapped = json.loads(row["stdout"])
        assert int(mapped["entry"], 16) == address, row
        assert int(mapped["bias"], 16) == 0 and mapped["executed"] is False, row
        row["case"] = name
        row["expected"] = "original fixed-address PT_LOAD mapping; not executed"
        results.append(row)
    relay = work / "i686/home/neople/relay/df_relay_r"
    raw = relay.read_bytes()
    expected = hashlib.sha256(raw).hexdigest()
    bad = BUILD / "invalid-inputs"
    bad.mkdir(parents=True, exist_ok=True)
    cases = [
        ("hash-mismatch", hosts[32], relay, "0" * 64, "SHA-256 mismatch"),
        ("architecture-mismatch", hosts[64], relay, expected, "architecture or header"),
    ]
    for name, content in [("truncated", raw[:20]), ("phoff-outside-file", raw[:28] + struct.pack("<I", 0xFFFFFFF0) + raw[32:])]:
        path = bad / name
        path.write_bytes(content)
        cases.append((name, hosts[32], path, hashlib.sha256(content).hexdigest(),
                      "cannot read ELF" if name == "truncated" else "architecture or header"))
    for name, message in [("invalid-load-extent", "PT_LOAD bounds"), ("entry-outside-executable", "executable entry")]:
        content = bytearray(raw)
        if name == "invalid-load-extent":
            phoff = struct.unpack_from("<I", content, 28)[0]
            stride, count = struct.unpack_from("<HH", content, 42)
            index = next(i for i in range(count) if struct.unpack_from("<I", content, phoff + i * stride)[0] == 1)
            struct.pack_into("<I", content, phoff + index * stride + 16, 0xFFFFFFFF)
        else:
            struct.pack_into("<I", content, 24, 1)
        path = bad / name
        path.write_bytes(content)
        cases.append((name, hosts[32], path, hashlib.sha256(content).hexdigest(), message))
    for name, host, path, sha, message in cases:
        row = invoke(host, path, sha)
        assert row["exit_code"] == 1 and message in row["stderr"], row
        row["case"] = name
        row["expected"] = "rejected before guest execution"
        results.append(row)
    receipt = {
        "recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "os": platform.platform(), "scope": "ELF validation and private initial PT_LOAD mapping only",
        "guest_execution": False, "service_acceptance": False,
        "host_sha256": {str(bits): hashlib.sha256(path.read_bytes()).hexdigest() for bits, path in hosts.items()},
        "results": results,
    }
    (BUILD / "map-checks.json").write_text(json.dumps(receipt, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Passed: {len(results) - len(cases)} original fixed-address mappings and {len(cases)} rejection checks")


if __name__ == "__main__":
    main()
