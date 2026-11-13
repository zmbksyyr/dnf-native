"""Exercise the original x64 STUN protocol through real guest pthreads and UDP."""

import datetime
import hashlib
import json
from pathlib import Path
import platform
import shutil
import socket
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    instance = ROOT / "build/probes" / stamp / "stun"
    instance.mkdir(parents=True)
    source = ROOT / ".local/sysroot/x86_64"
    for name in ["lib64", "usr"]:
        shutil.copytree(source / name, instance / name)
    ld = instance / "lib64/ld-2.17.so"
    elf = ROOT / "originals/dp123/home/neople/stun/df_stun_r"
    command = [str(ROOT / "build/x86_64/dnf-host.exe"), "--root", str(instance), hashlib.sha256(ld.read_bytes()).hexdigest(),
               "--run", str(elf), hashlib.sha256(elf.read_bytes()).hexdigest(), "/home/neople/stun/df_stun_r", "test"]
    records = []
    stdout_path, stderr_path = instance / "stdout.log", instance / "stderr.log"
    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        process = subprocess.Popen(command, stdout=stdout, stderr=stderr)
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
                client.bind(("127.0.0.1", 0))
                client.settimeout(0.25)
                local = client.getsockname()
                address_reply = b"\x02" + struct.pack("<IH", 0x7f000001, local[1])
                for iteration in range(10):
                    for port, request, expected, source_port in [
                        (2311, b"\x01", address_reply, 2311), (2312, b"\x01", address_reply, 2312),
                        (2311, b"\x05", address_reply, 2313), (2312, b"\x05", address_reply, 2313),
                        (2313, b"length-probe", struct.pack("<H", 12), 2313),
                    ]:
                        deadline = time.monotonic() + 5
                        while True:
                            if process.poll() is not None:
                                raise RuntimeError(f"STUN exited before response: {process.returncode}")
                            client.sendto(request, ("127.0.0.1", port))
                            try:
                                reply, remote = client.recvfrom(4096)
                                break
                            except (socket.timeout, ConnectionResetError):
                                if time.monotonic() >= deadline:
                                    raise RuntimeError(f"STUN response timed out on {port}")
                        passed = reply == expected and remote == ("127.0.0.1", source_port)
                        records.append({"iteration": iteration, "port": port, "request_hex": request.hex(), "reply_hex": reply.hex(),
                                        "expected_hex": expected.hex(), "source_port": remote[1], "passed": passed})
                        if not passed:
                            raise RuntimeError("Unexpected original STUN response")
        finally:
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=5)
    stderr = stderr_path.read_text(encoding="utf-8", errors="replace")
    gaps = [line for line in stderr.splitlines() if "syscall gap" in line]
    receipt = {"recorded_utc": stamp, "os": platform.platform(), "command": command,
               "elf_sha256": hashlib.sha256(elf.read_bytes()).hexdigest(), "host_sha256": hashlib.sha256(Path(command[0]).read_bytes()).hexdigest(),
               "scope": "Original STUN test/foreground mode, loopback UDP only. Forced test cleanup; no graceful-stop, fork or three-OS acceptance.",
               "forced_cleanup_exit_code": process.returncode, "syscall_gaps": gaps,
               "stdout_sha256": hashlib.sha256(stdout_path.read_bytes()).hexdigest(),
               "stderr_sha256": hashlib.sha256(stderr_path.read_bytes()).hexdigest(), "results": records}
    (ROOT / "build/stun-checks.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(f"Original STUN: {len(records)} verified UDP responses; {len(gaps)} unimplemented syscall diagnostics")


if __name__ == "__main__":
    main()
