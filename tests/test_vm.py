"""Check anonymous PROT_NONE, cross-region unmap and fixed remap through both hosts."""

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
    records = []
    for arch, target, base in [("i686", "i386-linux-gnu", "0x8048000"),
                               ("x86_64", "x86_64-linux-gnu", "0x400000")]:
        fixture = BUILD / "fixtures" / arch / "vm"
        fixture.parent.mkdir(parents=True, exist_ok=True)
        build = [str(BIN / "clang.exe"), "--target=" + target, "-fuse-ld=lld", "-nostdlib", "-static",
                 "-ffreestanding", "-fno-stack-protector", "-fno-pic", "-fomit-frame-pointer", "-O2",
                 "-Wall", "-Wextra", "-Werror", "-Wl,--image-base=" + base, "-Wl,-e,_start",
                 str(ROOT / "tests/vm.c"), "-o", str(fixture)]
        subprocess.run(build, env=env, check=True)
        host = BUILD / arch / "dnf-host.exe"
        sha = hashlib.sha256(fixture.read_bytes()).hexdigest()
        command = [str(host), "--run", str(fixture), sha, "vm-fixture"]
        process = subprocess.run(command, capture_output=True, timeout=20)
        stdout = process.stdout.decode("utf-8", "replace")
        stderr = process.stderr.decode("utf-8", "replace")
        passed = process.returncode == 42 and stdout == "anon-vm-ok\n"
        records.append({"arch": arch, "build": build, "command": command,
                        "host_sha256": hashlib.sha256(host.read_bytes()).hexdigest(),
                        "fixture_sha256": sha, "exit_code": process.returncode,
                        "stdout": stdout, "stderr": stderr, "passed": passed})
        print(arch, "PASS" if passed else "FAIL", "exit", process.returncode, flush=True)
    result = {"recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "os": platform.platform(), "scope": "Own anonymous VM fixtures: permissions, partial/fixed remap, cross-reservation unmap, retained outside pages and repeated holes; not full mmap or service acceptance",
              "results": records}
    (BUILD / "vm-checks.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if not all(record["passed"] for record in records):
        raise SystemExit("Anonymous VM checks failed; see ../x/dnf-native-output/vm-checks.json")


if __name__ == "__main__":
    main()
