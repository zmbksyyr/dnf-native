"""Execute independently compiled Linux ELF fixtures through our Windows hosts."""

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
BIN = TOOLCHAIN


def main():
    env = dict(os.environ)
    env["PATH"] = str(BIN) + os.pathsep + env.get("PATH", "")
    records, failures = [], []
    for arch, target, base in [("i686", "i386-linux-gnu", "0x8048000"), ("x86_64", "x86_64-linux-gnu", "0x400000")]:
        cases = [("entry", None, 42), ("unimplemented-tls", "NR_TLS_FAULT", 126), ("syscalls", None, 42)]
        if arch == "x86_64":
            cases.append(("redzone", "NR_REDZONE", 42))
        for name, macro, expected in cases:
            output = BUILD / "fixtures" / arch / name
            output.parent.mkdir(parents=True, exist_ok=True)
            build = [str(BIN / "clang.exe"), "--target=" + target, "-fuse-ld=lld", "-nostdlib", "-static",
                     "-ffreestanding", "-fno-stack-protector", "-fno-pic", "-O2",
                     "-Wl,--image-base=" + base, "-Wl,-e,_start",
                     str(ROOT / ("tests/syscalls.c" if name == "syscalls" else "tests/entry.S")), "-o", str(output)]
            if macro:
                build.append("-D" + macro)
            subprocess.run(build, env=env, check=True)
            sha = hashlib.sha256(output.read_bytes()).hexdigest()
            command = [str(BUILD / arch / "dnf-host.exe"), "--run", str(output), sha,
                       "fixture", "\u4e2d\u6587 \u53c2\u6570", "arg with spaces"]
            process = subprocess.run(command, capture_output=True, timeout=20)
            stdout, stderr = process.stdout.decode("utf-8", "replace"), process.stderr.decode("utf-8", "replace")
            passed = process.returncode == expected and stdout == ("writev-ok\n" if name == "syscalls" else "native-elf-ok\n")
            if name == "unimplemented-tls":
                passed = passed and "unimplemented guest TLS site" in stderr
            if not passed:
                failures.append(f"{arch}/{name}")
            record = {"case": f"{arch}/{name}", "build": build, "command": command,
                      "fixture_sha256": sha, "expected_exit_code": expected, "exit_code": process.returncode,
                      "stdout": stdout, "stderr": stderr, "passed": passed}
            records.append(record)
            print(record["case"], "PASS" if passed else "FAIL", "exit", process.returncode, flush=True)
    result = {"recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "os": platform.platform(),
              "scope": "Own Linux ELF fixtures only; not original service acceptance", "results": records,
              "failures": failures}
    (BUILD / "entry-checks.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if failures:
        raise SystemExit("Execution checks failed: " + ", ".join(failures))


if __name__ == "__main__":
    main()
