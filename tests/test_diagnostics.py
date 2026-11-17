"""Verify ENOSYS repeat records and a real guest fault in both hosts."""

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
        elf = BUILD / "fixtures" / arch / "diagnostics"
        elf.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(TOOLCHAIN / "clang.exe"), "--target=" + target, "-fuse-ld=lld", "-nostdlib", "-static",
                        "-Wl,--image-base=" + ("0x8048000" if arch == "i686" else "0x400000"),
                        str(ROOT / "tests/diagnostics.S"), "-o", str(elf)], env=env, check=True)
        host = BUILD / arch / "dnf-host.exe"
        result = subprocess.run([str(host), "--run", str(elf), sha256(elf), "/diagnostics-fixture"],
                                capture_output=True, text=True, timeout=20)
        log = result.stderr
        assert result.returncode == 125, log
        assert "code=0xc0000005" in log and "address=0x0" in log, log
        assert "symbol=crash_leaf+" in log and "symbol=_start+" in log, log
        assert "bx=0x12345678" in log and "instructions address=" in log and "stack address=" in log, log
        gaps = [line for line in log.splitlines() if line.startswith("syscall gap")]
        assert len(gaps) == 3 and all(f"count={n} " in line for n, line in zip((1, 2, 4), gaps)), log
        assert all("module=/diagnostics-fixture" in line and "nr=65535" in line for line in gaps), log
        assert sha256(elf) in log, log
        records.append({"arch": arch, "host_sha256": sha256(host), "elf_sha256": sha256(elf),
                        "exit_code": result.returncode, "stderr": log})
        print(arch, "ENOSYS repeats, real fault registers, symbol and frames: PASS")
    (BUILD / "diagnostics-checks.json").write_text(json.dumps({"os": platform.platform(), "results": records,
        "scope": "Own ELF fixtures; real fault delivery and diagnostic integrity."}, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
