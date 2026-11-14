"""Compress verified local resources once per content hash using Windows LZX."""

import argparse
import configparser
import datetime
import json
from pathlib import Path
import shutil
import subprocess

from bootstrap import sha256

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resources", type=Path, required=True, help="Verified uncompressed version-1 resource directory")
    parser.add_argument("--output", type=Path, required=True, help="New compressed resources directory")
    args = parser.parse_args()
    source = args.resources.resolve(strict=True)
    ini = configparser.ConfigParser(interpolation=None)
    ini.read(source / "resources.ini", encoding="utf-16")
    if ini["resources"]["version"] != "1":
        raise SystemExit("Input must be the uncompressed version-1 resources")
    unique = {}
    for index in range(ini.getint("resources", "count")):
        entry = ini["file" + str(index)]
        path = (source / entry["path"]).resolve(strict=True)
        if not path.is_relative_to(source) or sha256(path) != entry["sha256"]:
            raise SystemExit("Local resource identity differs: " + entry["path"])
        unique.setdefault(entry["sha256"], path)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    work = output.parent / (output.name + "-cab-build")
    work.mkdir(exist_ok=False)
    if any(not str(path).isascii() for path in [work, *unique.values()]):
        raise SystemExit("The developer makecab source/work paths currently require ASCII")
    ddf = [".OPTION EXPLICIT", ".Set Cabinet=on", ".Set Compress=on", ".Set CompressionType=LZX",
           ".Set CompressionMemory=21", ".Set MaxDiskSize=0", ".Set MaxCabinetSize=0",
           ".Set CabinetNameTemplate=resources.cab", ".Set DiskDirectoryTemplate=.",
           ".Set RptFileName=NUL", ".Set InfFileName=NUL"]
    ddf += ['"' + str(path) + '" "' + digest + '"' for digest, path in unique.items()]
    (work / "resources.ddf").write_text("\n".join(ddf) + "\n", encoding="ascii")
    result = subprocess.run(["makecab.exe", "/F", str(work / "resources.ddf")], cwd=work, capture_output=True, timeout=600)
    (work / "makecab.log").write_bytes(result.stdout + result.stderr)
    if result.returncode:
        raise SystemExit("Resource cabinet build failed; build log retained")
    cabinet = output / "resources.cab"
    shutil.move(work / "resources.cab", cabinet)
    ini["resources"]["version"] = "2"
    ini["resources"]["cabinet_sha256"] = sha256(cabinet)
    with (output / "resources.ini").open("w", encoding="utf-16") as stream:
        ini.write(stream)
    record = {"recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "scope": "All local resource bytes retained, exact duplicate objects stored once; native extraction and release acceptance recorded separately.",
              "file_count": ini.getint("resources", "count"), "unique_objects": len(unique),
              "cabinet_bytes": cabinet.stat().st_size, "cabinet_sha256": sha256(cabinet),
              "manifest_sha256": sha256(output / "resources.ini"),
              "source_manifest_sha256": sha256(source / "resources.ini"),
              "builder_sha256": sha256(Path(__file__))}
    (work / "receipt.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print("Local resource cabinet:", cabinet, cabinet.stat().st_size, "bytes;", len(unique), "unique objects", flush=True)


if __name__ == "__main__":
    main()
