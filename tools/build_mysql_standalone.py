"""Build an independent Win32 SFX containing official MySQL and app-local runtimes."""

import argparse
import hashlib
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
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/mysql-standalone")
    args = parser.parse_args()
    official = json.loads((ROOT / ".local/mysql-import.json").read_text(encoding="utf-8"))
    runtime = json.loads((ROOT / ".local/mysql-runtime-import.json").read_text(encoding="utf-8"))
    if not official["passed"] or official["archive_sha256"] != MYSQL_ZIP_SHA256 or not runtime["passed"]:
        raise ValueError("Run the official MySQL and Microsoft runtime imports first")
    work = args.build_dir.resolve()
    payload = work / "payload"
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
    manifest = json.dumps(files, sort_keys=True, separators=(",", ":")).encode("ascii")
    payload_id = hashlib.sha256(manifest).hexdigest()
    (work / "manifest.json").write_text(json.dumps(files, indent=2) + "\n", encoding="utf-8")

    ddf = [".OPTION EXPLICIT", ".Set Cabinet=on", ".Set Compress=on", ".Set CompressionType=LZX",
           ".Set CompressionMemory=21", ".Set MaxDiskSize=0", ".Set MaxCabinetSize=0",
           ".Set CabinetNameTemplate=payload.cab", ".Set DiskDirectoryTemplate=.",
           ".Set RptFileName=NUL", ".Set InfFileName=NUL"]
    ddf += ['"' + str(payload / name) + '" "' + name.replace("/", "\\") + '"' for name in files]
    (work / "payload.ddf").write_text("\n".join(ddf) + "\n", encoding="utf-8")
    result = subprocess.run(["makecab.exe", "/F", str(work / "payload.ddf")], cwd=work, capture_output=True, timeout=180)
    (work / "makecab.log").write_bytes(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError("makecab failed; see build/mysql-standalone/makecab.log")
    cabinet = work / "payload.cab"
    header = ['static const char payload_id[] = "' + payload_id + '";',
              'static const char payload_cab_sha256[] = "' + sha256(cabinet) + '";',
              "static const Entry payload_files[] = {"]
    header += ['    {"' + name + '", ' + str(info["bytes"]) + ', "' + info["sha256"] + '"},'
               for name, info in files.items()]
    header += ["};"]
    (work / "payload_manifest.h").write_text("\n".join(header) + "\n", encoding="ascii")
    resource = work / "payload.rc"
    resource.write_text('101 RCDATA "' + cabinet.as_posix() + '"\n', encoding="ascii")
    resource_object = work / "payload-resource.o"
    subprocess.run([str(BIN / "x86_64-w64-mingw32-windres.exe"), "-i", str(resource), "-o", str(resource_object)],
                   check=True, cwd=ROOT)
    output = work / "mysql-standalone.exe"
    source = ROOT / "delivery/mysql_standalone.cpp"
    command = [str(BIN / "x86_64-w64-mingw32-clang++.exe"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
               "-O2", "-static", "-municode", "-DUNICODE", "-D_UNICODE", "-D_WIN32_WINNT=0x0601",
               "-I", str(work), str(source), str(resource_object), "-lcabinet", "-lbcrypt", "-lshell32", "-ladvapi32", "-o", str(output)]
    subprocess.run(command, check=True, cwd=ROOT)
    receipt = {"payload_id": payload_id, "files": files, "cabinet_bytes": cabinet.stat().st_size,
               "cabinet_sha256": sha256(cabinet), "output_bytes": output.stat().st_size,
               "output_sha256": sha256(output), "source_sha256": sha256(source), "command": command,
               "official_mysql_source": {key: official[key] for key in ["url", "archive_sha256"]},
               "microsoft_runtime_sources": runtime["sources"],
               "scope": "Build of independent SFX and official tool delegation; not final minimized payload or three-OS acceptance."}
    (work / "build-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print("MySQL SFX:", output, output.stat().st_size, "bytes;", len(files), "verified files", flush=True)


if __name__ == "__main__":
    main()
