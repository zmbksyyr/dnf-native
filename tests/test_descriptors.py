"""Check shared descriptor allocation and epoll isolation with real guest threads."""

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from bootstrap import TOOLCHAIN, BUILD

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hosts", type=Path, default=BUILD)
    args = parser.parse_args()
    env = dict(os.environ)
    env["PATH"] = str(TOOLCHAIN) + os.pathsep + env.get("PATH", "")
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    directory = BUILD / "probes" / stamp / "descriptors"
    records = []
    for arch, target, base in [("i686", "i386-linux-gnu", "0x8048000"), ("x86_64", "x86_64-linux-gnu", "0x400000")]:
        root = directory / arch
        root.mkdir(parents=True)
        (root / "regular").write_bytes(b"descriptor probe\n")
        fixture = root / "descriptor-concurrent"
        build = [str(TOOLCHAIN / "clang.exe"), "--target=" + target, "-fuse-ld=lld", "-nostdlib", "-static",
                 "-ffreestanding", "-fno-stack-protector", "-fno-pic", "-fomit-frame-pointer", "-O2",
                 "-Wall", "-Wextra", "-Werror", "-Wl,--image-base=" + base, "-Wl,-e,_start",
                 str(ROOT / "tests/descriptor_concurrent.c"), str(ROOT / "tests/futex_clone.S"), "-o", str(fixture)]
        subprocess.run(build, env=env, check=True)
        host = args.hosts.resolve() / arch / "dnf-host.exe"
        digest = hashlib.sha256(fixture.read_bytes()).hexdigest()
        command = [str(host), "--root", str(root), "0" * 64, "--run", str(fixture), digest, "descriptor-fixture"]
        process = subprocess.run(command, capture_output=True, timeout=60)
        stdout, stderr = process.stdout.decode(), process.stderr.decode(errors="replace")
        passed = process.returncode == 42 and stdout == "native-descriptor-allocation-isolation-ok\n"
        records.append({"arch": arch, "build": build, "command": command,
                        "host_sha256": hashlib.sha256(host.read_bytes()).hexdigest(), "fixture_sha256": digest,
                        "exit_code": process.returncode, "stdout": stdout, "stderr": stderr, "passed": passed})
        print(arch, "concurrent descriptor allocation/epoll isolation", "PASS" if passed else "FAIL", "exit", process.returncode, flush=True)
    receipt = directory / "receipt.json"
    receipt.write_text(json.dumps({"recorded_utc": stamp, "os": platform.platform(),
                                  "scope": "8 guest threads, 200 rounds above fd 514: unique epoll/eventfd/socket/pipe/file/maps/auxv/urandom descriptors and epoll data isolation; high socket/mixed select, full-length poll, actual RLIMIT_NOFILE exhaustion, close/reuse and allocation rollback",
                                  "results": records}, indent=2) + "\n", encoding="utf-8")
    print("Descriptor receipt:", receipt, flush=True)
    if not all(record["passed"] for record in records):
        raise SystemExit("Descriptor checks failed")


if __name__ == "__main__":
    main()
