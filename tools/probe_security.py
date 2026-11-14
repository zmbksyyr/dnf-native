"""Observe original security services in a private copy; does not assert readiness."""

import argparse
import datetime
import json
from pathlib import Path
import platform
import shutil
import subprocess
import xml.etree.ElementTree as ET

from test_bridge import sha
from test_channel import request_stop

ROOT = Path(__file__).resolve().parents[1]
ZERG_SHA256 = "c2def44c445d372c544fd49f79dc79ac62a72750a04035818f3eceb407f26c17"
ZERG_CLEANUP_PATCH = {
    "address": "0x080775d5", "offset": 0x2f5d5,
    "before": "e8c6290200e8d1ed020085c0740de8c8ed02008b10890424ff5208e88bc3030085c0740de882c303008b10890424ff5224",
    "after": "e8d6ed020085c0740de8cded02008b10890424ff5208e890c3030085c0740de887c303008b10890424ff5224e89a290200",
    "purpose": "Destroy reactor/timer callbacks before Comm_Stat_Monitor; preserve both cleanups and function size.",
}


def fix_zerg_cleanup(elf):
    if sha(elf) != ZERG_SHA256:
        raise ValueError("Cleanup order fix requires the recorded original zerg ELF")
    data = bytearray(elf.read_bytes())
    patch = ZERG_CLEANUP_PATCH
    before, after = bytes.fromhex(patch["before"]), bytes.fromhex(patch["after"])
    offset = patch["offset"]
    if len(before) != len(after) or data[offset:offset + len(before)] != before:
        raise ValueError("Original zerg cleanup bytes differ")
    data[offset:offset + len(before)] = after
    elf.write_bytes(data)
    return {"original_sha256": ZERG_SHA256, "executed_sha256": sha(elf), "patch": patch}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", type=Path, default=ROOT / "build/i686/dnf-host.exe")
    parser.add_argument("--observe-seconds", type=float, default=5)
    parser.add_argument("--local-routes", action="store_true",
                        help="Use original cfgfile routing, remove identical duplicate routes, and start zerg first")
    parser.add_argument("--fix-cleanup-order", action="store_true",
                        help="Apply checked reactor/statistics cleanup order fix only to the private zerg ELF")
    args = parser.parse_args()
    if args.observe_seconds <= 0:
        parser.error("--observe-seconds must be positive")
    host = args.host.resolve()
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    private = ROOT / "build/probes" / stamp / "security"
    private.mkdir(parents=True)
    for name in ["lib", "usr"]:
        shutil.copytree(ROOT / ".local/sysroot/i686" / name, private / name)
    source = ROOT / "originals/dp123/home/neople/secsvr"
    shutil.copytree(source, private / "home/neople/secsvr")
    for name in ["dev/shm", "tmp"]:
        (private / name).mkdir(parents=True)
    changes = []
    elf_changes = []
    if args.fix_cleanup_order:
        elf_changes.append(fix_zerg_cleanup(private / "home/neople/secsvr/zergsvr/zergsvr"))
    if args.local_routes:
        cfg = private / "home/neople/secsvr/zergsvr/cfg/zergsvrd.xml"
        before = sha(cfg)
        old = b"<get_svr_info_type>cfgsvr</get_svr_info_type>"
        if cfg.read_bytes().count(old) != 1:
            raise RuntimeError("Original zerg routing setting differs")
        cfg.write_bytes(cfg.read_bytes().replace(old, b"<get_svr_info_type>cfgfile</get_svr_info_type>"))
        changes.append({"path": str(cfg.relative_to(private)), "before_sha256": before,
                        "after_sha256": sha(cfg), "change": "Select original cfgfile route implementation"})
        cfg = cfg.with_name("svcid.xml")
        before = sha(cfg)
        tree = ET.fromstring(cfg.read_text(encoding="gbk"))
        seen = {}
        for element in list(tree.findall("service_info_")):
            identity = (element.findtext("svr_type_").strip(), element.findtext("svr_id_").strip())
            values = {child.tag: (child.text or "").strip() for child in element}
            if identity in seen:
                if seen[identity] != values:
                    raise RuntimeError("Conflicting original service routes")
                tree.remove(element)
            else:
                seen[identity] = values
        tree.find("service_num_").text = str(len(seen))
        cfg.write_bytes(ET.tostring(tree, encoding="utf-8", xml_declaration=True))
        changes.append({"path": str(cfg.relative_to(private)), "before_sha256": before,
                        "after_sha256": sha(cfg), "change": "Keep each identical service route once; update count"})
    ld = private / "lib/ld-2.17.so"
    services = []
    receipt = {"recorded_utc": stamp, "os": platform.platform(),
               "probe": str(private.relative_to(ROOT)), "observe_seconds": args.observe_seconds,
               "scope": "Security startup/stop observation; optional private zerg patch is recorded explicitly. No readiness, game interaction, client or business acceptance.",
               "configuration_changes": changes,
               "elf_changes": elf_changes,
               "results": []}
    entries = [("gunnersvr", "gunnersvr", ["-t7", "-i570016"]), ("secagent", "zergsvr", []),
               ("zergsvr", "zergsvr", ["-t30", "-i570011"])]
    if args.local_routes:
        entries = [entries[2], entries[1], entries[0]]
    try:
        for name, directory, arguments in entries:
            guest = private / "home/neople/secsvr" / directory
            elf = guest / name
            record = {"name": name, "host_sha256": sha(host), "elf_sha256": sha(elf),
                      "loader_sha256": sha(ld), "arguments": arguments,
                      "configuration_sha256": {str(p.relative_to(guest)): sha(p)
                                                for p in sorted((guest / "cfg").rglob("*")) if p.is_file()}}
            command = [str(host), "--root", str(private), sha(ld), "--env", "HOME=/home/neople",
                       "--cwd", "/home/neople/secsvr/" + directory, "--run", str(elf), sha(elf),
                       "/home/neople/secsvr/" + directory + "/" + name, *arguments]
            with (private / (name + "-stdout.private.log")).open("wb") as out, (private / (name + "-stderr.log")).open("wb") as err:
                process = subprocess.Popen(command, stdout=out, stderr=err)
            services.append((process, guest, record))
            receipt["results"].append(record)
            try:
                process.wait(timeout=args.observe_seconds)
            except subprocess.TimeoutExpired:
                pass
    finally:
        markers = ["ZEN_Reactor and ZEN_Epoll_Reactor initialized.", "MMAP Pipe init success",
                   "Comm_Svrd_Appliction::init_instance Success.", "init gamesvr channel succ",
                   "init user info mngr succ", "update dpsdk config info succ",
                   "connect monitor failed", "monitor reporter init fail. ret=5092",
                   "connect monitor succ", "Get File Configure Success.", "Zerg_Service_App::run_instance end.",
                   "exit_instance Succ.Have Fun.!!!",
                   "Not pull config but get ip from cfgsvr", "init_instance fail. ret=290010039",
                   "restrict function will closed"]
        for process, guest, record in reversed(services):
            name = record["name"]
            record["forced_cleanup"] = False
            record["stop_signal"] = None
            if process.poll() is None:
                record["stop_signal"] = 15
                request_stop(process.pid)
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    record["forced_cleanup"] = True
                    process.terminate()
                    process.wait(timeout=5)
            record["exit_code"] = process.returncode
            stderr = (private / (name + "-stderr.log")).read_text(encoding="utf-8", errors="replace")
            record["gaps"] = [line for line in stderr.splitlines()
                              if any(x in line for x in ["syscall gap", "unhandled exception", "unsupported", "exhausted", "mapping failed"])]
            logs = [private / (name + "-stdout.private.log"), private / (name + "-stderr.log")]
            logs += [p for p in (guest / "log").glob(name + "*") if p.is_file()]
            text = "\n".join(p.read_text(encoding="utf-8", errors="replace") for p in logs)
            record["markers"] = [marker for marker in markers if marker in text]
            record["logs"] = {str(p.relative_to(private)): sha(p) for p in logs}
            record["shared_user_info_bytes"] = sum(p.stat().st_size for p in guest.rglob("PLAT_USER_INFO_*.MMAP"))
        (private / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        (ROOT / "build/security-probe.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        print("Security observation:", private / "receipt.json", flush=True)
        for record in receipt["results"]:
            print(record["name"], "exit", record["exit_code"], "forced", record["forced_cleanup"], "gaps", record["gaps"], flush=True)


if __name__ == "__main__":
    main()
