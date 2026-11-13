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
    sources = sorted((ROOT / "runtime").glob("*.c")) + sorted((ROOT / "runtime").glob("*.S"))
    decoder = ROOT / ".local/sources/zydis-4.1.1"
    decoder_names = ["Decoder", "DecoderData", "Encoder", "EncoderData", "MetaInfo", "Mnemonic", "Register", "SharedData", "Utils", "Zydis"]
    dependency_sources = [decoder / "src" / (name + ".c") for name in decoder_names]
    records = []
    for arch, target in [("i686", "i686-w64-mingw32"), ("x86_64", "x86_64-w64-mingw32")]:
        compiler = TOOLCHAIN / (target + "-clang.exe")
        if not compiler.is_file():
            raise SystemExit("Run python tools/bootstrap.py first")
        output = ROOT / "build" / arch / "dnf-host.exe"
        output.parent.mkdir(parents=True, exist_ok=True)
        command = [str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", "-g",
                   "-D_WIN32_WINNT=0x0601", "-DUNICODE", "-D_UNICODE", "-municode", "-static",
                   "-DZYDIS_STATIC_BUILD", "-DZYCORE_STATIC_BUILD", "-DZYDIS_DISABLE_FORMATTER",
                   "-I", str(decoder / "include"), "-I", str(decoder / "src"),
                   "-I", str(decoder / "dependencies/zycore/include"),
                   *map(str, sources + dependency_sources), "-lbcrypt", "-lws2_32", "-o", str(output)]
        if arch == "i686":
            command.append("-Wl,--large-address-aware")
        subprocess.run(command, env=env, check=True, cwd=ROOT)
        record = {"arch": arch, "command": command,
                  "compiler_version": subprocess.check_output([str(compiler), "--version"], env=env, text=True),
                  "sources": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                              for p in sorted((ROOT / "runtime").iterdir()) if p.is_file()},
                  "decoder_commit": "a2278f1d254e492f6a6b39f6cb5d1f5d515659dc",
                  "output_sha256": hashlib.sha256(output.read_bytes()).hexdigest()}
        records.append(record)
        print(output, flush=True)
    (ROOT / "build/build-receipt.json").write_text(json.dumps(records, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
