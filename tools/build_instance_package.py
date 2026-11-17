"""Package current builds with the repository's verified ZIP resource baseline."""

import argparse
import configparser
import hashlib
import json
from pathlib import Path
import shutil
import zipfile

from bootstrap import external_output, sha256, BUILD
from resource_baseline import RESOURCES, verify_baseline, elf_dependencies

ROOT = Path(__file__).resolve().parents[1]


def check_sources(sources):
    for name, expected in sources.items():
        if sha256(ROOT / name) != expected:
            raise ValueError("Build sources changed; rebuild before packaging: " + name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=RESOURCES, help="Pinned ZIP baseline directory")
    parser.add_argument("--gui-dir", type=Path, default=BUILD / "gui")
    parser.add_argument("--hosts", type=Path, default=BUILD, help="Current host build directory")
    parser.add_argument("--login-dir", type=Path, default=BUILD / "login")
    parser.add_argument("--robot-dir", type=Path, default=BUILD / "robot")
    parser.add_argument("--output", type=Path, default=BUILD / "package")
    args = parser.parse_args()
    source, gui, hosts, output = (p.resolve() for p in (args.source, args.gui_dir, args.hosts, args.output))
    output = external_output(output)
    baseline = verify_baseline(source)
    login = args.login_dir.resolve(strict=True)
    login_receipt = json.loads((login / "build-receipt.json").read_text(encoding="utf-8"))
    check_sources(login_receipt["sources"])
    if sha256(login / "DNFLogin.exe") != login_receipt["output_sha256"]:
        raise ValueError("Launcher differs from its build receipt")
    ini = configparser.ConfigParser(interpolation=None)
    ini.read(source / "baseline.ini", encoding="utf-16")
    files = {ini["file" + str(n)]["path"]: ini["file" + str(n)]["sha256"]
             for n in range(ini.getint("resources", "count"))}
    dependencies = {"i686": {}, "x86_64": {}}
    with zipfile.ZipFile(source / "native.zip") as archive:
        if archive.read("home/neople/game/publickey.pem") != (ROOT / "resources/login/publickey.pem").read_bytes():
            raise ValueError("Server default public key differs from the launcher key pair")
        if set(archive.namelist()) != set(files):
            raise ValueError("Native ZIP members differ from the baseline manifest")
        for name, expected in files.items():
            data = archive.read(name)
            if hashlib.sha256(data).hexdigest() != expected:
                raise ValueError("Native member hash differs: " + name)
            if data.startswith(b"\x7fELF"):
                dependencies["x86_64" if data[4] == 2 else "i686"][name] = elf_dependencies(data)
        for helper, entry in [("frida_item_reset.js", "dp2/df_game_r.js"),
                              ("dp2_cleanup.lua", "dp2/df_game_r.lua")]:
            code = (ROOT / "src/runtime" / helper).read_bytes().replace(b"\r\n", b"\n")
            if code not in archive.read(entry).replace(b"\r\n", b"\n"):
                raise ValueError("Resource baseline embeds a different helper: " + helper)
    closure = {}
    for arch, entries in dependencies.items():
        available = {Path(name).name for name in entries}
        missing = {name: [n for n in needed if n not in available] for name, needed in entries.items()
                   if any(n not in available for n in needed)}
        if missing:
            raise ValueError("Native ELF dependency is missing: " + json.dumps(missing))
        closure[arch] = {"elf_count": len(entries), "missing": missing}
    gui_receipt = json.loads((gui / "build-receipt.json").read_text(encoding="utf-8"))
    check_sources(gui_receipt["sources"])
    if sha256(gui / "DNF-Native.exe") != gui_receipt["output_sha256"]:
        raise ValueError("GUI differs from its build receipt")
    if sha256(gui / "runtime/mysql-control.exe") != gui_receipt["mysql_sha256"]:
        raise ValueError("Database helper differs from the GUI build receipt")
    robot = args.robot_dir.resolve(strict=True)
    robot_receipt = json.loads((robot / "build-receipt.json").read_text(encoding="utf-8"))
    for name, expected in robot_receipt["sources"].items():
        if sha256(Path(robot_receipt["source"]) / name) != expected:
            raise ValueError("Robot sources changed; rebuild before packaging: " + name)
    if sha256(robot / "robot.exe") != robot_receipt["output_sha256"]:
        raise ValueError("Robot differs from its build receipt")
    host_receipts = json.loads((hosts / "build-receipt.json").read_text(encoding="utf-8"))
    for arch in ("i686", "x86_64"):
        receipt = next(r for r in host_receipts if r["arch"] == arch)
        check_sources(receipt["sources"])
        if sha256(hosts / arch / "dnf-host.exe") != receipt["output_sha256"]:
            raise ValueError("Host differs from its build receipt: " + arch)
    output.mkdir(parents=True, exist_ok=False)
    resources = output / "resources"
    resources.mkdir()
    shutil.copy2(gui / "DNF-Native.exe", output / "DNF-Native.exe")
    shutil.copy2(gui / "runtime/mysql-control.exe", resources / "mysql-control.exe")
    for arch, label in [("i686", "x86"), ("x86_64", "x64")]:
        shutil.copy2(hosts / arch / "dnf-host.exe", resources / ("dnf-host-" + label + ".exe"))
    for name in ("native.zip", "mysql.zip"):
        shutil.copy2(source / name, resources / name)
    shutil.copy2(login / "DNFLogin.exe", resources / "DNFLogin.exe")
    shutil.copy2(robot / "robot.exe", resources / "robot.exe")
    ini["package"] = {name: sha256(resources / name) for name in
                      ["mysql.zip", "native.zip", "dnf-host-x86.exe", "dnf-host-x64.exe", "mysql-control.exe", "robot.exe"]}
    with (resources / "resources.ini").open("w", encoding="utf-16") as stream:
        ini.write(stream)
    record = {"baseline": baseline["files"], "elf_dependency_closure": closure,
              "package_files": {p.relative_to(output).as_posix(): sha256(p) for p in output.rglob("*") if p.is_file()},
              "builder_sha256": sha256(Path(__file__)),
              "scope": "Current source builds and pinned resource identity/dependencies; runtime checks recorded separately."}
    (output.parent / (output.name + "-receipt.json")).write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print("Instance package:", output, "native files", len(files), flush=True)


if __name__ == "__main__":
    main()
