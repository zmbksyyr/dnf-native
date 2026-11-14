"""Verify TCP I/O, poll, receive timeout and real SO_LINGER close behavior."""

import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import socket
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / ".local/toolchains/llvm-mingw-20261104-msvcrt-x86_64/bin"
REQUEST = b"native-tcp\0guest"
REPLY = b"native-tcp\0reply"
FIELDS = ["poll_empty", "poll_write_count", "poll_write_events", "poll_negative_count", "poll_negative_events",
          "poll_invalid_count", "poll_invalid_events", "poll_read_count", "poll_read_events",
          "receive_timeout_result", "receive_timeout_ms", "keepalive_result", "nodelay_result", "timeout_option_result"]


def main():
    env = dict(os.environ)
    env["PATH"] = str(BIN) + os.pathsep + env.get("PATH", "")
    records = []
    for arch, target, base, abortive in [(arch, target, base, abortive)
            for arch, target, base in [("i686", "i386-linux-gnu", "0x8048000"), ("x86_64", "x86_64-linux-gnu", "0x400000")]
            for abortive in [False, True]]:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(8)
            port = listener.getsockname()[1]
            fixture = ROOT / "build/fixtures" / arch / "net"
            fixture.parent.mkdir(parents=True, exist_ok=True)
            build = [str(BIN / "clang.exe"), "--target=" + target, "-fuse-ld=lld", "-nostdlib", "-static",
                     "-ffreestanding", "-fno-stack-protector", "-fno-pic", "-O2", "-Wall", "-Wextra", "-Werror",
                     "-DNR_PORT=" + str(port), "-DNR_LINGER_ON=" + str(int(abortive)), "-Wl,--image-base=" + base, "-Wl,-e,_start",
                     str(ROOT / "tests/net.c"), "-o", str(fixture)]
            subprocess.run(build, env=env, check=True)
            host = ROOT / "build" / arch / "dnf-host.exe"
            sha = hashlib.sha256(fixture.read_bytes()).hexdigest()
            command = [str(host), "--run", str(fixture), sha, "net"]
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            received, error, forced, eof, reset = b"", None, False, False, False
            try:
                with listener.accept()[0] as peer:
                    peer.settimeout(8)
                    while len(received) < len(REQUEST):
                        chunk = peer.recv(len(REQUEST) - len(received))
                        if not chunk:
                            raise RuntimeError("Guest closed before completing request")
                        received += chunk
                    if received != REQUEST:
                        raise RuntimeError("Guest TCP payload differed")
                    # Exercise socket readiness while the guest also waits on an empty pipe.
                    time.sleep(0.1)
                    peer.sendall(REPLY)
                    # Keep the peer open and silent so the guest must hit SO_RCVTIMEO.
                    try:
                        eof = peer.recv(1) == b""
                    except ConnectionResetError:
                        reset = True
                stdout, stderr = process.communicate(timeout=8)
            except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
                error = str(exc)
                if process.poll() is None:
                    process.terminate()
                    forced = True
                stdout, stderr = process.communicate(timeout=5)
            values = dict(zip(FIELDS, struct.unpack("<14i", stdout[:56]))) if len(stdout) >= 56 else {}
            passed = (not error and not forced and process.returncode == 42 and (reset if abortive else eof) and received == REQUEST and
                      len(stdout) == 56 + len(REPLY) and stdout[56:] == REPLY)
            records.append({"arch": arch, "build": build, "command": command, "port": port,
                            "fixture_sha256": sha, "host_sha256": hashlib.sha256(host.read_bytes()).hexdigest(),
                            "exit_code": process.returncode, "forced_cleanup": forced, "error": error,
                            "linger_on": abortive, "peer_eof": eof, "peer_reset": reset,
                            "server_received_hex": received.hex(), "server_sent_hex": REPLY.hex(),
                            "guest_returned_hex": stdout[56:].hex(), "checks": values,
                            "stderr": stderr.decode("utf-8", "replace"), "passed": passed})
            print(arch, "TCP/poll/100ms timeout", "abortive" if abortive else "graceful", "PASS" if passed else "FAIL", "exit", process.returncode, flush=True)
    receipt = {"recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "os": platform.platform(),
               "scope": "Own Linux ELF fixtures and loopback Python peer only; not original service or cross-system acceptance.",
               "results": records}
    (ROOT / "build/net-checks.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    if not all(r["passed"] for r in records):
        raise SystemExit("TCP/poll checks failed")


if __name__ == "__main__":
    main()
