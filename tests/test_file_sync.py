"""Check real rename replacement and shared mapped-file writeback through both hosts."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import platform
import subprocess

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from bootstrap import TOOLCHAIN, BUILD

ROOT = Path(__file__).resolve().parents[1]
BIN = TOOLCHAIN


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hosts", type=Path, default=BUILD)
    args = parser.parse_args()
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    records = []
    for arch, target, base in [("i686", "i386-linux-gnu", "0x8048000"), ("x86_64", "x86_64-linux-gnu", "0x400000")]:
        for mode in ["rename", "msync"]:
            private = BUILD / "probes" / stamp / ("file-sync-" + arch) / mode
            private.mkdir(parents=True)
            fixture = private / "fixture"
            command = [str(BIN / "clang.exe"), "--target=" + target, "-fuse-ld=lld", "-nostdlib", "-static",
                       "-ffreestanding", "-fno-stack-protector", "-fno-pic", "-fomit-frame-pointer", "-O2",
                       "-Wall", "-Wextra", "-Werror", "-Wl,--image-base=" + base, "-Wl,-e,_start",
                       str(ROOT / "tests/file_sync.c"), "-o", str(fixture)]
            if mode == "rename":
                command.append("-DNR_RENAME")
            subprocess.run(command, check=True)
            host = args.hosts.resolve() / arch / "dnf-host.exe"
            command = [str(host), "--root", str(private), "0" * 64, "--run", str(fixture), sha(fixture), "/file-sync"]
            result = subprocess.run(command, capture_output=True, timeout=10)
            path = private / ("destination" if mode == "rename" else "shared")
            data = path.read_bytes()
            expected = b"n" if mode == "rename" else b"s" + b"\0" * 8190 + b"z"
            passed = result.returncode == 42 and data == expected and (mode != "rename" or not (private / "incoming").exists())
            records.append({"arch": arch, "case": mode, "host_sha256": sha(host), "fixture_sha256": sha(fixture),
                            "exit_code": result.returncode, "persisted_sha256": sha(path), "passed": passed,
                            "stderr": result.stderr.decode("utf-8", "replace")})
            print(arch, mode, "PASS" if passed else "FAIL", "exit", result.returncode, flush=True)
    receipt = {"recorded_utc": stamp, "os": platform.platform(),
               "scope": "Own ELF fixtures: rename replacement of a closed destination, explicit EACCES without mutation for an open destination, and MS_SYNC after closing the original fd, asynchronous/anonymous mappings and error cases. Byte comparison after exit. Open-target POSIX replacement, full msync, security-service and power-loss acceptance excluded.",
               "results": records}
    (BUILD / "file-sync-checks.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    if not all(row["passed"] for row in records):
        raise SystemExit("See ../x/dnf-native-output/file-sync-checks.json")


if __name__ == "__main__":
    main()
