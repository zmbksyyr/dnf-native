"""Stage official MySQL tools unchanged and build the instance maintenance helper."""

import argparse
import json
from pathlib import Path
import shutil
import subprocess

from bootstrap import sha256
from import_mysql import SHA256 as MYSQL_ZIP_SHA256

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / ".local/toolchains/llvm-mingw-20261104-msvcrt-x86_64/bin"
TOOLS = ["mysqld.exe", "mysql.exe", "mysqladmin.exe", "mysqldump.exe"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/mysql-tools")
    args = parser.parse_args()
    official = json.loads((ROOT / ".local/mysql-import.json").read_text(encoding="utf-8"))
    runtime = json.loads((ROOT / ".local/mysql-runtime-import.json").read_text(encoding="utf-8"))
    if not official["passed"] or official["archive_sha256"] != MYSQL_ZIP_SHA256 or not runtime["passed"]:
        raise ValueError("Run the official MySQL and Microsoft runtime imports first")
    work = args.build_dir.resolve()
    payload = work / "mysql"
    payload.mkdir(parents=True, exist_ok=True)
    files = {}

    def add(source, name, expected):
        if sha256(source) != expected:
            raise ValueError("Imported payload differs: " + str(source))
        target = payload / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        files[name] = {"bytes": target.stat().st_size, "sha256": expected}

    for name, metadata in official["payload"].items():
        if name in ["bin/" + tool for tool in TOOLS] or name.startswith("share/") or name in ["LICENSE", "README"]:
            add(ROOT / ".local/mysql-5.7.44" / name, name, metadata["sha256"])
    for name, metadata in runtime["files"].items():
        add(ROOT / ".local/mysql-runtime-x64" / name,
            ("bin/" if name.endswith(".dll") else "licenses/") + name, metadata["sha256"])
    files = dict(sorted(files.items()))
    (work / "manifest.json").write_text(json.dumps(files, indent=2) + "\n", encoding="utf-8")
    (work / "runtime").mkdir(exist_ok=True)
    output = work / "runtime/mysql-control.exe"
    source = ROOT / "delivery/mysql_control.cpp"
    command = [str(BIN / "x86_64-w64-mingw32-clang++.exe"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
               "-O2", "-static", "-municode", "-DUNICODE", "-D_UNICODE", "-D_WIN32_WINNT=0x0601",
               str(source), "-lbcrypt", "-lshell32", "-ladvapi32", "-o", str(output)]
    subprocess.run(command, check=True, cwd=ROOT)
    receipt = {"files": files, "output_bytes": output.stat().st_size,
               "output_sha256": sha256(output), "source_sha256": sha256(source),
               "source_files": {str(path.relative_to(ROOT)).replace("\\", "/"): sha256(path)
                                for path in [source, ROOT / "delivery/mysql_operations.h", ROOT / "delivery/win32_files.h"]}, "command": command,
               "official_mysql_source": {key: official[key] for key in ["url", "archive_sha256"]},
               "microsoft_runtime_sources": runtime["sources"],
               "scope": "Official EXEs and app-local Microsoft runtimes in a directory; helper only manages instance operations. No clean three-OS acceptance."}
    (work / "build-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print("MySQL tools:", output, output.stat().st_size, "bytes;", len(files), "verified files", flush=True)


if __name__ == "__main__":
    main()
