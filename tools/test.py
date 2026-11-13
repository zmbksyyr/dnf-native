"""Build and check original ELF mapping; this is not service execution acceptance."""

import datetime
import hashlib
import json
from pathlib import Path
import platform
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def invoke(host, path, expected):
    command = [str(host), "--map", str(path), expected]
    result = subprocess.run(command, capture_output=True, text=True, timeout=20)
    return {"command": command, "exit_code": result.returncode,
            "stdout": result.stdout, "stderr": result.stderr}


def main():
    subprocess.run([sys.executable, "tools/build.py"], cwd=ROOT, check=True)
    source = json.loads((ROOT / "docs/evidence/resource-inventory.json").read_text(encoding="utf-8"))
    hosts = {32: ROOT / "build/i686/dnf-host.exe", 64: ROOT / "build/x86_64/dnf-host.exe"}
    results = []
    for asset in source["assets"]:
        if asset["group"] not in ("core_16_elf", "security_3_elf"):
            continue
        row = invoke(hosts[asset["elf"]["bits"]], ROOT / "originals/dp123" / asset["path"], asset["sha256"])
        assert row["exit_code"] == 0, row
        mapped = json.loads(row["stdout"])
        assert int(mapped["entry"], 16) == int(asset["elf"]["entry"], 16), row
        assert int(mapped["bias"], 16) == 0 and mapped["executed"] is False, row
        row["case"] = asset["path"]
        row["expected"] = "original fixed-address PT_LOAD mapping; not executed"
        results.append(row)
    relay = ROOT / "originals/dp123/home/neople/relay/df_relay_r"
    raw = relay.read_bytes()
    expected = hashlib.sha256(raw).hexdigest()
    bad = ROOT / "build/invalid-inputs"
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
    (ROOT / "build/map-checks.json").write_text(json.dumps(receipt, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Passed: {len(results) - len(cases)} original fixed-address mappings and {len(cases)} rejection checks")


if __name__ == "__main__":
    main()
