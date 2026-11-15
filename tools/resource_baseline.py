"""Read the repository's pinned resource inputs without changing them."""

import collections
import json
import struct
from pathlib import Path

from bootstrap import sha256

ROOT = Path(__file__).resolve().parents[1]
RESOURCES = ROOT / "resources"


def verify_baseline(folder=RESOURCES):
    folder = Path(folder).resolve(strict=True)
    record = json.loads((folder / "baseline.json").read_text(encoding="utf-8"))
    for name in ("native.zip", "mysql.zip", "baseline.ini"):
        metadata = record["files"][name]
        path = folder / name
        if path.stat().st_size != metadata["bytes"] or sha256(path) != metadata["sha256"]:
            raise ValueError("Resource baseline differs: " + name)
    return record


def elf_dependencies(data):
    """Read direct DT_NEEDED names from the pinned little-endian ELF inputs."""
    bits = {1: 32, 2: 64}[data[4]]
    if data[5] != 1:
        raise ValueError("Only little-endian input supported")
    wide = bits == 64
    eh = struct.unpack_from("<HHIQQQIHHHHHH" if wide else "<HHIIIIIHHHHHH", data, 16)
    phoff = eh[4]
    phentsize, phnum = eh[8:10]
    headers = []
    for index in range(phnum):
        ph = struct.unpack_from("<IIQQQQQQ" if wide else "<IIIIIIII", data, phoff + index * phentsize)
        if wide:
            typ, _, offset, address, _, filesz, _, _ = ph
        else:
            typ, offset, address, _, filesz, _, _, _ = ph
        headers.append(dict(type=typ, offset=offset, address=address, filesz=filesz))

    def offset(address):
        for ph in headers:
            if ph["type"] == 1 and ph["address"] <= address < ph["address"] + ph["filesz"]:
                return address - ph["address"] + ph["offset"]
        raise ValueError(f"Unmapped ELF virtual address {address:x}")

    dynamic = collections.defaultdict(list)
    for ph in headers:
        if ph["type"] != 2:
            continue
        width = 16 if wide else 8
        for pos in range(ph["offset"], ph["offset"] + ph["filesz"], width):
            tag, value = struct.unpack_from("<qQ" if wide else "<iI", data, pos)
            if tag == 0:
                break
            dynamic[tag].append(value)
    strings = offset(dynamic[5][0]) if 5 in dynamic else 0

    def string(index):
        pos = strings + index
        return data[pos:data.index(b"\0", pos)].decode("utf-8", "replace")

    return [string(value) for value in dynamic.get(1, [])]
