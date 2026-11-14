"""Verify original relay TCP forwarding and UDP disconnect control on loopback."""

import datetime
import hashlib
import json
from pathlib import Path
import platform
import re
import shutil
import socket
import struct
import subprocess
import time

from test_channel import receive, request_stop

ROOT = Path(__file__).resolve().parents[1]
RELAY_SHA256 = "39c87b68408a82f840808c09c520b911c01176349af9eaa3bf071cce2295c3eb"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def packet(identifier, sender, recipient, body=b""):
    return struct.pack("<HHII", identifier, 12 + len(body), sender, recipient) + body


def main():
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    instance = ROOT / "build/probes" / stamp / "relay"
    instance.mkdir(parents=True)
    for name in ["lib", "usr"]:
        shutil.copytree(ROOT / ".local/sysroot/i686" / name, instance / name)
    guest = instance / "home/neople/relay"
    shutil.copytree(ROOT / "originals/dp123/home/neople/relay", guest)
    for name in ["log", "pid"]:
        (guest / name).mkdir(exist_ok=True)
    config = guest / "cfg/relay_200.cfg"
    text = config.read_text(encoding="ascii")
    text = re.sub(r"(?m)^max_client\s*=.*$", "max_client = 16", text)
    config.write_text(text, encoding="ascii")
    elf, ld, host = guest / "df_relay_r", instance / "lib/ld-2.17.so", ROOT / "build/i686/dnf-host.exe"
    if sha(elf) != RELAY_SHA256:
        raise ValueError("This protocol check requires the recorded original relay ELF")
    command = [str(host), "--root", str(instance), sha(ld), "--env", "HOME=/home/neople",
               "--cwd", "/home/neople/relay", "--run", str(elf), sha(elf),
               "/home/neople/relay/df_relay_r", "relay_200", "run"]
    checks = {"registered_peers": 0, "tcp_messages": 0, "udp_disconnect": False, "other_peer_preserved": False,
              "fragmented_request": False, "coalesced_requests": False}
    peers = []
    error, forced, stop_sent = None, False, False
    with (instance / "stdout.log").open("wb") as out, (instance / "stderr.log").open("wb") as err:
        process = subprocess.Popen(command, stdout=out, stderr=err)
        try:
            deadline = time.monotonic() + 10
            for account in [701, 702]:
                while True:
                    if process.poll() is not None:
                        raise RuntimeError("Relay exited during startup")
                    try:
                        peer = socket.create_connection(("127.0.0.1", 7200), timeout=0.2)
                        break
                    except OSError:
                        if time.monotonic() >= deadline:
                            raise RuntimeError("Relay TCP startup timeout")
                        time.sleep(0.1)
                peer.settimeout(5)
                peers.append(peer)
                body = b"registered\0" + struct.pack("<I", account)
                # Register and self-forward on the same stream. The response
                # confirms registration before the other peer sends to it.
                peer.sendall(packet(0, account, 0) + packet(1, account, account, body))
                expected = packet(3, account, account, body)
                if receive(peer, len(expected)) != expected:
                    raise RuntimeError("Relay registration/self-forward differs")
                checks["registered_peers"] += 1
            for index in range(32):
                sender, recipient = (0, 1) if index % 2 == 0 else (1, 0)
                body = struct.pack("<I", index) + bytes(range(256)) * 7
                request = packet(1, 701 + sender, 701 + recipient, body)
                peers[sender].sendall(request)
                expected = packet(3, 701 + sender, 701 + recipient, body)
                if receive(peers[recipient], len(expected)) != expected:
                    raise RuntimeError("Relay TCP payload differs")
                checks["tcp_messages"] += 1
            request = packet(1, 701, 702, b"fragmented\0packet")
            peers[0].sendall(request[:5])
            time.sleep(0.05)
            peers[0].sendall(request[5:])
            if receive(peers[1], len(request)) != struct.pack("<H", 3) + request[2:]:
                raise RuntimeError("Relay fragmented request differs")
            checks["fragmented_request"] = True
            peers[0].sendall(request * 2)
            expected = (struct.pack("<H", 3) + request[2:]) * 2
            if receive(peers[1], len(expected)) != expected:
                raise RuntimeError("Relay coalesced requests differ")
            checks["coalesced_requests"] = True
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
                # Original PVP mode exposes the S2S handler on 8200. ID 2500
                # with result 0 disconnects the account at offset 10.
                udp.sendto(struct.pack("<HHHIIB", 2500, 15, 0, 0, 701, 0), ("127.0.0.1", 8200))
                if peers[0].recv(1) != b"":
                    raise RuntimeError("Relay UDP disconnect did not close its target")
                checks["udp_disconnect"] = True
            peers[1].sendall(packet(1, 702, 702, b"still connected"))
            expected = packet(3, 702, 702, b"still connected")
            if receive(peers[1], len(expected)) != expected:
                raise RuntimeError("Relay disconnected the unrelated peer")
            checks["other_peer_preserved"] = True
        except Exception as failure:
            error = str(failure)
        finally:
            for peer in peers:
                peer.close()
            if process.poll() is None:
                request_stop(process.pid)
                stop_sent = True
                try:
                    process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    forced = True
                    process.terminate()
                    process.wait(timeout=5)
    stderr = (instance / "stderr.log").read_text(encoding="utf-8", errors="replace")
    gaps = [line for line in stderr.splitlines() if any(word in line for word in
            ["syscall gap", "unsupported", "decode gap", "unhandled exception", "unimplemented guest", "exhausted"])]
    result = {"recorded_utc": stamp, "os": platform.platform(), "probe": str(instance.relative_to(ROOT)),
              "scope": "Original relay PVP-mode TCP forwarding and UDP disconnect control with own loopback peers; no original monitor/game/client acceptance.",
              "command": command, "host_sha256": sha(host), "elf_sha256": sha(elf), "ld_sha256": sha(ld),
              "config_sha256": sha(config), "checks": checks, "error": error, "gaps": gaps,
              "exit_code": process.returncode, "sigterm_event_sent": stop_sent, "forced_cleanup": forced,
              "log_sha256": {p.name: sha(p) for p in instance.glob("*.log")}}
    (ROOT / "build/relay-checks.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    passed = not error and not gaps and not forced and stop_sent and process.returncode == 0
    print("Original relay forwarding:", "PASS" if passed else "FAIL", checks, "exit", process.returncode)
    if not passed:
        raise SystemExit("See build/relay-checks.json and the private logs")


if __name__ == "__main__":
    main()
