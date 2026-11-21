"""Check trap cache ownership and parallel collisions on both host architectures."""
import json
import os
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from bootstrap import BUILD, TOOLCHAIN

ROOT = Path(__file__).resolve().parents[1]


def main():
    output = BUILD / "fixtures/site-cache"
    output.mkdir(parents=True, exist_ok=True)
    records = []
    for arch in ("i686", "x86_64"):
        binary = output / (arch + ".exe")
        command = [str(TOOLCHAIN / (arch + "-w64-mingw32-clang.exe")), "-std=c11",
                   "-Wall", "-Wextra", "-Werror", "-O2", "-ffunction-sections", "-fdata-sections",
                   "-Wl,--gc-sections", "-static", "-I" + str(ROOT / "src/runtime"),
                   str(ROOT / "tests/site_cache.c"), str(ROOT / "src/runtime/site_cache.c"), "-o", str(binary)]
        subprocess.run(command, check=True)
        for disabled in (False, True):
            env = dict(os.environ, NR_SITE_CACHE_OFF="1" if disabled else "0")
            result = subprocess.run([str(binary)], capture_output=True, text=True, env=env, timeout=40)
            passed = result.returncode == 0 and result.stdout.strip() == "site-cache-ownership-and-collisions-ok"
            records.append(dict(arch=arch, cache=not disabled, exit_code=result.returncode,
                                stdout=result.stdout, stderr=result.stderr, passed=passed))
            print(arch, "cache-off" if disabled else "cache-on", "PASS" if passed else "FAIL", flush=True)
    (output / "receipt.json").write_text(json.dumps(records, indent=2) + "\n", encoding="utf-8")
    if not all(row["passed"] for row in records):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
