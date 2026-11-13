"""Read-only inventory of supplied archives and PE files; never execute inputs.

Python 3.11+ standard library only. Metadata is not runtime acceptance evidence.
"""

import argparse
import collections
import datetime
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shlex
import struct
import tarfile
import zlib


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def elf_metadata(data):
    bits = {1: 32, 2: 64}[data[4]]
    if data[5] != 1:
        raise ValueError("Only little-endian input supported")
    wide = bits == 64
    eh = struct.unpack_from("<HHIQQQIHHHHHH" if wide else "<HHIIIIIHHHHHH", data, 16)
    kind, machine, _, entry, phoff = eh[:5]
    phentsize, phnum = eh[8:10]
    headers = []
    for index in range(phnum):
        ph = struct.unpack_from("<IIQQQQQQ" if wide else "<IIIIIIII", data, phoff + index * phentsize)
        if wide:
            typ, flags, offset, address, _, filesz, memsz, align = ph
        else:
            typ, offset, address, _, filesz, memsz, flags, align = ph
        headers.append(dict(type=typ, flags=flags, offset=offset, address=address,
                            filesz=filesz, memsz=memsz, align=align))

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

    versions = collections.defaultdict(list)
    if 0x6FFFFFFE in dynamic:
        pos = offset(dynamic[0x6FFFFFFE][0])
        for _ in range(dynamic.get(0x6FFFFFFF, [0])[0]):
            _, count, name, aux, nxt = struct.unpack_from("<HHIII", data, pos)
            apos = pos + aux
            for _ in range(count):
                _, _, _, vname, anext = struct.unpack_from("<IHHII", data, apos)
                versions[string(name)].append(string(vname))
                if not anext:
                    break
                apos += anext
            if not nxt:
                break
            pos += nxt
    relocations = collections.Counter()
    tables = []
    for address_tag, size_tag, entry_tag, rela in [(17, 18, 19, False), (7, 8, 9, True)]:
        if address_tag in dynamic and size_tag in dynamic:
            tables.append((dynamic[address_tag][0], dynamic[size_tag][0], rela,
                           dynamic.get(entry_tag, [24 if wide and rela else 16 if wide else 12 if rela else 8])[0]))
    if 23 in dynamic and 2 in dynamic:
        rela = dynamic.get(20, [17])[0] == 7
        tables.append((dynamic[23][0], dynamic[2][0], rela,
                       24 if wide and rela else 16 if wide else 12 if rela else 8))
    for address, size, rela, stride in tables:
        start = offset(address)
        for pos in range(start, start + size, stride):
            info = struct.unpack_from("<Q" if wide else "<I", data, pos + (8 if wide else 4))[0]
            relocations[str(info & (0xFFFFFFFF if wide else 0xFF))] += 1
    loads = [ph for ph in headers if ph["type"] == 1]
    interps = [data[ph["offset"]:ph["offset"] + ph["filesz"]].rstrip(b"\0").decode("utf-8")
               for ph in headers if ph["type"] == 3]
    return {
        "bits": bits, "machine": {3: "i386", 62: "x86_64"}.get(machine, str(machine)),
        "type": {2: "ET_EXEC", 3: "ET_DYN"}.get(kind, str(kind)),
        "entry": hex(entry), "interpreter": interps[0] if interps else None,
        "load_start": hex(min(ph["address"] for ph in loads)) if loads else None,
        "load_end": hex(max(ph["address"] + ph["memsz"] for ph in loads)) if loads else None,
        "needed": [string(value) for value in dynamic.get(1, [])],
        "soname": string(dynamic[14][0]) if 14 in dynamic else None,
        "version_needs": dict(versions), "relocation_type_counts": dict(relocations),
        "tls": [{"filesz": ph["filesz"], "memsz": ph["memsz"], "align": ph["align"]}
                for ph in headers if ph["type"] == 7],
    }


