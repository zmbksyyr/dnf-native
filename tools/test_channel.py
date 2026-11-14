"""Check the original channel TCP handshake in an isolated loopback instance."""

import argparse
import ctypes
from ctypes import wintypes as w
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

ROOT = Path(__file__).resolve().parents[1]
CHANNEL_SHA256 = "7f7f6bd9eea6452dc463bca2f2e33f07eec7675e9f37e280e92739cfb87747bd"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


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


def request_stop(pid, signal=15):
    # Same runtime events used by cross-process guest kill(). This run mode
    # does not write a PID file, so its original stop helper cannot target it.
    if signal not in [12, 15]:
        raise ValueError("Only SIGUSR2 and SIGTERM have runtime events")
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenEventW.argtypes = [w.DWORD, w.BOOL, w.LPCWSTR]
    kernel.OpenEventW.restype = w.HANDLE
    kernel.SetEvent.argtypes = [w.HANDLE]
    kernel.CloseHandle.argtypes = [w.HANDLE]
    prefix = "Local\\dnf-native-sigusr2-" if signal == 12 else "Local\\dnf-native-terminate-"
    event = kernel.OpenEventW(2, False, prefix + str(pid))
    if not event:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        if not kernel.SetEvent(event):
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        kernel.CloseHandle(event)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", type=Path, default=ROOT / "build/i686/dnf-host.exe")
    options = parser.parse_args()
    host = options.host.resolve()
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    instance = ROOT / "build/probes" / stamp / "channel"
    instance.mkdir(parents=True)
    for name in ["lib", "usr"]:
        shutil.copytree(ROOT / ".local/sysroot/i686" / name, instance / name)
    guest = instance / "home/neople/channel"
    shutil.copytree(ROOT / "originals/dp123/home/neople/channel", guest)
    for name in ["log", "pid"]:
        (guest / name).mkdir(exist_ok=True)
    config = guest / "cfg/channel.cfg"
    text = config.read_text(encoding="utf-8")
    text = re.sub(r"(?m)^this_ip\s*=.*$", "this_ip = 127.0.0.1", text)
    config.write_text(text, encoding="utf-8", newline="\n")
    elf, ld = guest / "df_channel_r", instance / "lib/ld-2.17.so"
    if sha(elf) != CHANNEL_SHA256:
        raise ValueError("This protocol check requires the recorded original channel ELF")
    command = [str(host), "--root", str(instance), sha(ld), "--env", "HOME=/home/neople",
               "--cwd", "/home/neople/channel", "--run", str(elf), sha(elf),
               "/home/neople/channel/df_channel_r", "channel", "run"]
    forced, error, stop_sent = False, None, False
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
                    peer = socket.create_connection(("127.0.0.1", 7001), timeout=0.2)
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
    result = {"recorded_utc": stamp, "os": platform.platform(), "probe": str(instance.relative_to(ROOT)),
              "scope": "Original channel TCP connect protocol only; loopback port 7001, no bridge/database/client business acceptance.",
              "command": command, "host_sha256": sha(host), "elf_sha256": sha(elf), "ld_sha256": sha(ld),
              "config_sha256": sha(config), "checks": checks, "error": error, "gaps": gaps,
              "exit_code": process.returncode, "sigterm_event_sent": stop_sent, "forced_cleanup": forced,
              "log_sha256": {p.name: sha(p) for p in instance.glob("*.log")}}
    (ROOT / "build/channel-checks.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    passed = not error and not gaps and not forced and stop_sent and process.returncode == 0
    print("Original channel TCP:", "PASS" if passed else "FAIL", checks, "exit", process.returncode)
    if not passed:
        raise SystemExit("See build/channel-checks.json and the private logs")


if __name__ == "__main__":
    main()
