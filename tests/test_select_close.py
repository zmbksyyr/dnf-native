"""Check select ready sets and the runtime's EBADF policy for concurrent close."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import platform
import struct
import subprocess

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from bootstrap import TOOLCHAIN, BUILD
from runtime_support import unpack_runtime

ROOT = Path(__file__).resolve().parents[1]
BIN = TOOLCHAIN


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hosts", type=Path, default=BUILD)
    args = parser.parse_args()
    records = []
    for arch, triple, lib, base in [("i686", "i386-linux-gnu", "lib", "0x8048000"),
                                   ("x86_64", "x86_64-linux-gnu", "lib64", "0x400000")]:
        directory = BUILD / "fixtures/select-close" / arch
        unpack_runtime(directory, arch)
        ld = directory / lib / "ld-2.17.so"
        fixture = directory / "select-close"
        command = [str(BIN / "clang.exe"), "--target=" + triple, "-fuse-ld=lld", "-nostdlib",
                   "-fno-stack-protector", "-fno-pic", "-no-pie", "-O2", "-Wall", "-Wextra", "-Werror",
                   "-Wl,-e,_start", "-Wl,--image-base=" + base, "-Wl,--dynamic-linker=/" + lib + "/ld-2.17.so",
                   str(ROOT / "tests/select_close.c"), "-L" + str(directory / lib),
                   "-l:libpthread.so.0", "-l:libc.so.6", "-o", str(fixture)]
        subprocess.run(command, check=True)
        host = args.hosts.resolve() / arch / "dnf-host.exe"
        command = [str(host), "--root", str(directory), sha(ld), "--run", str(fixture), sha(fixture), "select-close"]
        result = subprocess.run(command, capture_output=True, timeout=15)
        reports = [dict(zip(["attempt", "select_result", "errno", "empty_read_set"], row))
                   for row in struct.iter_unpack("<4i", result.stdout)]
        passed = result.returncode == 42 and len(reports) == 5
        records.append({"arch": arch, "host_sha256": sha(host), "fixture_sha256": sha(fixture),
                        "exit_code": result.returncode, "reports": reports,
                        "stderr": result.stderr.decode("utf-8", "replace"), "passed": passed})
        print(arch, "select/concurrent close", "PASS" if passed else "FAIL", "exit", result.returncode, flush=True)
    receipt = {"recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "os": platform.platform(),
               "scope": "Own ELF using upstream glibc/pthread: ready masks/counts and concurrent socket close. Cross-thread close/select is unspecified on Linux; this runtime returns EBADF. No original-service acceptance.",
               "results": records}
    (BUILD / "select-close-checks.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    if not all(row["passed"] for row in records):
        raise SystemExit("See ../x/dnf-native-output/select-close-checks.json")


if __name__ == "__main__":
    main()
