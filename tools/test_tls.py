"""Check native TLS loads/stores, flags and call/jump return semantics."""

import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / ".local/toolchains/llvm-mingw-20261104-msvcrt-x86_64/bin"


def main():
    env = dict(os.environ)
    env["PATH"] = str(BIN) + os.pathsep + env.get("PATH", "")
    records = []
    for arch, target, base in [("i686", "i386-linux-gnu", "0x8048000"),
                               ("x86_64", "x86_64-linux-gnu", "0x400000")]:
        fixture = ROOT / "build/fixtures" / arch / "tls"
        fixture.parent.mkdir(parents=True, exist_ok=True)
        build = [str(BIN / "clang.exe"), "--target=" + target, "-fuse-ld=lld", "-nostdlib", "-static",
                 "-ffreestanding", "-fno-stack-protector", "-fno-pic", "-mno-red-zone", "-O2",
                 "-Wall", "-Wextra", "-Werror", "-Wl,--image-base=" + base, "-Wl,-e,_start",
                 str(ROOT / "tests/tls.c"), "-o", str(fixture)]
        subprocess.run(build, env=env, check=True)
        host = ROOT / "build" / arch / "dnf-host.exe"
        sha = hashlib.sha256(fixture.read_bytes()).hexdigest()
        command = [str(host), "--run", str(fixture), sha, "tls-fixture"]
        started = time.perf_counter()
        process = subprocess.run(command, capture_output=True, timeout=30)
        elapsed = time.perf_counter() - started
        stdout = process.stdout.decode("utf-8", "replace")
        stderr = process.stderr.decode("utf-8", "replace")
        passed = process.returncode == 42 and stdout == "native-tls-ok\n"
        records.append({"arch": arch, "build": build, "command": command,
                        "host_sha256": hashlib.sha256(host.read_bytes()).hexdigest(),
                        "fixture_sha256": sha, "exit_code": process.returncode,
                        "elapsed_seconds": round(elapsed, 6), "rounds": 10000,
                        "stdout": stdout, "stderr": stderr, "passed": passed})
        print(arch, "PASS" if passed else "FAIL", "exit", process.returncode,
              "seconds", round(elapsed, 3), flush=True)
    result = {"recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "os": platform.platform(), "scope": "Own TLS fixtures; timing is local observation only, not service acceptance",
              "results": records}
    (ROOT / "build/tls-checks.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if not all(record["passed"] for record in records):
        raise SystemExit("TLS checks failed; see build/tls-checks.json")


if __name__ == "__main__":
    main()
