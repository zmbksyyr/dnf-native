"""Check the original channel TCP handshake in an isolated loopback instance."""

import argparse
import datetime
import hashlib
import json
from pathlib import Path
import platform
import re
import socket
import struct
import subprocess
import time

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from bootstrap import BUILD
from probe_core_protocols import encrypt, receive
from runtime_support import request_stop, unpack_runtime, process_memory

ROOT = Path(__file__).resolve().parents[1]
CHANNEL_SHA256 = "7f7f6bd9eea6452dc463bca2f2e33f07eec7675e9f37e280e92739cfb87747bd"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", type=Path, default=BUILD / "i686/dnf-host.exe")
    parser.add_argument("--port", type=int, default=7001, help="Private loopback TCP/UDP port")
    options = parser.parse_args()
    host = options.host.resolve()
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    instance = BUILD / "probes" / stamp / "channel"
    instance.mkdir(parents=True)
    unpack_runtime(instance, "i686", ("lib/", "usr/", "home/neople/channel/"))
    guest = instance / "home/neople/channel"
    for name in ["log", "pid"]:
        (guest / name).mkdir(exist_ok=True)
    config = guest / "cfg/channel.cfg"
    text = config.read_text(encoding="utf-8")
    text = re.sub(r"(?m)^this_ip\s*=.*$", "this_ip = 127.0.0.1", text)
    text = re.sub(r"(?m)^this_(tcp|udp)_port\s*=.*$", lambda m: "this_" + m[1] + "_port = " + str(options.port), text)
    config.write_text(text, encoding="utf-8", newline="\n")
    elf, ld = guest / "df_channel_r", instance / "lib/ld-2.17.so"
    if sha(elf) != CHANNEL_SHA256:
        raise ValueError("This protocol check requires the recorded original channel ELF")
    command = [str(host), "--root", str(instance), sha(ld), "--env", "HOME=/home/neople",
               "--cwd", "/home/neople/channel", "--run", str(elf), sha(elf),
               "/home/neople/channel/df_channel_r", "channel", "run"]
    forced, error, stop_sent, memory = False, None, False, None
    checks = {"negative_handshake": False, "positive_responses": 0, "fragmented_request": False,
              "coalesced_requests": False}
    with (instance / "stdout.log").open("wb") as out, (instance / "stderr.log").open("wb") as err:
        process = subprocess.Popen(command, stdout=out, stderr=err)
        try:
            deadline = time.monotonic() + 10
            while True:
                if process.poll() is not None:
                    raise RuntimeError("Channel exited during startup")
                try:
                    peer = socket.create_connection(("127.0.0.1", options.port), timeout=0.2)
                    break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise RuntimeError("Channel TCP startup timeout")
                    time.sleep(0.1)
            with peer:
                peer.settimeout(5)
                # Derived from this ELF's registerProtocols, onCS_CONNECT,
                # tagPacketHeader and CRijndael::Initialize disassembly.
                header = struct.pack("<BBIIB", 0x7c, 11, 27, 0, 0)
                reply_header = struct.pack("<BBIIB", 0x7c, 12, 47, 0, 1)
                peer.sendall(header + bytes(16))
                reply = receive(peer, 47)
                if reply[:11] != reply_header or reply[11:15] != bytes(4) or not any(reply[15:31]):
                    raise RuntimeError("Channel negative handshake differs")
                checks["negative_handshake"] = True
                packet = header + encrypt(reply[15:31], b"dnf_game" + bytes(8))
                expected = reply_header + struct.pack("<I", 1) + bytes(32)
                for _ in range(29):
                    peer.sendall(packet)
                    if receive(peer, 47) != expected:
                        raise RuntimeError("Channel positive handshake differs")
                    checks["positive_responses"] += 1
                peer.sendall(packet[:5])
                time.sleep(0.05)
                peer.sendall(packet[5:])
                if receive(peer, 47) != expected:
                    raise RuntimeError("Channel fragmented request differs")
                checks["positive_responses"] += 1
                checks["fragmented_request"] = True
                peer.sendall(packet * 2)
                if receive(peer, 94) != expected * 2:
                    raise RuntimeError("Channel coalesced requests differ")
                checks["positive_responses"] += 2
                checks["coalesced_requests"] = True
            memory = process_memory(process.pid)
        except Exception as failure:
            error = str(failure)
        finally:
            if process.poll() is None:
                request_stop(process.pid)
                stop_sent = True
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    forced = True
                    process.terminate()
                    process.wait(timeout=5)
    stderr = (instance / "stderr.log").read_text(encoding="utf-8", errors="replace")
    gaps = [line for line in stderr.splitlines() if any(word in line for word in
            ["syscall gap", "unsupported", "unhandled exception", "unimplemented guest", "exhausted"])]
    result = {"recorded_utc": stamp, "os": platform.platform(), "probe": str(instance.relative_to(BUILD)),
               "scope": "Original channel TCP connect protocol only; private loopback port.",
               "service_port": options.port, "memory": memory,
              "command": command, "host_sha256": sha(host), "elf_sha256": sha(elf), "ld_sha256": sha(ld),
              "config_sha256": sha(config), "checks": checks, "error": error, "gaps": gaps,
              "exit_code": process.returncode, "sigterm_event_sent": stop_sent, "forced_cleanup": forced,
              "log_sha256": {p.name: sha(p) for p in instance.glob("*.log")}}
    (BUILD / "channel-checks.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    passed = not error and not gaps and not forced and stop_sent and process.returncode == 0
    print("Original channel TCP:", "PASS" if passed else "FAIL", checks, "exit", process.returncode)
    if not passed:
        raise SystemExit("See ../x/dnf-native-output/channel-checks.json and the private logs")


if __name__ == "__main__":
    main()
