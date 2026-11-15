"""Check concurrent guest mmap records with eight native Linux threads."""

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
BIN = TOOLCHAIN


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hosts", type=Path, default=BUILD)
    args = parser.parse_args()
    env = dict(os.environ)
    env["PATH"] = str(BIN) + os.pathsep + env.get("PATH", "")
    records = []
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    for arch, target, base in [("i686", "i386-linux-gnu", "0x8048000"), ("x86_64", "x86_64-linux-gnu", "0x400000")]:
        fixture = BUILD / "fixtures" / arch / "concurrent-mappings"
        build = [str(BIN / "clang.exe"), "--target=" + target, "-fuse-ld=lld", "-nostdlib", "-static",
                 "-ffreestanding", "-fno-stack-protector", "-fno-pic", "-fomit-frame-pointer", "-O2",
                 "-Wall", "-Wextra", "-Werror", "-Wl,--image-base=" + base, "-Wl,-e,_start",
                 str(ROOT / "tests/concurrent_mappings.c"), str(ROOT / "tests/futex_clone.S"), "-o", str(fixture)]
        subprocess.run(build, env=env, check=True)
        digest = hashlib.sha256(fixture.read_bytes()).hexdigest()
        host = args.hosts.resolve() / arch / "dnf-host.exe"
        instance = BUILD / "probes" / stamp / ("concurrent-mappings-" + arch)
        instance.mkdir(parents=True)
        process = subprocess.run([str(host), "--root", str(instance), "0" * 64,
                                  "--run", str(fixture), digest, "concurrent-mappings"],
                                 capture_output=True, timeout=30)
        stdout, stderr = process.stdout.decode(errors="replace"), process.stderr.decode(errors="replace")
        passed = process.returncode == 42 and stdout == "native-concurrent-mappings-ok\n"
        records.append({"arch": arch, "host_sha256": hashlib.sha256(host.read_bytes()).hexdigest(),
                        "fixture_sha256": digest, "exit_code": process.returncode,
                        "stdout": stdout, "stderr": stderr, "passed": passed})
        print(arch, "concurrent mappings", "PASS" if passed else "FAIL", "exit", process.returncode, flush=True)
    result = {"recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "os": platform.platform(),
              "scope": "Eight native guest threads perform 200 rounds of mmap/mprotect/read/write/munmap while the parent reads proc maps snapshots; no service or load acceptance.",
              "results": records}
    (BUILD / "vm-concurrent-checks.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if not all(record["passed"] for record in records):
        raise SystemExit("Concurrent mapping checks failed; see ../x/dnf-native-output/vm-concurrent-checks.json")


if __name__ == "__main__":
    main()
