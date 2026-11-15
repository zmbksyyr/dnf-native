"""Verify eventfd counter/overflow/semaphore, poll/epoll and cross-thread IO."""
import json
import os
from pathlib import Path
import platform
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from bootstrap import BUILD, TOOLCHAIN, sha256
ROOT = Path(__file__).resolve().parents[1]


def main():
    env = dict(os.environ, PATH=str(TOOLCHAIN) + os.pathsep + os.environ.get("PATH", ""))
    records = []
    for arch, target in [("i686", "i386-linux-gnu"), ("x86_64", "x86_64-linux-gnu")]:
        elf = BUILD / "fixtures" / arch / "ipc"
        elf.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(TOOLCHAIN / "clang.exe"), "--target=" + target, "-fuse-ld=lld", "-nostdlib", "-static",
                        "-ffreestanding", "-fno-stack-protector", "-fno-pic", "-fomit-frame-pointer", "-O2",
                        "-Wall", "-Wextra", "-Werror", "-Wl,--image-base=" + ("0x8048000" if arch == "i686" else "0x400000"),
                        str(ROOT / "tests/ipc.c"), str(ROOT / "tests/futex_clone.S"), "-o", str(elf)], env=env, check=True)
        host = BUILD / arch / "dnf-host.exe"
        p = subprocess.run([str(host), "--run", str(elf), sha256(elf), "ipc-fixture"], capture_output=True, timeout=20)
        log = p.stderr.decode("utf-8", "replace")
        assert p.returncode == 42 and p.stdout == b"native-eventfd-ok\n", (p.returncode, log)
        records.append({"arch": arch, "host_sha256": sha256(host), "elf_sha256": sha256(elf), "stderr": log, "exit_code": p.returncode})
        print(arch, "eventfd counter, overflow, semaphore, poll/epoll and thread: PASS")
    (BUILD / "ipc-checks.json").write_text(json.dumps({"os": platform.platform(), "results": records}, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
