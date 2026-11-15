"""Check channel/relay/STUN protocols on an already running private core chain."""

import argparse
import ctypes
from ctypes import wintypes as w
import datetime
import hashlib
import json
from pathlib import Path
import socket
import struct
import time

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from bootstrap import external_output


def encrypt(key, data):
    """Use Windows AES-128 ECB; no copied Rijndael code or third-party package."""
    bcrypt = ctypes.WinDLL("bcrypt")
    pointer, ulong = ctypes.c_void_p, w.ULONG
    bcrypt.BCryptOpenAlgorithmProvider.argtypes = [ctypes.POINTER(pointer), w.LPCWSTR, w.LPCWSTR, ulong]
    bcrypt.BCryptSetProperty.argtypes = [pointer, w.LPCWSTR, pointer, ulong, ulong]
    bcrypt.BCryptGenerateSymmetricKey.argtypes = [pointer, ctypes.POINTER(pointer), pointer, ulong, pointer, ulong, ulong]
    bcrypt.BCryptEncrypt.argtypes = [pointer, pointer, ulong, pointer, pointer, ulong, pointer, ulong, ctypes.POINTER(ulong), ulong]
    bcrypt.BCryptDestroyKey.argtypes = [pointer]
    bcrypt.BCryptCloseAlgorithmProvider.argtypes = [pointer, ulong]

    def check(status):
        if status:
            raise RuntimeError("Windows AES status: " + hex(status & 0xffffffff))

    algorithm, handle = pointer(), pointer()
    check(bcrypt.BCryptOpenAlgorithmProvider(ctypes.byref(algorithm), "AES", None, 0))
    try:
        mode = ctypes.create_unicode_buffer("ChainingModeECB")
        check(bcrypt.BCryptSetProperty(algorithm, "ChainingMode", mode, ctypes.sizeof(mode), 0))
        secret = ctypes.create_string_buffer(key)
        check(bcrypt.BCryptGenerateSymmetricKey(algorithm, ctypes.byref(handle), None, 0, secret, len(key), 0))
        try:
            source, output = ctypes.create_string_buffer(data), ctypes.create_string_buffer(len(data))
            count = ulong()
            check(bcrypt.BCryptEncrypt(handle, source, len(data), None, None, 0, output, len(data), ctypes.byref(count), 0))
            return output.raw[:count.value]
        finally:
            bcrypt.BCryptDestroyKey(handle)
    finally:
        bcrypt.BCryptCloseAlgorithmProvider(algorithm, 0)


def receive(peer, size):
    data = b""
    while len(data) < size:
        chunk = peer.recv(size - len(data))
        if not chunk:
            raise RuntimeError("Channel closed before its complete response")
        data += chunk
    return data


def packet(identifier, sender, recipient, body=b""):
    return struct.pack("<HHII", identifier, 12 + len(body), sender, recipient) + body


def channel(port, host="127.0.0.1"):
    header = struct.pack("<BBIIB", 0x7c, 11, 27, 0, 0)
    reply_header = struct.pack("<BBIIB", 0x7c, 12, 47, 0, 1)
    with socket.create_connection((host, port), timeout=5) as peer:
        peer.sendall(header + bytes(16))
        reply = receive(peer, 47)
        if reply[:11] != reply_header or reply[11:15] != bytes(4) or not any(reply[15:31]):
            raise RuntimeError("Channel challenge differs")
        request = header + encrypt(reply[15:31], b"dnf_game" + bytes(8))
        expected = reply_header + struct.pack("<I", 1) + bytes(32)
        peer.sendall(request[:5])
        time.sleep(0.05)
        peer.sendall(request[5:])
        if receive(peer, 47) != expected:
            raise RuntimeError("Channel fragmented handshake differs")
        peer.sendall(request * 2)
        if receive(peer, 94) != expected * 2:
            raise RuntimeError("Channel coalesced handshakes differ")
    return {"challenge": True, "positive_responses": 3, "fragmented": True, "coalesced": True}


