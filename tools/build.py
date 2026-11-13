"""Build the independent x86/x64 Windows hosts with the pinned compiler."""

import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
TOOLCHAIN = ROOT / ".local/toolchains/llvm-mingw-20261104-msvcrt-x86_64/bin"


def main():
    env = dict(os.environ)
    env["PATH"] = str(TOOLCHAIN) + os.pathsep + env.get("PATH", "")
    sources = sorted((ROOT / "runtime").glob("*.c"))
    records = []
    for arch, target in [("i686", "i686-w64-mingw32"), ("x86_64", "x86_64-w64-mingw32")]:
        compiler = TOOLCHAIN / (target + "-clang.exe")
        if not compiler.is_file():
            raise SystemExit("Run python tools/bootstrap.py first")
        output = ROOT / "build" / arch / "dnf-host.exe"
        output.parent.mkdir(parents=True, exist_ok=True)
        command = [str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", "-g",
                   "-D_WIN32_WINNT=0x0601", "-DUNICODE", "-D_UNICODE", "-municode", "-static",
                   *map(str, sources), "-lbcrypt", "-o", str(output)]
        if arch == "i686":
            command.append("-Wl,--large-address-aware")
        subprocess.run(command, env=env, check=True, cwd=ROOT)
        record = {"arch": arch, "command": command,
                  "compiler_version": subprocess.check_output([str(compiler), "--version"], env=env, text=True),
                  "sources": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
                  "output_sha256": hashlib.sha256(output.read_bytes()).hexdigest()}
        records.append(record)
        print(output, flush=True)
    (ROOT / "build/build-receipt.json").write_text(json.dumps(records, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
