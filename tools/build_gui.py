"""Build the independent Win32 GUI with a verified current MySQL SFX beside it."""

import argparse
import json
from pathlib import Path
import shutil
import subprocess

from bootstrap import sha256

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / ".local/toolchains/llvm-mingw-20261104-msvcrt-x86_64/bin"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mysql-exe", type=Path, default=ROOT / "build/mysql-standalone/mysql-standalone.exe")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/gui")
    args = parser.parse_args()
    mysql = args.mysql_exe.resolve(strict=True)
    receipt = json.loads((mysql.parent / "build-receipt.json").read_text(encoding="utf-8"))
    sources = ["delivery/mysql_standalone.cpp", "delivery/mysql_operations.h", "delivery/win32_files.h"]
    if (sha256(mysql) != receipt["output_sha256"] or
            receipt.get("source_files") != {name: sha256(ROOT / name) for name in sources}):
        raise SystemExit("Rebuild the MySQL SFX from current sources before building the GUI")
    work = args.build_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    manifest = ROOT / "delivery/controller_gui.manifest"
    resource = work / "gui.rc"
    resource.write_text('1 24 "' + manifest.as_posix() + '"\n', encoding="ascii")
    obj = work / "gui-resource.o"
    subprocess.run([str(BIN / "x86_64-w64-mingw32-windres.exe"), "-i", str(resource), "-o", str(obj)], check=True)
    output = work / "DNF-Native.exe"
    source = ROOT / "delivery/controller_gui.cpp"
    command = [str(BIN / "x86_64-w64-mingw32-clang++.exe"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
               "-O2", "-static", "-municode", "-mwindows", "-DUNICODE", "-D_UNICODE", "-D_WIN32_WINNT=0x0601",
               str(source), str(obj), "-luser32", "-lgdi32", "-lshell32", "-lole32", "-ladvapi32", "-lws2_32", "-lbcrypt", "-o", str(output)]
    subprocess.run(command, check=True, cwd=ROOT)
    target = work / "mysql-standalone.exe"
    if mysql != target:
        shutil.copy2(mysql, target)
    record = {"command": command, "output_bytes": output.stat().st_size, "output_sha256": sha256(output),
              "sources": {str(p.relative_to(ROOT)).replace("\\", "/"): sha256(p)
                          for p in [source, manifest, ROOT / "delivery/win32_files.h", ROOT / "delivery/service_processes.h", ROOT / "delivery/instance_setup.h", Path(__file__)]},
              "mysql_sha256": sha256(target), "mysql_build_receipt_sha256": sha256(mysql.parent / "build-receipt.json"),
              "scope": "Native database and ELF process GUI build; not gameplay or clean multi-OS acceptance."}
    (work / "build-receipt.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print("Native GUI:", output, output.stat().st_size, "bytes", flush=True)


if __name__ == "__main__":
    main()
