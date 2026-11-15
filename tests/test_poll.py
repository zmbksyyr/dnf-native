"""Verify select timeout normalization and real pipe poll state/wakeups."""

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import threading
import time

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from bootstrap import TOOLCHAIN, BUILD

ROOT = Path(__file__).resolve().parents[1]
BIN = TOOLCHAIN


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hosts", type=Path, default=BUILD, help="Directory containing i686/x86_64 hosts")
    args = parser.parse_args()
    env = dict(os.environ)
    env["PATH"] = str(BIN) + os.pathsep + env.get("PATH", "")
    records = []
    for arch, target, base in [("i686", "i386-linux-gnu", "0x8048000"),
                               ("x86_64", "x86_64-linux-gnu", "0x400000")]:
        fixture = BUILD / "fixtures" / arch / "poll"
        fixture.parent.mkdir(parents=True, exist_ok=True)
        build = [str(BIN / "clang.exe"), "--target=" + target, "-fuse-ld=lld", "-nostdlib", "-static",
                 "-ffreestanding", "-fno-stack-protector", "-fno-pic", "-O2",
                 "-Wall", "-Wextra", "-Werror", "-Wl,--image-base=" + base, "-Wl,-e,_start",
                 str(ROOT / "tests/poll.c"), "-o", str(fixture)]
        subprocess.run(build, env=env, check=True)
        host = args.hosts.resolve() / arch / "dnf-host.exe"
        sha = hashlib.sha256(fixture.read_bytes()).hexdigest()
        command = [str(host), "--run", str(fixture), sha, "poll-fixture"]
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        peer = {}

        def write_later():
            peer["marker"] = process.stdout.readline()
            if peer["marker"] == b"pipe-wait\n":
                time.sleep(0.1)
                try:
                    process.stdin.write(b"g")
                    process.stdin.flush()
                except BrokenPipeError:
                    peer["write_failed"] = True

        writer = threading.Thread(target=write_later)
        writer.start()
        forced = False
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            forced = True
            process.terminate()
            process.wait(timeout=5)
        writer.join(timeout=5)
        process.stdin.close()
        stdout = peer.get("marker", b"") + process.stdout.read()
        stderr = process.stderr.read().decode("utf-8", "replace")
        passed = not forced and not peer.get("write_failed") and process.returncode == 42 and stdout == b"pipe-wait\npipe-poll-ok\n"
        records.append({"arch": arch, "build": build, "command": command,
                        "host_sha256": hashlib.sha256(host.read_bytes()).hexdigest(),
                        "fixture_sha256": sha, "exit_code": process.returncode, "forced_cleanup": forced,
                        "stdout": stdout.decode("utf-8", "replace"), "stderr": stderr, "passed": passed})
        print(arch, "select timeout / pipe poll", "PASS" if passed else "FAIL", "exit", process.returncode, flush=True)
    result = {"recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "os": platform.platform(), "scope": "Own Linux select/pipe fixtures and external Windows pipe writer; not service acceptance",
              "results": records}
    (BUILD / "poll-checks.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if not all(record["passed"] for record in records):
        raise SystemExit("Pipe poll checks failed; see ../x/dnf-native-output/poll-checks.json")


if __name__ == "__main__":
    main()
