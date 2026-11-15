"""Check concurrent guest mmap records with eight native Linux threads."""

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / ".local/toolchains/llvm-mingw-20261104-msvcrt-x86_64/bin"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hosts", type=Path, default=ROOT / "build")
    args = parser.parse_args()
    env = dict(os.environ)
    env["PATH"] = str(BIN) + os.pathsep + env.get("PATH", "")
    records = []
    for arch, target, base in [("i686", "i386-linux-gnu", "0x8048000"), ("x86_64", "x86_64-linux-gnu", "0x400000")]:
        fixture = ROOT / "build/fixtures" / arch / "concurrent-mappings"
        build = [str(BIN / "clang.exe"), "--target=" + target, "-fuse-ld=lld", "-nostdlib", "-static",
                 "-ffreestanding", "-fno-stack-protector", "-fno-pic", "-fomit-frame-pointer", "-O2",
                 "-Wall", "-Wextra", "-Werror", "-Wl,--image-base=" + base, "-Wl,-e,_start",
                 str(ROOT / "tests/concurrent_mappings.c"), str(ROOT / "tests/futex_clone.S"), "-o", str(fixture)]
        subprocess.run(build, env=env, check=True)
        digest = hashlib.sha256(fixture.read_bytes()).hexdigest()
        host = args.hosts.resolve() / arch / "dnf-host.exe"
        process = subprocess.run([str(host), "--run", str(fixture), digest, "concurrent-mappings"],
                                 capture_output=True, timeout=30)
        stdout, stderr = process.stdout.decode(errors="replace"), process.stderr.decode(errors="replace")
        passed = process.returncode == 42 and stdout == "native-concurrent-mappings-ok\n"
        records.append({"arch": arch, "host_sha256": hashlib.sha256(host.read_bytes()).hexdigest(),
                        "fixture_sha256": digest, "exit_code": process.returncode,
                        "stdout": stdout, "stderr": stderr, "passed": passed})
        print(arch, "concurrent mappings", "PASS" if passed else "FAIL", "exit", process.returncode, flush=True)
    result = {"recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "os": platform.platform(),
              "scope": "Eight native guest threads perform 200 rounds of mmap/mprotect/read/write/munmap; no service or load acceptance.",
              "results": records}
    (ROOT / "build/vm-concurrent-checks.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if not all(record["passed"] for record in records):
        raise SystemExit("Concurrent mapping checks failed; see build/vm-concurrent-checks.json")


if __name__ == "__main__":
    main()
