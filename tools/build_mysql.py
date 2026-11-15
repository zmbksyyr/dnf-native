"""Stage the pinned official MySQL ZIP and build the instance maintenance helper."""

import argparse
import json
from pathlib import Path
import subprocess
import zipfile

from bootstrap import external_output, sha256, TOOLCHAIN, BUILD
from resource_baseline import RESOURCES, verify_baseline

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resources", type=Path, default=RESOURCES)
    parser.add_argument("--build-dir", type=Path, default=BUILD / "mysql-tools")
    args = parser.parse_args()
    baseline = verify_baseline(args.resources)
    work = external_output(args.build_dir)
    payload = work / "mysql"
    payload.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.resources / "mysql.zip") as archive:
        archive.extractall(payload)
    files = {p.relative_to(payload).as_posix(): {"bytes": p.stat().st_size, "sha256": sha256(p)}
             for p in sorted(payload.rglob("*")) if p.is_file()}
    (work / "manifest.json").write_text(json.dumps(files, indent=2) + "\n", encoding="utf-8")
    (work / "runtime").mkdir(exist_ok=True)
    output = work / "runtime/mysql-control.exe"
    source = ROOT / "src/database/mysql_control.cpp"
    command = [str(TOOLCHAIN / "x86_64-w64-mingw32-clang++.exe"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
               "-O2", "-static", "-municode", "-DUNICODE", "-D_UNICODE", "-D_WIN32_WINNT=0x0601",
               str(source), "-lbcrypt", "-lshell32", "-ladvapi32", "-o", str(output)]
    subprocess.run(command, check=True, cwd=ROOT)
    sources = [source, ROOT / "src/database/mysql_operations.h", ROOT / "src/win32_files.h"]
    receipt = {"files": files, "output_bytes": output.stat().st_size, "output_sha256": sha256(output),
               "source_files": {p.relative_to(ROOT).as_posix(): sha256(p) for p in sources},
               "command": command, "resource_baseline": baseline["files"]["mysql.zip"],
               "scope": "Unchanged official MySQL payload and rebuilt instance helper; runtime checks recorded separately."}
    (work / "build-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print("MySQL tools:", output, len(files), "verified files", flush=True)


if __name__ == "__main__":
    main()