def relay(monitor_auth=False, host="127.0.0.1"):
    peers = []
    accounts = [701, 702]
    stage = "connect"
    try:
        for account in accounts:
            stage = "registration for account " + str(account)
            peer = socket.create_connection((host, 7200), timeout=5)
            peers.append(peer)
            peer.sendall(packet(0, account, 0) + packet(1, account, account, b"registered"))
            expected = packet(3, account, account, b"registered")
            if receive(peer, len(expected)) != expected:
                raise RuntimeError("Relay registration differs")
        for index in range(8):
            stage = "forwarded message " + str(index)
            source, target = index % 2, 1 - index % 2
            body = struct.pack("<I", index) + bytes(range(256)) * 7
            peers[source].sendall(packet(1, accounts[source], accounts[target], body))
            expected = packet(3, accounts[source], accounts[target], body)
            if receive(peers[target], len(expected)) != expected:
                raise RuntimeError("Relay forwarded bytes differ")
        if monitor_auth:
            # These own test accounts have no live game login. Wake the relay's
            # normal auth queue with ignored S2S type 1 packets, then require
            # the actual monitor to reject both; never forge an auth response.
            stage = "monitor rejection of unlogged test accounts"
            closed = set()
            for peer in peers:
                peer.settimeout(0.1)
            deadline = time.monotonic() + 6
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as wake:
                while len(closed) != len(peers):
                    if time.monotonic() >= deadline:
                        raise RuntimeError("Unlogged relay accounts were not both rejected")
                    wake.sendto(struct.pack("<HHHIIB", 1, 15, 0, 0, 0, 0), (host, 8200))
                    for index, peer in enumerate(peers):
                        if index in closed:
                            continue
                        try:
                            data = peer.recv(1)
                        except socket.timeout:
                            continue
                        except ConnectionResetError:
                            data = b""
                        if data:
                            raise RuntimeError("Unexpected bytes during monitor rejection")
                        closed.add(index)
            return {"registered_peers": 2, "bidirectional_messages": 8,
                    "unlogged_accounts_disconnected": accounts, "own_auth_response_sent": False}
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            stage = "targeted disconnect"
            udp.sendto(struct.pack("<HHHIIB", 2500, 15, 0, 0, accounts[0], 0), (host, 8200))
        if peers[0].recv(1) != b"":
            raise RuntimeError("Relay targeted disconnect differs")
        peers[1].sendall(packet(1, accounts[1], accounts[1], b"still connected"))
        stage = "unrelated peer after targeted disconnect"
        expected = packet(3, accounts[1], accounts[1], b"still connected")
        if receive(peers[1], len(expected)) != expected:
            raise RuntimeError("Relay disconnected the unrelated peer")
    except Exception as error:
        raise RuntimeError("Relay " + stage + ": " + str(error)) from error
    finally:
        for peer in peers:
            peer.close()
    return {"registered_peers": 2, "bidirectional_messages": 8, "targeted_disconnect": True,
            "other_peer_preserved": True}


def stun():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
        peer.bind(("127.0.0.1", 0))
        peer.settimeout(5)
        expected = b"\x02" + struct.pack("<IH", 0x7f000001, peer.getsockname()[1])
        for port, request, reply, source in [(2311, b"\x01", expected, 2311),
                                             (2312, b"\x01", expected, 2312),
                                             (2311, b"\x05", expected, 2313),
                                             (2312, b"\x05", expected, 2313),
                                             (2313, b"length-probe", struct.pack("<H", 12), 2313)]:
            peer.sendto(request, ("127.0.0.1", port))
            data, address = peer.recvfrom(4096)
            if data != reply or address != ("127.0.0.1", source):
                raise RuntimeError("STUN response bytes or source port differ")
    return {"responses": 5, "address_and_source_ports": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--channel-port", type=int, required=True)
    parser.add_argument("--relay-monitor-auth", action="store_true",
                        help="Expect the running original monitor to reject the unlogged test accounts")
    args = parser.parse_args()
    output = external_output(args.output)
    receipt = {"recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
               "scope": "Own loopback protocol peers against existing private services; no game client acceptance.",
               "source_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
               "channel_port": args.channel_port, "relay_accounts": [701, 702], "results": {}}
    receipt["relay_monitor_auth"] = args.relay_monitor_auth
    for name, check in [("channel", lambda: channel(args.channel_port)),
                        ("relay", lambda: relay(args.relay_monitor_auth)), ("stun", stun)]:
        try:
            receipt["results"][name] = {"passed": True, "checks": check()}
        except Exception as error:
            receipt["results"][name] = {"passed": False, "error": str(error)}
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(receipt))
    if not all(result["passed"] for result in receipt["results"].values()):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
