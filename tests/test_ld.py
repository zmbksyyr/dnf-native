"""Probe real upstream Linux ld.so startup, without claiming application startup."""

import datetime
import hashlib
import json
from pathlib import Path
import platform
import subprocess

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from bootstrap import BUILD
from runtime_support import unpack_runtime

ROOT = Path(__file__).resolve().parents[1]


def main():
    records, failures = [], []
    for arch, relative in [("i686", "lib/ld-2.17.so"), ("x86_64", "lib64/ld-2.17.so")]:
        directory = unpack_runtime(BUILD / "fixtures/ld" / arch, arch)
        elf = directory / relative
        sha = hashlib.sha256(elf.read_bytes()).hexdigest()
        for case, args in [("usage", []), ("missing-program", ["--help"])]:
            command = [str(BUILD / arch / "dnf-host.exe"), "--root", str(directory), sha,
                       "--run", str(elf), sha, "ld.so", *args]
            process = subprocess.run(command, capture_output=True, timeout=20)
            stdout = process.stdout.decode("utf-8", "replace")
            stderr = process.stderr.decode("utf-8", "replace")
            if case == "usage":
                passed = process.returncode == 127 and "Usage: ld.so" in stdout + stderr and "syscall gap" not in stderr
            else:
                passed = (process.returncode == 127 and "syscall gap" not in stderr
                          and "cannot open shared object file: No such file or directory" in stdout + stderr)
            if not passed:
                failures.append(f"{arch}/{case}")
            records.append({"case": f"{arch}/{case}", "command": command, "elf_sha256": sha,
                            "exit_code": process.returncode, "stdout": stdout, "stderr": stderr, "passed": passed})
            print(f"{arch}/{case}: {'PASS' if passed else 'FAIL'} exit={process.returncode}", flush=True)
    result = {"recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "os": platform.platform(),
              "scope": "Official ld.so entry/usage and genuine ENOENT for a missing program; no DNF service acceptance.",
              "results": records, "failures": failures}
    (BUILD / "ld-start-checks.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if failures:
        raise SystemExit("ld.so checks failed: " + ", ".join(failures))


if __name__ == "__main__":
    main()
