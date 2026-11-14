"""Observe original monitor initialization and stop, with an explicit private UDP fix."""

import argparse
import datetime
import hashlib
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
MONITOR_SHA256 = "a099d2b3ebc77bfe1393bec3543a41ba8c20c99d0abc26619241d31964dea6d7"

# Independently derived from this ELF's disassembly. Preserve all addresses and
# file length. The input remains read-only; omit --fix-udp-stop to reproduce it.
UDP_STOP_PATCHES = [
    {"address": "0x0805aa4c", "offset": 0x12a4c,
     "before": "8b45088944240cc7442408b8aa0508c744240400000000",
     "after": "c64008018944240cc7442408b8aa050883642404009090",
     "purpose": "Set running before pthread_create; preserve its original arguments."},
    {"address": "0x080973ce", "offset": 0x4f3ce,
     "before": "c6400801", "after": "90909090",
     "purpose": "Do not overwrite an early stop after the UDP worker's startup sleep."},
    {"address": "0x08064042", "offset": 0x1c042,
     "before": "8b45088b803401000085c074538b45088b80340100008b008b108b45088b8034010000890424ffd28b45088b803401000085c0741e8b45088b80340100008b0083c00c8b108b45088b8034010000890424ffd28b4508c7803401000000000000",
     "after": "8b45088b983401000085db7453c64308008b43108b00890424e8606bfeff8b4310c700ffffffff891c24e8296affff891c24e80d3203008b4508c7803401000000000000" + "90" * 28,
     "purpose": "Stop, close UDP receive fd, invalidate it, join, then delete the thread object."},
]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def private_elf(original, destination):
    data = bytearray(original.read_bytes())
    if hashlib.sha256(data).hexdigest() != MONITOR_SHA256:
        raise ValueError("UDP stop fix requires the recorded original monitor ELF")
    for patch in UDP_STOP_PATCHES:
        before, after = bytes.fromhex(patch["before"]), bytes.fromhex(patch["after"])
        offset = patch["offset"]
        if len(before) != len(after) or data[offset:offset + len(before)] != before:
            raise ValueError("UDP stop patch bytes differ at " + patch["address"])
        data[offset:offset + len(before)] = after
    destination.write_bytes(data)
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fix-udp-stop", action="store_true", help="Apply the checked fix only to a private ELF copy")
    parser.add_argument("--stop-after", type=float, default=12, help="Seconds before the original stop command (default: 12)")
    options = parser.parse_args()
    if options.stop_after <= 0:
        parser.error("--stop-after must be positive")
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    instance = ROOT / "build/probes" / stamp / "monitor"
    instance.mkdir(parents=True)
    for name in ["lib", "usr"]:
        shutil.copytree(ROOT / ".local/sysroot/i686" / name, instance / name)
    original = ROOT / "originals/dp123/home/neople/monitor"
    guest = instance / "home/neople/monitor"
    for name in ["cfg", "script"]:
        shutil.copytree(original / name, guest / name)
    for name in ["pid", "log"]:
        (guest / name).mkdir()
    shutil.copy2(original / "server_str.xml", guest / "server_str.xml")
    config = guest / "cfg/mnt_siroco.cfg"
    text = config.read_text(encoding="utf-8")
    config.write_text(re.sub(r"(?<!\d)(?:\d{1,3}\.){3}\d{1,3}(?!\d)", "127.0.0.1", text), encoding="utf-8")
    original_elf = original / "df_monitor_r"
    elf = private_elf(original_elf, guest / "df_monitor_r") if options.fix_udp_stop else original_elf
    host, ld = ROOT / "build/i686/dnf-host.exe", instance / "lib/ld-2.17.so"
    command = [str(host), "--root", str(instance), sha(ld), "--env", "HOME=/home/neople",
               "--cwd", "/home/neople/monitor", "--run", str(elf), sha(elf),
               "/home/neople/monitor/df_monitor_r", "mnt_siroco", "nofork"]
    forced, pid_matches, stop_code = False, False, None
    pid_file = guest / "pid/mnt_siroco.pid"
    with (instance / "stdout.log").open("wb") as out, (instance / "stderr.log").open("wb") as err:
        process = subprocess.Popen(command, stdout=out, stderr=err)
        try:
            process.wait(timeout=options.stop_after)
        except subprocess.TimeoutExpired:
            pid_matches = pid_file.is_file() and pid_file.read_text().strip() == str(process.pid)
            with (instance / "stop-stdout.log").open("wb") as so, (instance / "stop-stderr.log").open("wb") as se:
                try:
                    stop = subprocess.run(command[:-1] + ["stop"], stdout=so, stderr=se, timeout=10)
                    stop_code = stop.returncode
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    forced = True
                    process.terminate()
                    process.wait(timeout=5)
    stdout = (instance / "stdout.log").read_text(encoding="utf-8", errors="replace")
    stderr = (instance / "stderr.log").read_text(encoding="utf-8", errors="replace")
    markers = {name: name in stdout for name in ["Application Init() Success!", "Application Load() Success!",
               "Recv SIGTERM signal", "Udp Thread Free Success!", "Application Exit!", "Application Stop!"]}
    gaps = [line for line in stderr.splitlines() if any(word in line for word in ["syscall gap", "unsupported", "unhandled exception"])]
    result = {"recorded_utc": stamp, "os": platform.platform(), "probe": str(instance.relative_to(ROOT)),
              "scope": "Original monitor initialization and stop observation; loopback-only private configuration, no DBMW/manager peers or protocol acceptance.",
              "command": command, "host_sha256": sha(host), "ld_sha256": sha(ld),
              "original_elf_sha256": sha(original_elf), "executed_elf_sha256": sha(elf),
              "patches": UDP_STOP_PATCHES if options.fix_udp_stop else [], "stop_after_seconds": options.stop_after,
              "configuration_sha256": sha(config), "exit_code": process.returncode, "stop_exit_code": stop_code,
              "forced_cleanup": forced, "pid_matches_host": pid_matches, "pid_removed": not pid_file.exists(),
              "markers": markers, "gaps": gaps,
              "log_sha256": {p.name: sha(p) for p in instance.glob("*.log")}}
    (ROOT / "build/monitor-probe.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print("Monitor initialization/stop:", markers, "exit:", process.returncode, "stop command:", stop_code)
    # This ELF returns 1 after Application Stop. Its stop helper unconditionally
    # throws CDNFException after sending SIGTERM (exit 134); retain both codes.
    if forced or not pid_matches or not result["pid_removed"] or not all(markers.values()) or gaps or process.returncode != 1 or stop_code != 134:
        raise SystemExit("Monitor did not complete the recorded initialization/stop path")


if __name__ == "__main__":
    main()
