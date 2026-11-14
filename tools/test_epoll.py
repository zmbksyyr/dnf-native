"""Check pipe/socket epoll readiness, timeout, HUP and watch removal."""

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import socket
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / ".local/toolchains/llvm-mingw-20261104-msvcrt-x86_64/bin"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hosts", type=Path, default=ROOT / "build")
    args = parser.parse_args()
    env = dict(os.environ)
    env["PATH"] = str(BIN) + os.pathsep + env.get("PATH", "")
    records = []
    for arch, target, base in [("i686", "i386-linux-gnu", "0x8048000"), ("x86_64", "x86_64-linux-gnu", "0x400000")]:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
            peer.bind(("127.0.0.1", 0))
            peer.settimeout(3)
            fixture = ROOT / "build/fixtures" / arch / "epoll"
            fixture.parent.mkdir(parents=True, exist_ok=True)
            build = [str(BIN / "clang.exe"), "--target=" + target, "-fuse-ld=lld", "-nostdlib", "-static",
                     "-ffreestanding", "-fno-stack-protector", "-fno-pic", "-O2", "-Wall", "-Wextra", "-Werror",
                     "-Wl,--image-base=" + base, "-Wl,-e,_start", "-DNR_PORT=" + str(peer.getsockname()[1]),
                     str(ROOT / "tests/epoll.c"), "-o", str(fixture)]
            subprocess.run(build, env=env, check=True)
            host = args.hosts.resolve() / arch / "dnf-host.exe"
            digest = hashlib.sha256(fixture.read_bytes()).hexdigest()
            command = [str(host), "--run", str(fixture), digest, "epoll-fixture"]
            process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            checks = {}

            def respond():
                try:
                    data, address = peer.recvfrom(16)
                    checks["udp_request"] = data == b"u"
                    time.sleep(0.1)
                    peer.sendto(b"v", address)
                    checks["marker"] = process.stdout.readline().decode("ascii")
                    if checks["marker"] == "epoll-wait\n":
                        time.sleep(0.1)
                        process.stdin.write(b"g")
                        process.stdin.flush()
                except OSError as failure:
                    checks["error"] = str(failure)

            worker = threading.Thread(target=respond)
            worker.start()
            forced = False
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                forced = True
                process.terminate()
                process.wait(timeout=5)
            worker.join(timeout=5)
            process.stdin.close()
            stdout = checks.get("marker", "") + process.stdout.read().decode("ascii")
            stderr = process.stderr.read().decode("utf-8", errors="replace")
            passed = not forced and not checks.get("error") and checks.get("udp_request") and process.returncode == 42 and stdout == "epoll-wait\npipe-epoll-ok\n"
            records.append({"arch": arch, "host_sha256": hashlib.sha256(host.read_bytes()).hexdigest(),
                            "fixture_sha256": digest, "build": build, "command": command,
                            "exit_code": process.returncode, "forced_cleanup": forced, "checks": checks,
                            "stdout": stdout, "stderr": stderr, "passed": passed})
            print(arch, "pipe/socket epoll", "PASS" if passed else "FAIL", "exit", process.returncode, flush=True)
    receipt = {"recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "os": platform.platform(),
               "scope": "Own native ELF pipe/socket epoll fixtures; no full epoll or service acceptance", "results": records}
    (ROOT / "build/epoll-checks.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    if not all(record["passed"] for record in records):
        raise SystemExit("Epoll checks failed; see build/epoll-checks.json")


if __name__ == "__main__":
    main()