def pe_metadata(path):
    data = path.read_bytes()
    if data[:2] != b"MZ":
        raise ValueError(f"Not PE: {path}")
    peoff = struct.unpack_from("<I", data, 0x3C)[0]
    if data[peoff:peoff + 4] != b"PE\0\0":
        raise ValueError(f"Invalid PE: {path}")
    machine, count = struct.unpack_from("<HH", data, peoff + 4)
    optsize = struct.unpack_from("<H", data, peoff + 20)[0]
    opt = peoff + 24
    wide = struct.unpack_from("<H", data, opt)[0] == 0x20B
    dd = opt + (112 if wide else 96)
    imp_rva = struct.unpack_from("<I", data, dd + 8)[0]
    sections = []
    for index in range(count):
        pos = opt + optsize + index * 40
        vsize, va, size, raw = struct.unpack_from("<IIII", data, pos + 8)
        sections.append((va, max(vsize, size), raw))

    def off(rva):
        for va, size, raw in sections:
            if va <= rva < va + size:
                return raw + rva - va
        raise ValueError(f"Unmapped PE RVA {rva:x}")

    imports = []
    if imp_rva:
        pos = off(imp_rva)
        while any(data[pos:pos + 20]):
            name_rva = struct.unpack_from("<I", data, pos + 12)[0]
            start = off(name_rva)
            imports.append(data[start:data.index(b"\0", start)].decode("ascii"))
            pos += 20
    return {"machine": {0x14C: "i386", 0x8664: "x86_64"}.get(machine, hex(machine)),
            "import_dlls": imports, "note": "Normal imports only; not delay-load/runtime closure"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = args.workspace.resolve()
    archives = list(root.glob("DP*/*/DOF.tar.gz"))
    if len(archives) != 1:
        raise ValueError("Expected exactly one DP input archive")
    archive = archives[0]
    assets, members, startup, script_apis, frida_versions = [], [], [], {}, []
    groups = collections.defaultdict(lambda: {"files": 0, "bytes": 0, "raw_deflate9_bytes": 0})
    service_names = {"df_auction_r", "df_bridge_r", "df_channel_r", "df_community_r", "df_coserver_r",
                     "df_dbmw_r", "df_game_r", "df_guild_r", "df_manager_r", "df_monitor_r",
                     "df_point_r", "df_relay_r", "df_statics_r", "df_stun_r"}
    with tarfile.open(archive, "r|gz") as tf:
        for member in tf:
            members.append({"path": member.name, "bytes": member.size,
                            "type": member.type.decode("ascii", "replace"),
                            **({"link_target": member.linkname} if member.linkname else {})})
            if not member.isfile():
                continue
            name = PurePosixPath(member.name).name
            if member.name == "home/neople/game/Script.pvf":
                group = "pvf"
            elif member.name.startswith("dp2/"):
                group = "dp2_all"
            elif name in service_names:
                group = "core_16_elf"
            elif name in {"gunnersvr", "secagent", "zergsvr"}:
                group = "security_3_elf"
            elif name.startswith(("libtss", "libantisvr")):
                group = "tss_antisvr_regular_files"
            else:
                group = "other_regular_files"
            data = tf.extractfile(member).read()
            if member.name == "root/run":
                cwd = "/"
                for line in data.decode("utf-8", "replace").splitlines():
                    tokens = shlex.split(line, comments=True)
                    if not tokens:
                        continue
                    if tokens[0] == "cd" and len(tokens) == 2:
                        cwd = tokens[1]
                    env = {}
                    if tokens[0].startswith("LD_PRELOAD="):
                        env["LD_PRELOAD"] = tokens.pop(0).split("=", 1)[1]
                    if tokens and PurePosixPath(tokens[0]).name in service_names | {"gunnersvr", "secagent", "zergsvr"}:
                        startup.append({"cwd": cwd, "argv": [t for t in tokens if t != "&"], "environment": env})
            if member.name in {"dp2/df_game_r.lua", "dp2/df_game_r.js"}:
                script_apis[member.name] = dict(collections.Counter(re.findall(
                    r"(?:Module|Process|Memory|Interceptor|Stalker|NativeFunction|NativeCallback|dp2_frida_resolver|frida\.\w+|lua\.\w+)[\w.]*",
                    data.decode("utf-8", "replace"))))
            if member.name == "dp2/frida/frida.so":
                frida_versions = sorted({v.decode("ascii") for v in re.findall(
                    rb"(?<=\x00)1[567]\.\d{1,2}\.\d{1,2}(?=\x00)", data)})
            packer = zlib.compressobj(9, zlib.DEFLATED, -15)
            compressed = len(packer.compress(data)) + len(packer.flush())
            groups[group]["files"] += 1
            groups[group]["bytes"] += len(data)
            groups[group]["raw_deflate9_bytes"] += compressed
            if data.startswith(b"\x7fELF") or group == "pvf" or member.name in {"root/run", "root/stop"}:
                row = {"path": member.name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                       "raw_deflate9_bytes": compressed, "group": group}
                if data.startswith(b"\x7fELF"):
                    row["elf"] = elf_metadata(data)
                assets.append(row)
    mysql = root / "opendnf-win-jbz69/mysql/mysql-5.7.44-winx64"
    db_rows = []
    with tarfile.open(archive.parent / "MySQL.tar.gz", "r|gz") as tf:
        for member in tf:
            if member.isfile():
                db_rows.append({"path": member.name, "bytes": member.size})
    cases = collections.defaultdict(list)
    for row in db_rows:
        cases[row["path"].casefold()].append(row["path"])
    database = {
        "archive": "MySQL.tar.gz", "files": len(db_rows), "bytes": sum(r["bytes"] for r in db_rows),
        "extensions": dict(collections.Counter(PurePosixPath(r["path"]).suffix.lower() for r in db_rows)),
        "case_collisions": [names for names in cases.values() if len(set(names)) > 1], "members": db_rows,
    }
    mysql_files = []
    if mysql.is_dir():
        for name in ["mysqld.exe", "mysql.exe", "mysqladmin.exe", "mysqldump.exe"]:
            path = mysql / "bin" / name
            packer = zlib.compressobj(9, zlib.DEFLATED, -15)
            packed_size = len(packer.compress(path.read_bytes())) + len(packer.flush())
            mysql_files.append({"path": str(path.relative_to(root)).replace("\\", "/"),
                                "bytes": path.stat().st_size, "sha256": digest(path),
                                "raw_deflate9_bytes": packed_size,
                                "pe": pe_metadata(path)})
    result = {
        "schema_version": 1, "recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "scope": "Static metadata and raw DEFLATE measurement; no input code executed or extracted",
        "tool": {"sha256": digest(Path(__file__)), "zlib_version": zlib.ZLIB_VERSION},
        "input_files": [{"path": str(p.relative_to(root)).replace("\\", "/"),
                         "bytes": p.stat().st_size, "sha256": digest(p)}
                        for p in sorted(archive.parent.iterdir()) if p.is_file()],
        "archive_member_count": len(members), "archive_members": members,
        "compression_groups": dict(groups), "assets": assets, "mysql_files": mysql_files,
        "startup_from_original_run": startup, "script_api_occurrences": script_apis,
        "frida_null_terminated_version_candidates": frida_versions, "database_archive": database,
        "limitations": ["Supplied asset provenance is not proof of vendor-original binaries",
                        "ELF DT_NEEDED is not a full dlopen/runtime dependency closure",
                        "Raw DEFLATE sizes exclude ZIP headers, project runtime and external dependencies",
                        "PE parsing covers normal import DLL names, not imported APIs or runtime loads",
                        "No runtime, hook, service, database or Windows-version acceptance performed"],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"assets": len(assets), "elf_count": sum("elf" in a for a in assets),
                      "groups": dict(groups), "output": str(args.output)}))


if __name__ == "__main__":
    main()
