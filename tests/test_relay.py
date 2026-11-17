"""Verify original relay TCP forwarding and UDP disconnect control on loopback."""

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
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

from probe_core_protocols import receive, packet
from runtime_support import request_stop, unpack_runtime

from bootstrap import BUILD

ROOT = Path(__file__).resolve().parents[1]
RELAY_SHA256 = "39c87b68408a82f840808c09c520b911c01176349af9eaa3bf071cce2295c3eb"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def concurrent_clients(count, duration, checks):
    """Keep registered peers active, route a ring, replace 10%, then repeat."""
    peers, accounts = [None] * count, [10000 + index for index in range(count)]
    checks.update(registered=0, rounds=0, forwarded=0, replaced=0)

    def register(index):
        deadline = time.monotonic() + 10
        while True:
            try:
                peer = socket.create_connection(("127.0.0.1", 7200), timeout=5)
                break
            except ConnectionRefusedError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.05)
        peer.settimeout(5)
        account = accounts[index]
        body = struct.pack("<I", account)
        try:
            peer.sendall(packet(0, account, 0) + packet(1, account, account, body))
            if receive(peer, 12 + len(body)) != packet(3, account, account, body):
                raise RuntimeError("Concurrent relay registration differs")
            return peer
        except BaseException:
            peer.close()
            raise

    def connect(indices):
        errors = []
        last_keepalive = time.monotonic()
        with ThreadPoolExecutor(max_workers=32) as pool:
            pending = {pool.submit(register, index): index for index in indices}
            for future in as_completed(pending):
                try:
                    peers[pending[future]] = future.result()
                    checks["registered"] += 1
                    if checks["registered"] % 250 == 0:
                        print("Relay concurrent registrations:", checks["registered"], flush=True)
                except Exception as error:
                    errors.append("peer " + str(pending[future]) + ": " + str(error))
                # Keep successful registrations active while the ramp finishes.
                # Batch sends before reads to avoid a per-peer round-trip delay.
                if time.monotonic() - last_keepalive >= 1:
                    active_peers = [(index, peer) for index, peer in enumerate(peers) if peer is not None]
                    body = b"ramp-active"
                    for index, peer in active_peers:
                        account = accounts[index]
                        peer.sendall(packet(1, account, account, body))
                    for index, peer in active_peers:
                        account = accounts[index]
                        if receive(peer, 12 + len(body)) != packet(3, account, account, body):
                            raise RuntimeError("Relay ramp keepalive differs")
                    last_keepalive = time.monotonic()
        if errors:
            checks["registration_errors"] = errors
            raise RuntimeError("Concurrent relay registration: " + errors[0])

    def forward():
        sequence = checks["rounds"]
        for index, peer in enumerate(peers):
            body = struct.pack("<II", sequence, index) + bytes(range(256))
            peer.sendall(packet(1, accounts[index], accounts[(index + 1) % count], body))
        for index, peer in enumerate(peers):
            source = (index - 1) % count
            body = struct.pack("<II", sequence, source) + bytes(range(256))
            if receive(peer, 12 + len(body)) != packet(3, accounts[source], accounts[index], body):
                raise RuntimeError("Concurrent relay forwarded sender/recipient/payload differs")
            checks["forwarded"] += 1
        checks["rounds"] += 1

    started = time.monotonic()
    try:
        connect(range(count))
        checks["initial_connections"] = checks["registered"]
        active = time.monotonic()
        while checks["rounds"] == 0 or time.monotonic() - active < duration:
            forward()
            time.sleep(0.02)
        checks["active_seconds"] = round(time.monotonic() - active, 3)
        replaced = list(range(0, count, 10))
        for index in replaced:
            peers[index].shutdown(socket.SHUT_WR)
        for index in replaced:
            if peers[index].recv(1) != b"":
                raise RuntimeError("Relay did not acknowledge the replaced connection's close")
            peers[index].close(); peers[index] = None
            accounts[index] += count
        checks["closed_before_replacement"] = len(replaced)
        connect(replaced)
        checks["replaced"] = len(replaced)
        forward()
        checks["after_replacement_ring"] = True
        return checks
    finally:
        try:
            if checks.get("after_replacement_ring"):
                for peer in peers:
                    peer.shutdown(socket.SHUT_WR)
                checks["closed_before_recovery"] = 0
                for peer in peers:
                    if peer.recv(1) != b"":
                        raise RuntimeError("Relay did not acknowledge the final connection's close")
                    checks["closed_before_recovery"] += 1
        finally:
            checks["elapsed_seconds"] = round(time.monotonic() - started, 3)
            for peer in peers:
                if peer is not None:
                    peer.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reconnect-rounds", type=int, default=0)
    parser.add_argument("--host", type=Path, default=BUILD / "i686/dnf-host.exe")
    parser.add_argument("--concurrent-clients", type=int, default=0,
                        help="Exercise the unchanged original max_client=1000 configuration (2..1000)")
    parser.add_argument("--load-seconds", type=float, default=30)
    args = parser.parse_args()
    if args.reconnect_rounds < 0:
        parser.error("--reconnect-rounds must be nonnegative")
    if args.concurrent_clients and not 2 <= args.concurrent_clients <= 1000:
        parser.error("--concurrent-clients must be 0 or 2..1000")
    if not 0 <= args.load_seconds <= 300:
        parser.error("--load-seconds must be 0..300")
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    instance = BUILD / "probes" / stamp / "relay"
    instance.mkdir(parents=True)
    unpack_runtime(instance, "i686", ("lib/", "usr/", "home/neople/relay/"))
    guest = instance / "home/neople/relay"
    for name in ["log", "pid"]:
        (guest / name).mkdir(exist_ok=True)
    config = guest / "cfg/relay_200.cfg"
    text = config.read_text(encoding="ascii")
    if not args.concurrent_clients:
        text = re.sub(r"(?m)^max_client\s*=.*$", "max_client = 16", text)
        config.write_text(text, encoding="ascii")
    elf, ld, host = guest / "df_relay_r", instance / "lib/ld-2.17.so", args.host.resolve()
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
            if args.concurrent_clients:
                checks["concurrent"] = {}
                concurrent_clients(args.concurrent_clients, args.load_seconds, checks["concurrent"])
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
            if args.reconnect_rounds:
                from probe_core_protocols import relay
                for peer in peers:
                    peer.close()
                peers.clear()
                checks["reconnect_rounds"] = 0
                for _ in range(args.reconnect_rounds):
                    time.sleep(0.2)
                    relay()
                    checks["reconnect_rounds"] += 1
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
    result = {"recorded_utc": stamp, "os": platform.platform(), "probe": str(instance.relative_to(BUILD)),
              "scope": "Original relay PVP-mode TCP forwarding and UDP disconnect control with own loopback peers",
              "command": command, "host_sha256": sha(host), "elf_sha256": sha(elf), "ld_sha256": sha(ld),
              "config_sha256": sha(config), "checks": checks, "error": error, "gaps": gaps,
              "source_sha256": sha(Path(__file__)), "concurrent_clients": args.concurrent_clients,
              "load_seconds": args.load_seconds,
              "exit_code": process.returncode, "sigterm_event_sent": stop_sent, "forced_cleanup": forced,
              "log_sha256": {p.name: sha(p) for p in instance.glob("*.log")}}
    (BUILD / "relay-checks.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    passed = not error and not gaps and not forced and stop_sent and process.returncode == 0
    print("Original relay forwarding:", "PASS" if passed else "FAIL", checks, "exit", process.returncode)
    if not passed:
        raise SystemExit("See ../x/dnf-native-output/relay-checks.json and the private logs")


if __name__ == "__main__":
    main()
