"""Check futex requeue and atomic wake operations with native guest threads."""

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
from runtime_support import unpack_runtime

ROOT = Path(__file__).resolve().parents[1]
BIN = TOOLCHAIN


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hosts", type=Path, default=BUILD)
    parser.add_argument("--robust", action="store_true", help="Check robust list registration, owner death and malformed lists")
    args = parser.parse_args()
    env = dict(os.environ)
    env["PATH"] = str(BIN) + os.pathsep + env.get("PATH", "")
    records = []
    for arch, target, base in [("i686", "i386-linux-gnu", "0x8048000"), ("x86_64", "x86_64-linux-gnu", "0x400000")]:
        fixture = BUILD / "fixtures" / arch / ("robust" if args.robust else "futex")
        fixture.parent.mkdir(parents=True, exist_ok=True)
        build = [str(BIN / "clang.exe"), "--target=" + target, "-fuse-ld=lld", "-nostdlib", "-static",
                 "-ffreestanding", "-fno-stack-protector", "-fno-pic", "-fomit-frame-pointer", "-O2",
                 "-Wall", "-Wextra", "-Werror", "-Wl,--image-base=" + base, "-Wl,-e,_start",
                 str(ROOT / "tests/futex.c"), str(ROOT / "tests/futex_clone.S"), "-o", str(fixture)]
        if args.robust:
            build.append("-DROBUST_FIXTURE")
        subprocess.run(build, env=env, check=True)
        host = args.hosts.resolve() / arch / "dnf-host.exe"
        digest = hashlib.sha256(fixture.read_bytes()).hexdigest()
        command = [str(host), "--run", str(fixture), digest, "futex-fixture"]
        process = subprocess.run(command, capture_output=True, timeout=15)
        stdout, stderr = process.stdout.decode(), process.stderr.decode(errors="replace")
        expected = "native-robust-owner-death-ok\n" if args.robust else "native-futex-requeue-wake-op-ok\n"
        passed = process.returncode == 42 and stdout == expected
        if args.robust:
            passed = passed and all(reason in stderr for reason in ["unreadable head", "unreadable link", "list limit", "PI unsupported"])
        records.append({"arch": arch, "build": build, "command": command,
                        "host_sha256": hashlib.sha256(host.read_bytes()).hexdigest(), "fixture_sha256": digest,
                        "exit_code": process.returncode, "stdout": stdout, "stderr": stderr, "passed": passed})
        print(arch, "robust owner death" if args.robust else "futex requeue/wake-op", "PASS" if passed else "FAIL", "exit", process.returncode, flush=True)
        if args.robust:
            directory = BUILD / "fixtures/robust-pthread" / arch
            unpack_runtime(directory, arch)
            lib = "lib64" if arch == "x86_64" else "lib"
            program, loader = directory / "robust-pthread", directory / lib / "ld-2.17.so"
            dynamic_build = [str(BIN / "clang.exe"), "--target=" + target, "-fuse-ld=lld", "-nostdlib",
                             "-fno-stack-protector", "-fno-pic", "-no-pie", "-O2", "-Wall", "-Wextra", "-Werror",
                             "-Wl,--image-base=" + base, "-Wl,-e,_start", "-Wl,--dynamic-linker=/" + lib + "/ld-2.17.so",
                             str(ROOT / "tests/robust_pthread.c"), "-L" + str(directory / lib),
                             "-l:libpthread.so.0", "-l:libc.so.6", "-o", str(program)]
            subprocess.run(dynamic_build, env=env, check=True)
            loader_sha = hashlib.sha256(loader.read_bytes()).hexdigest()
            program_sha = hashlib.sha256(program.read_bytes()).hexdigest()
            command = [str(host), "--root", str(directory), loader_sha, "--run", str(program), program_sha, "robust-pthread"]
            process = subprocess.run(command, capture_output=True, timeout=20)
            stderr = process.stderr.decode(errors="replace")
            passed = process.returncode == 42 and "robust cleanup stopped" not in stderr
            records.append({"arch": arch, "case": "packaged-glibc-pthread", "build": dynamic_build,
                            "command": command, "host_sha256": hashlib.sha256(host.read_bytes()).hexdigest(),
                            "fixture_sha256": program_sha, "loader_sha256": loader_sha,
                            "exit_code": process.returncode, "stderr": stderr, "passed": passed})
            print(arch, "glibc robust mutex EOWNERDEAD/recovery/ENOTRECOVERABLE", "PASS" if passed else "FAIL",
                  "exit", process.returncode, flush=True)
    result = {"recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "os": platform.platform(),
              "scope": ("Own native Linux ELF threads: robust owner death, negative futex_offset, pending linked/unlinked/unlocked nodes, real waiter wake, bad length/pointers/cycles and explicit PI diagnostic. No process-shared futex acceptance." if args.robust else
                        "Own native Linux ELF threads: compare/requeue, conditional wakes, atomic word operations and clear_tid; no service or full futex acceptance."),
              "results": records}
    (BUILD / ("robust-checks.json" if args.robust else "futex-checks.json")).write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if not all(record["passed"] for record in records):
        raise SystemExit("Futex checks failed; see the external robust-checks.json or futex-checks.json")


if __name__ == "__main__":
    main()
