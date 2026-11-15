"""Describe every pinned ELF offline; all generated reports belong outside Git."""

import argparse
from collections import Counter
import configparser
import hashlib
import json
from pathlib import Path
import struct
import zipfile

from bootstrap import BUILD, external_output, sha256
from resource_baseline import RESOURCES, verify_baseline


def fail(message: str) -> None:
    raise RuntimeError(message)


def read_c_string(data: bytes, offset: int, size: int | None = None) -> str:
    if offset < 0 or offset >= len(data):
        fail(f"ELF string offset out of bounds: 0x{offset:x}")
    end = len(data) if size is None else min(len(data), offset + size)
    end = data.find(b"\0", offset, end)
    if end < 0:
        fail(f"ELF string is not terminated at 0x{offset:x}")
    return data[offset:end].decode("utf-8", "replace")


def parse_elf(name: str, data: bytes, expected_hash: str | None) -> dict:
    if len(data) < 16 or data[:4] != b"\x7fELF":
        fail(f"not an ELF input: {name}")
    elf_class = {1: 32, 2: 64}.get(data[4])
    if elf_class is None or data[5] != 1:
        fail(f"unsupported ELF class or endian for {name}")
    wide = elf_class == 64
    header_format = "<HHIQQQIHHHHHH" if wide else "<HHIIIIIHHHHHH"
    header_size = struct.calcsize(header_format)
    if len(data) < 16 + header_size:
        fail(f"truncated ELF header: {name}")
    header = struct.unpack_from(header_format, data, 16)
    elf_type, machine = header[0], header[1]
    entry, phoff, phentsize, phnum = header[3], header[4], header[8], header[9]
    expected_phentsize = 56 if wide else 32
    if phentsize < expected_phentsize or phnum > 4096:
        fail(f"invalid ELF program header table: {name}")
    program_format = "<IIQQQQQQ" if wide else "<IIIIIIII"
    program_size = struct.calcsize(program_format)
    if phentsize < program_size or phoff + phentsize * phnum > len(data):
        fail(f"ELF program header table is outside input: {name}")

    programs: list[dict] = []
    for index in range(phnum):
        raw = struct.unpack_from(program_format, data, phoff + index * phentsize)
        if wide:
            p_type, flags, offset, vaddr, _paddr, filesz, memsz, align = raw
        else:
            p_type, offset, vaddr, _paddr, filesz, memsz, flags, align = raw
        if offset + filesz > len(data):
            fail(f"ELF segment is outside input: {name} ph={index}")
        programs.append({
            "index": index,
            "type": p_type,
            "offset": offset,
            "vaddr": vaddr,
            "filesz": filesz,
            "memsz": memsz,
            "flags": flags,
            "align": align,
        })

    loads = [p for p in programs if p["type"] == 1]

    def file_offset(address: int) -> int:
        for segment in loads:
            start = segment["vaddr"]
            end = start + segment["filesz"]
            if start <= address < end:
                return segment["offset"] + address - start
        fail(f"ELF virtual address is not backed by a PT_LOAD: {name} 0x{address:x}")

    interp = None
    for segment in programs:
        if segment["type"] == 3:
            interp = read_c_string(data, segment["offset"], segment["filesz"])
            break

    tls = []
    for segment in programs:
        if segment["type"] == 7:
            tls.append({
                "vaddr": f"0x{segment['vaddr']:x}",
                "offset": segment["offset"],
                "filesz": segment["filesz"],
                "memsz": segment["memsz"],
                "align": segment["align"],
            })

    dynamic_entries: list[tuple[int, int]] = []
    for segment in programs:
        if segment["type"] != 2:
            continue
        width = 16 if wide else 8
        item_format = "<qQ" if wide else "<iI"
        if segment["filesz"] % width:
            fail(f"misaligned PT_DYNAMIC: {name}")
        for pos in range(segment["offset"], segment["offset"] + segment["filesz"], width):
            tag, value = struct.unpack_from(item_format, data, pos)
            dynamic_entries.append((tag, value))
            if tag == 0:
                break

    string_table = next((value for tag, value in dynamic_entries if tag == 5), None)
    string_size = next((value for tag, value in dynamic_entries if tag == 10), None)

    def dynamic_string(tag: int) -> str | None:
        if string_table is None:
            return None
        value = next((v for t, v in dynamic_entries if t == tag), None)
        if value is None:
            return None
        address = file_offset(string_table) + value
        return read_c_string(data, address, string_size - value if string_size is not None else None)

    needed = []
    if string_table is not None:
        string_offset = file_offset(string_table)
        for tag, value in dynamic_entries:
            if tag == 1:
                needed.append(read_c_string(data, string_offset + value,
                                            string_size - value if string_size is not None else None))

    dynamic_tag_names = {
        0: "DT_NULL", 1: "DT_NEEDED", 2: "DT_PLTRELSZ", 3: "DT_PLTGOT", 4: "DT_HASH",
        5: "DT_STRTAB", 6: "DT_SYMTAB", 7: "DT_RELA", 8: "DT_RELASZ", 9: "DT_RELAENT",
        10: "DT_STRSZ", 11: "DT_SYMENT", 12: "DT_INIT", 13: "DT_FINI", 14: "DT_SONAME",
        15: "DT_RPATH", 16: "DT_SYMBOLIC", 17: "DT_REL", 18: "DT_RELSZ", 19: "DT_RELENT",
        20: "DT_PLTREL", 21: "DT_DEBUG", 22: "DT_TEXTREL", 23: "DT_JMPREL", 24: "DT_BIND_NOW",
        25: "DT_INIT_ARRAY", 26: "DT_FINI_ARRAY", 27: "DT_INIT_ARRAYSZ", 28: "DT_FINI_ARRAYSZ",
        29: "DT_RUNPATH", 30: "DT_FLAGS", 0x6FFFFFFB: "DT_FLAGS_1",
        0x6FFFFEF5: "DT_GNU_HASH", 0x6FFFFFF0: "DT_VERSYM", 0x6FFFFFFE: "DT_VERNEED",
        0x6FFFFFFF: "DT_VERNEEDNUM",
    }

    tags = []
    for tag, value in dynamic_entries:
        if tag == 0:
            continue
        record = {"tag": dynamic_tag_names.get(tag, f"0x{tag:x}"), "value": f"0x{value:x}"}
        if tag in (1, 14, 15, 29):
            string_value = dynamic_string(tag)
            if string_value is not None:
                record["string"] = string_value
        tags.append(record)

    sections = []
    shoff, shentsize, shnum = header[5], header[10], header[11]
    section_format = "<IIQQQQIIQQ" if wide else "<IIIIIIIIII"
    if shnum:
        if shentsize != struct.calcsize(section_format) or shoff + shnum * shentsize > len(data):
            fail(f"invalid ELF section table: {name}")
        for i in range(shnum):
            section = struct.unpack_from(section_format, data, shoff + i * shentsize)
            if section[1] != 8 and section[4] + section[5] > len(data):
                fail(f"ELF section outside input: {name}")
            sections.append(section)
    relocations = Counter()
    imports = set()
    for section in sections:
        kind, offset, size, link, stride = section[1], section[4], section[5], section[6], section[9]
        if kind in (4, 9):
            expected = (24 if kind == 4 else 16) if wide else (12 if kind == 4 else 8)
            if stride != expected or size % stride:
                fail(f"invalid ELF relocation table: {name}")
            for at in range(offset, offset + size, stride):
                info = struct.unpack_from("<Q" if wide else "<I", data, at + (8 if wide else 4))[0]
                relocations[str(info & (0xFFFFFFFF if wide else 0xFF))] += 1
        if kind == 11:
            if link >= len(sections) or stride != (24 if wide else 16) or size % stride:
                fail(f"invalid ELF dynamic symbols: {name}")
            strings = sections[link]
            for at in range(offset, offset + size, stride):
                symbol = struct.unpack_from("<IBBHQQ" if wide else "<IIIBBH", data, at)
                defined = symbol[3] if wide else symbol[5]
                if not defined and symbol[0]:
                    if symbol[0] >= strings[5]:
                        fail(f"ELF import string outside table: {name}")
                    imports.add(read_c_string(data, strings[4] + symbol[0], strings[5] - symbol[0]))

    return {
        "path": name,
        "bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "baseline_sha256": expected_hash,
        "baseline_match": expected_hash == hashlib.sha256(data).hexdigest() if expected_hash else False,
        "class_bits": elf_class,
        "machine": machine,
        "type": {2: "ET_EXEC", 3: "ET_DYN"}.get(elf_type, f"0x{elf_type:x}"),
        "entry": f"0x{entry:x}",
        "program_headers": len(programs),
        "pt_load": [
            {
                "offset": p["offset"], "vaddr": f"0x{p['vaddr']:x}",
                "filesz": p["filesz"], "memsz": p["memsz"],
                "flags": "".join(c for bit, c in ((4, "R"), (2, "W"), (1, "X")) if p["flags"] & bit),
                "align": p["align"],
            }
            for p in loads
        ],
        "pt_interp": interp,
        "pt_tls": tls,
        "dt_needed": needed,
        "dynamic_tags": tags,
        "relocation_types": dict(sorted(relocations.items(), key=lambda item: int(item[0]))),
        "undefined_dynamic_symbols": sorted(imports),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=BUILD / "elf-manifest.json")
    args = parser.parse_args()
    output = external_output(args.output)
    baseline = verify_baseline()
    ini = configparser.ConfigParser(interpolation=None)
    ini.read(RESOURCES / "baseline.ini", encoding="utf-16")
    expected = {ini["file" + str(i)]["path"]: ini["file" + str(i)]["sha256"]
                for i in range(ini.getint("resources", "count"))}
    images = []
    with zipfile.ZipFile(RESOURCES / "native.zip") as archive:
        if set(archive.namelist()) != set(expected):
            fail("ZIP members differ from baseline")
        for name, digest in expected.items():
            data = archive.read(name)
            if hashlib.sha256(data).hexdigest() != digest:
                fail("ZIP member differs: " + name)
            if data.startswith(b"\x7fELF"):
                images.append(parse_elf(name, data, digest))
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps({"schema": "dnf-native/elf-manifest/v1", "baseline": baseline,
        "generator_sha256": sha256(Path(__file__)), "images": images,
        "scope": "Static identity, headers, direct dependencies, relocation types and undefined symbols; no execution claim."},
        ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print("ELF manifest:", output, "images:", len(images))


if __name__ == "__main__":
    main()

