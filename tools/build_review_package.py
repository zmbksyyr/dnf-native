"""Create the GUI / mysql / runtime / native directory package from verified builds."""

import argparse
import configparser
import datetime
import json
from pathlib import Path
import shutil
import subprocess
import zipfile

from bootstrap import sha256

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gui-dir", type=Path, required=True)
    parser.add_argument("--resources", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    gui, source, output = args.gui_dir.resolve(), args.resources.resolve(), args.output.resolve()
    receipt = json.loads((gui / "build-receipt.json").read_text(encoding="utf-8"))
    if sha256(gui / "DNF-Native.exe") != receipt["output_sha256"]:
        raise SystemExit("GUI identity differs")
    ini = configparser.ConfigParser(interpolation=None)
    ini.read(source / "resources.ini", encoding="utf-16")
    if ini["resources"]["version"] != "2" or sha256(source / "resources.cab") != ini["resources"]["cabinet_sha256"]:
        raise SystemExit("Verified source cabinet required")
    output.mkdir(parents=True, exist_ok=False)
    work = output.parent / (output.name + "-cab-build")
    work.mkdir(exist_ok=False)
    objects = work / "objects"
    objects.mkdir()
    result = subprocess.run(["expand.exe", "-F:*", str(source / "resources.cab"), str(objects)], capture_output=True, timeout=180)
    (work / "expand.log").write_bytes(result.stdout + result.stderr)
    result.check_returncode()
    shutil.copy2(gui / "DNF-Native.exe", output / "DNF-Native.exe")
    for name in ["mysql", "runtime"]:
        shutil.copytree(gui / name, output / name)
    entries = []
    hosts = {}
    for index in range(ini.getint("resources", "count")):
        entry = dict(ini["file" + str(index)])
        obj = objects / entry["sha256"]
        if sha256(obj) != entry["sha256"]:
            raise SystemExit("Extracted source identity differs: " + entry["path"])
        if entry["path"].startswith("hosts/"):
            target = output / "runtime" / entry["path"]
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(obj, target)
            hosts[entry["path"]] = entry["sha256"]
        else:
            entries.append(entry)
    if set(hosts) != {"hosts/i686/dnf-host.exe", "hosts/x86_64/dnf-host.exe"}:
        raise SystemExit("Both project host architectures are required")
    native = output / "native"
    native.mkdir()
    hashes = sorted({entry["sha256"] for entry in entries})
    ddf = [".OPTION EXPLICIT", ".Set Cabinet=on", ".Set Compress=on", ".Set CompressionType=LZX",
           ".Set CompressionMemory=21", ".Set MaxDiskSize=0", ".Set MaxCabinetSize=0",
           ".Set CabinetNameTemplate=resources.cab", ".Set DiskDirectoryTemplate=.",
           ".Set RptFileName=NUL", ".Set InfFileName=NUL"]
    ddf += ['"' + str(objects / digest) + '" "' + digest + '"' for digest in hashes]
    (work / "resources.ddf").write_text("\n".join(ddf) + "\n", encoding="ascii")
    result = subprocess.run(["makecab.exe", "/F", str(work / "resources.ddf")], cwd=work, capture_output=True, timeout=600)
    (work / "makecab.log").write_bytes(result.stdout + result.stderr)
    result.check_returncode()
    shutil.move(work / "resources.cab", native / "resources.cab")
    manifest = configparser.ConfigParser(interpolation=None)
    manifest["resources"] = {"version": "2", "count": str(len(entries)),
        "seed_sha256": ini["resources"]["seed_sha256"], "cabinet_sha256": sha256(native / "resources.cab")}
    for index, entry in enumerate(entries):
        manifest["file" + str(index)] = entry
    with (native / "resources.ini").open("w", encoding="utf-16") as stream:
        manifest.write(stream)
    official = json.loads((ROOT / ".local/mysql-import.json").read_text(encoding="utf-8"))
    official_exes = {}
    for name in ["mysqld", "mysql", "mysqladmin", "mysqldump"]:
        relative = "bin/" + name + ".exe"
        digest = sha256(output / "mysql" / relative)
        if digest != official["payload"][relative]["sha256"]:
            raise SystemExit("Official MySQL executable identity differs")
        official_exes[relative] = digest
    files = {path.relative_to(output).as_posix(): {"bytes": path.stat().st_size, "sha256": sha256(path)}
             for path in sorted(output.rglob("*")) if path.is_file()}
    archive = output.with_suffix(".zip")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as package:
        for name in files:
            package.write(output / name, name)
    record = {"recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "scope": "Review directory package; original resource contents retained except project hosts moved to runtime. Validation receipts recorded separately.",
        "files": files, "official_exes": official_exes, "hosts": hosts,
        "archive_bytes": archive.stat().st_size, "archive_sha256": sha256(archive),
        "builder_sha256": sha256(Path(__file__)), "source_manifest_sha256": sha256(source / "resources.ini")}
    output.with_suffix(".receipt.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print("Review package:", output, archive.stat().st_size, "compressed bytes", flush=True)


if __name__ == "__main__":
    main()
