"""Import pinned CentOS 7 user libraries from upstream metadata, never execute RPM scripts."""

import gzip
import hashlib
import json
import lzma
from pathlib import Path, PurePosixPath
import posixpath
import struct
import urllib.request
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
BASE = "https://vault.centos.org/7.9.2009/os/x86_64/"
PRIMARY = "repodata/2b479c0f3efa73f75b7fb76c82687744275fff78e4a138b5b3efba95f91e099e-primary.xml.gz"
PRIMARY_SHA256 = "2b479c0f3efa73f75b7fb76c82687744275fff78e4a138b5b3efba95f91e099e"


def fetch(relative, expected):
    target = ROOT / ".local/downloads" / PurePosixPath(relative).name
    target.parent.mkdir(parents=True, exist_ok=True)
    if not target.exists() or hashlib.sha256(target.read_bytes()).hexdigest() != expected:
        print(f"Downloading {relative}", flush=True)
        with urllib.request.urlopen(BASE + relative, timeout=45) as source:
            content = source.read()
        if hashlib.sha256(content).hexdigest() != expected:
            raise ValueError("Upstream metadata/package checksum mismatch")
        target.write_bytes(content)
    return target.read_bytes()


def header_end(data, offset):
    if data[offset:offset + 4] != b"\x8e\xad\xe8\x01":
        raise ValueError("Invalid RPM header")
    count, size = struct.unpack_from(">II", data, offset + 8)
    end = offset + 16 + count * 16 + size
    if end > len(data):
        raise ValueError("RPM header outside file")
    return end


def unpack_rpm(data, destination):
    if data[:4] != b"\xed\xab\xee\xdb":
        raise ValueError("Invalid RPM lead")
    offset = (header_end(data, 96) + 7) & ~7
    payload = data[header_end(data, offset):]
    content = lzma.decompress(payload) if payload.startswith(b"\xfd7zXZ") else gzip.decompress(payload)
    offset, links, hardlinks = 0, {}, {}
    while offset < len(content):
        header = content[offset:offset + 110]
        if header[:6] not in (b"070701", b"070702"):
            raise ValueError("Unsupported CPIO payload")
        fields = [int(header[6 + 8 * i:14 + 8 * i], 16) for i in range(13)]
        inode, mode, _, _, nlink, _, size, major, minor, _, _, namesize, _ = fields
        start = offset + 110
        name = content[start:start + namesize - 1].decode("utf-8")
        offset = (start + namesize + 3) & ~3
        value = content[offset:offset + size]
        offset = (offset + size + 3) & ~3
        if name == "TRAILER!!!":
            break
        name = posixpath.normpath(name.removeprefix("./"))
        target = (destination / name).resolve()
        if not target.is_relative_to(destination.resolve()):
            raise ValueError("RPM path outside destination")
        typ = mode & 0o170000
        if typ == 0o040000:
            target.mkdir(parents=True, exist_ok=True)
        elif typ == 0o120000:
            links[name] = value.decode("utf-8")
        elif typ == 0o100000:
            target.parent.mkdir(parents=True, exist_ok=True)
            if nlink > 1:
                hardlinks.setdefault((major, minor, inode), []).append((target, value))
            else:
                target.write_bytes(value)
        else:
            raise ValueError(f"Unexpected RPM file type: {name}")
    for paths in hardlinks.values():
        value = next((v for _, v in paths if v), b"")
        for path, _ in paths:
            path.write_bytes(value)
    return links


def main():
    metadata = ET.fromstring(gzip.decompress(fetch(PRIMARY, PRIMARY_SHA256)))
    ns = {"m": "http://linux.duke.edu/metadata/common"}
    wanted = {("glibc", "2.17", "317.el7"), ("libstdc++", "4.8.5", "44.el7"),
              ("libgcc", "4.8.5", "44.el7"), ("zlib", "1.2.7", "18.el7"),
              ("nss-softokn-freebl", "3.44.0", "8.el7_7")}
    packages = []
    for package in metadata:
        name = package.findtext("m:name", namespaces=ns)
        arch = package.findtext("m:arch", namespaces=ns)
        version = package.find("m:version", ns).attrib
        if (name, version["ver"], version["rel"]) not in wanted or arch not in ("i686", "x86_64"):
            continue
        checksum = package.find("m:checksum", ns)
        if checksum.attrib["type"] != "sha256":
            raise ValueError("Expected upstream SHA-256 package checksum")
        relative = package.find("m:location", ns).attrib["href"]
        packages.append({"name": name, "arch": arch, "version": version["ver"] + "-" + version["rel"],
                         "url": BASE + relative, "sha256": checksum.text})
    if len(packages) != 10:
        raise ValueError("Expected exactly ten pinned user-library packages")
    links_by_arch = {"i686": {}, "x86_64": {}}
    for package in packages:
        arch = package["arch"]
        destination = ROOT / ".local/sysroot" / arch
        links_by_arch[arch].update(unpack_rpm(fetch(package["url"][len(BASE):], package["sha256"]), destination))
    for arch, links in links_by_arch.items():
        destination = ROOT / ".local/sysroot" / arch
        pending = dict(links)
        for _ in range(len(links) + 1):
            progress = False
            for name, link in list(pending.items()):
                resolved = posixpath.normpath(link.lstrip("/") if link.startswith("/") else posixpath.join(posixpath.dirname(name), link))
                source = (destination / resolved).resolve()
                if not source.is_relative_to(destination.resolve()):
                    raise ValueError("RPM symlink outside guest root")
                if source.is_file():
                    target = destination / name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(source.read_bytes())
                    del pending[name]
                    progress = True
            if not progress:
                break
        (destination / "links.json").write_text(json.dumps(links, indent=2) + "\n", encoding="utf-8")
        print(f"{arch}: imported five packages; {len(pending)} aliases target optional packages", flush=True)
    receipt = {"source": BASE, "metadata_sha256": PRIMARY_SHA256, "packages": packages,
               "note": "RPM scripts not executed. File aliases are development copies; full guest symlink semantics not implemented."}
    (ROOT / ".local/linux-libraries.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
