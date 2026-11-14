"""Check futex requeue and atomic wake operations with native guest threads."""

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
        fixture = ROOT / "build/fixtures" / arch / "futex"
        fixture.parent.mkdir(parents=True, exist_ok=True)
        build = [str(BIN / "clang.exe"), "--target=" + target, "-fuse-ld=lld", "-nostdlib", "-static",
                 "-ffreestanding", "-fno-stack-protector", "-fno-pic", "-fomit-frame-pointer", "-O2",
                 "-Wall", "-Wextra", "-Werror", "-Wl,--image-base=" + base, "-Wl,-e,_start",
                 str(ROOT / "tests/futex.c"), str(ROOT / "tests/futex_clone.S"), "-o", str(fixture)]
        subprocess.run(build, env=env, check=True)
        host = args.hosts.resolve() / arch / "dnf-host.exe"
        digest = hashlib.sha256(fixture.read_bytes()).hexdigest()
        command = [str(host), "--run", str(fixture), digest, "futex-fixture"]
        process = subprocess.run(command, capture_output=True, timeout=15)
        stdout, stderr = process.stdout.decode(), process.stderr.decode(errors="replace")
        passed = process.returncode == 42 and stdout == "native-futex-requeue-wake-op-ok\n"
        records.append({"arch": arch, "build": build, "command": command,
                        "host_sha256": hashlib.sha256(host.read_bytes()).hexdigest(), "fixture_sha256": digest,
                        "exit_code": process.returncode, "stdout": stdout, "stderr": stderr, "passed": passed})
        print(arch, "futex requeue/wake-op", "PASS" if passed else "FAIL", "exit", process.returncode, flush=True)
    result = {"recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "os": platform.platform(),
              "scope": "Own native Linux ELF threads: compare/requeue, conditional wakes, atomic word operations and clear_tid; no service or full futex acceptance.",
              "results": records}
    (ROOT / "build/futex-checks.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if not all(record["passed"] for record in records):
        raise SystemExit("Futex checks failed; see build/futex-checks.json")


if __name__ == "__main__":
    main()
