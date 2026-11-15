"""Build a visible ZIP resource package from the verified original resource snapshot."""

import argparse
import configparser
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile

from audit_resources import elf_metadata
from bootstrap import sha256

ROOT = Path(__file__).resolve().parents[1]


def retained_files(files, objects):
    """Keep original services/plugins and their Linux dependencies; omit deployment/debug data."""
    kept = set()
    removed = {}
    for name in files:
        if name.startswith("hosts/"):
            continue
        if name.startswith(("i686/lib/", "i686/usr/", "x86_64/lib64/", "x86_64/usr/")):
            continue
        if name.endswith(("/test1.lua", "/test2.lua", "/test.lua", "/README.md")):
            removed[name] = "Plugin example/test or documentation"
        elif name.endswith(("/auction_restart", "/guild_restart", "/gunnersvs")):
            removed[name] = "Linux shell launcher superseded by the native controller"
        else:
            kept.add(name)

    gconv = {"BIG5", "BIG5HKSCS", "GBBIG5", "GBGBK", "GBK", "GB18030", "EUC-KR", "UHC",
             "CP1252", "ISO8859-1", "UTF-16", "UTF-32", "UNICODE", "UNICODE2", "ISO2022KR", "HZ"}
    for arch in ("i686", "x86_64"):
        library_paths = [p for p in files if p.startswith(arch + "/") and
                         (p.startswith(arch + "/lib") or p.startswith(arch + "/usr/lib")) and
                         ".so" in Path(p).name and "/audit/" not in p]
        by_name = {}
        for name in library_paths:
            by_name.setdefault(Path(name).name, []).append(name)
        needed = {"ld-2.17.so", "ld-linux.so.2" if arch == "i686" else "ld-linux-x86-64.so.2",
                  "libnss_files.so.2", "libnss_dns.so.2", "libresolv.so.2"}
        # dlopen users are outside DT_NEEDED: NSS/crypt and original plugin modules.
        roots = [p for p in kept if p.startswith(arch + "/")]
        if arch == "i686":
            for p in files:
                if p.startswith("i686/usr/lib/gconv/") and (Path(p).stem in gconv or Path(p).name.startswith("gconv-modules")):
                    kept.add(p); roots.append(p)
        scanned = set()
        pending = list(roots)
        while pending or needed:
            while needed:
                name = needed.pop()
                for p in by_name.get(name, []):
                    if p not in kept:
                        kept.add(p); pending.append(p)
            if not pending:
                break
            p = pending.pop()
            if p in scanned:
                continue
            scanned.add(p)
            data = (objects / files[p]).read_bytes()
            if not data.startswith(b"\x7fELF"):
                continue
            metadata = elf_metadata(data)
            needed.update(n for n in metadata["needed"] if not all(x in scanned for x in by_name.get(n, [])))
        # SONAME aliases and their real filenames are both retained at original paths.
        digests = {files[p] for p in kept if p in library_paths}
        kept.update(p for p in library_paths if files[p] in digests)

    for name in files:
        if name in kept or name.startswith("hosts/"):
            continue
        if name.startswith("i686/usr/local/share/GeoIP/"):
            kept.add(name)
        else:
            removed.setdefault(name, "Unused Linux library, encoding module, debugger or system-deployment support")
    return sorted(kept), removed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True, help="Verified v2 CAB resource directory (read only)")
    parser.add_argument("--gui-dir", type=Path, required=True)
    parser.add_argument("--hosts", type=Path, required=True, help="Previous compatibility directory with both host architectures")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source, gui, hosts, output = (p.resolve() for p in (args.source, args.gui_dir, args.hosts, args.output))
    old = configparser.ConfigParser(interpolation=None)
    old.read(source / "resources.ini", encoding="utf-16")
    if sha256(source / "resources.cab") != old["resources"]["cabinet_sha256"]:
        raise SystemExit("Source cabinet hash differs")
    work = output.parent / (output.name + "-work")
    objects = work / "objects"
    objects.mkdir(parents=True, exist_ok=True)
    if not any(objects.iterdir()):
        subprocess.run(["expand.exe", "-F:*", str(source / "resources.cab"), str(objects)],
                       check=True, stdout=subprocess.DEVNULL, timeout=180)
    files = {old["file" + str(n)]["path"]: old["file" + str(n)]["sha256"]
             for n in range(old.getint("resources", "count"))}
    for digest in set(files.values()):
        if sha256(objects / digest) != digest:
            raise SystemExit("Source object hash differs")
    kept, removed = retained_files(files, objects)
    closure = {}
    for arch in ("i686", "x86_64"):
        paths = [p for p in kept if p.startswith(arch + "/")]
        available = {Path(p).name for p in paths}
        dependencies = {}
        for path in paths:
            data = (objects / files[path]).read_bytes()
            if data.startswith(b"\x7fELF"):
                dependencies[path] = elf_metadata(data)["needed"]
        missing = {p: [n for n in needs if n not in available] for p, needs in dependencies.items()
                   if any(n not in available for n in needs)}
        if missing:
            raise SystemExit("Native ELF dependency is missing: " + json.dumps(missing))
        closure[arch] = {"elf_count": len(dependencies), "needed": dependencies, "missing": missing}
    output.mkdir(parents=True, exist_ok=False)
    resources = output / "resources"
    resources.mkdir()
    shutil.copy2(gui / "DNF-Native.exe", output / "DNF-Native.exe")
    shutil.copy2(ROOT / "delivery/README.txt", resources / "README.txt")
    shutil.copy2(gui / "runtime/mysql-control.exe", resources / "mysql-control.exe")
    for arch, label in [("i686", "x86"), ("x86_64", "x64")]:
        shutil.copy2(hosts / "hosts" / arch / "dnf-host.exe", resources / ("dnf-host-" + label + ".exe"))
    with zipfile.ZipFile(resources / "mysql.zip", "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for path in sorted((gui / "mysql").rglob("*")):
            if path.is_file(): z.write(path, path.relative_to(gui / "mysql").as_posix())
    ini = configparser.ConfigParser(interpolation=None)
    ini["resources"] = {"version": "3", "count": str(len(kept)), "seed_sha256": old["resources"]["seed_sha256"]}
    with zipfile.ZipFile(resources / "native.zip", "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for n, name in enumerate(kept):
            data = (objects / files[name]).read_bytes()
            if name == "services.template.ini":
                template = configparser.ConfigParser(interpolation=None)
                template.read_string(data.decode("utf-16"))
                for section in template.sections():
                    if "host" in template[section]:
                        template[section]["host"] = "dnf-host-x64.exe" if template[section]["root"] == "x86_64" else "dnf-host-x86.exe"
                import io
                text = io.StringIO(); template.write(text); data = text.getvalue().encode("utf-16")
            z.writestr(name, data)
            ini["file" + str(n)] = {"path": name, "sha256": hashlib.sha256(data).hexdigest()}
    ini["package"] = {name: sha256(resources / name) for name in
                      ["mysql.zip", "native.zip", "dnf-host-x86.exe", "dnf-host-x64.exe", "mysql-control.exe"]}
    with (resources / "resources.ini").open("w", encoding="utf-16") as stream:
        ini.write(stream)
    third_party = resources / "licenses"
    third_party.mkdir()
    for path in (ROOT / "LICENSES").glob("*.txt"):
        shutil.copy2(path, third_party / path.name)
    shutil.copy2(ROOT / "THIRD_PARTY.md", third_party / "THIRD_PARTY.md")
    # Preserve the imported license materials without carrying debugger/deployment docs.
    for group in ("glibc-2.17", "libgcc-4.8.5", "zlib-1.2.7"):
        for path in (ROOT / ".local/sysroot/i686/usr/share/doc" / group).iterdir():
            if path.is_file() and (path.name.startswith("COPYING") or path.name in ("LICENSES", "README")):
                target = third_party / "linux" / group / path.name
                target.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(path, target)
    record = {"original_files": len(files), "retained_files": len(kept), "removed_files": removed,
              "source_sha256": sha256(source / "resources.cab"), "elf_dependency_closure": closure,
              "package_files": {p.relative_to(output).as_posix(): sha256(p) for p in output.rglob("*") if p.is_file()},
              "builder_sha256": sha256(Path(__file__)), "scope": "Resource selection and ZIP construction; runtime validation recorded separately."}
    (work / "receipt.json").write_text(json.dumps(record, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")
    print("Instance package:", output, "native files", len(files), "->", len(kept), "removed", len(removed), flush=True)


if __name__ == "__main__":
    main()
