"""Build the Win32 GUI with external official MySQL tools and a maintenance helper."""

import argparse
import json
from pathlib import Path
import shutil
import subprocess

from bootstrap import external_output, sha256, BUILD

from bootstrap import TOOLCHAIN

ROOT = Path(__file__).resolve().parents[1]
BIN = TOOLCHAIN


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mysql-dir", type=Path, default=BUILD / "mysql-tools")
    parser.add_argument("--build-dir", type=Path, default=BUILD / "gui")
    args = parser.parse_args()
    mysql_dir = args.mysql_dir.resolve(strict=True)
    mysql = mysql_dir / "runtime/mysql-control.exe"
    receipt = json.loads((mysql_dir / "build-receipt.json").read_text(encoding="utf-8"))
    sources = ["src/database/mysql_control.cpp", "src/database/mysql_operations.h", "src/win32_files.h"]
    if (sha256(mysql) != receipt["output_sha256"] or
            receipt.get("source_files") != {name: sha256(ROOT / name) for name in sources}):
        raise SystemExit("Rebuild the MySQL tools from current sources before building the GUI")
    work = external_output(args.build_dir)
    work.mkdir(parents=True, exist_ok=True)
    manifest = ROOT / "src/controller/controller_gui.manifest"
    resource = work / "gui.rc"
    resource.write_text('1 24 "' + manifest.as_posix() + '"\n', encoding="ascii")
    obj = work / "gui-resource.o"
    subprocess.run([str(BIN / "x86_64-w64-mingw32-windres.exe"), "-i", str(resource), "-o", str(obj)], check=True)
    output = work / "DNF-Native.exe"
    source = ROOT / "src/controller/controller_gui.cpp"
    zip_objects = []
    zip_defines = ["-DMINIZ_NO_STDIO", "-DMINIZ_NO_TIME", "-DMINIZ_NO_ZLIB_APIS", "-DMINIZ_NO_DEFLATE_APIS"]
    for name in ["miniz", "miniz_tinfl", "miniz_zip"]:
        target = work / (name + ".o")
        subprocess.run([str(BIN / "x86_64-w64-mingw32-clang.exe"), "-O2", *zip_defines,
                        "-c", str(ROOT / "src/third_party/miniz" / (name + ".c")), "-o", str(target)], check=True)
        zip_objects.append(str(target))
    command = [str(BIN / "x86_64-w64-mingw32-clang++.exe"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
               "-O2", "-static", "-municode", "-mwindows", "-DUNICODE", "-D_UNICODE", "-D_WIN32_WINNT=0x0601",
               str(source), str(obj), *zip_objects, *zip_defines, "-luser32", "-lgdi32", "-lshell32", "-lole32", "-ladvapi32", "-lws2_32", "-lbcrypt", "-luxtheme", "-lcomctl32", "-lpsapi", "-o", str(output)]
    subprocess.run(command, check=True, cwd=ROOT)
    for name in ["mysql", "runtime"]:
        if (mysql_dir / name).resolve() != (work / name).resolve():
            shutil.copytree(mysql_dir / name, work / name, dirs_exist_ok=True)
    target = work / "runtime/mysql-control.exe"
    record = {"command": command, "output_bytes": output.stat().st_size, "output_sha256": sha256(output),
              "sources": {str(p.relative_to(ROOT)).replace("\\", "/"): sha256(p)
                          for p in [source, manifest, ROOT / "src/win32_files.h", ROOT / "src/game_levels.h", *sorted((ROOT / "src/controller").glob("*.h")), Path(__file__)]},
              "mysql_sha256": sha256(target), "mysql_build_receipt_sha256": sha256(mysql_dir / "build-receipt.json"),
              "scope": "Native database and ELF process GUI build; not gameplay or clean multi-OS acceptance."}
    (work / "build-receipt.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print("Native GUI:", output, output.stat().st_size, "bytes", flush=True)


if __name__ == "__main__":
    main()
