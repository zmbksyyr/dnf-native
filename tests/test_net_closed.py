"""Real TCP reset/shutdown, UDP errors and SIGPIPE; optionally run identical ELF on Linux."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import socket
import struct
import subprocess
import sys
import threading


def exercise(command, case, signal):
    errors = []
    listener = None
    thread = None
    if case < 4 or case == 6:
        listener = socket.socket()
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", 48124))
        listener.listen(1)
        listener.settimeout(8)

        def peer():
            try:
                with listener.accept()[0] as stream:
                    stream.settimeout(8)
                    assert stream.recv(1) == b"x"
                    if case < 3 or case == 6:
                        if case == 6:
                            stream.sendall(b"y")
                        stream.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                                          struct.pack("HH" if os.name == "nt" else "ii", 1, 0))
                    else:
                        stream.sendall(b"y")
                        assert stream.recv(1) == b""
            except Exception as exc:
                errors.append(repr(exc))

        thread = threading.Thread(target=peer)
        thread.start()
    try:
        process = subprocess.run(command, capture_output=True, timeout=12)
    finally:
        if thread:
            thread.join(9)
            listener.close()
    default_signal = signal == 3 and case != 5
    expected = (141 if os.name == "nt" else -13) if default_signal else 42
    passed = (process.returncode == expected and not errors and
              process.stdout == (b"" if default_signal else b"closed-socket-ok\n"))
    return {"case": case, "signal": signal, "exit": process.returncode, "passed": passed,
            "stdout": process.stdout.decode(errors="replace"),
            "stderr": process.stderr.decode(errors="replace"), "peer_errors": errors}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hosts", type=Path)
    parser.add_argument("--linux-distro")
    parser.add_argument("--buffered-reset", action="store_true",
                        help="Also expose the remaining Winsock loss of unread data on reset; currently fails Windows")
    parser.add_argument("--native", type=Path)
    parser.add_argument("--case", type=int)
    parser.add_argument("--signal", type=int)
    args = parser.parse_args()
    if args.native:
        print(json.dumps(exercise([str(args.native)], args.case, args.signal)))
        return
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
    from bootstrap import TOOLCHAIN, BUILD
    hosts = args.hosts or BUILD
    records = []
    for arch, target, base in [("i686", "i386-linux-gnu", "0x8048000"),
                               ("x86_64", "x86_64-linux-gnu", "0x400000")]:
        for case in range(7 if args.buffered_reset else 6):
            for signal in (range(4) if case < 5 else [1]):
                fixture = BUILD / "fixtures" / arch / f"net-closed-{case}-{signal}"
                fixture.parent.mkdir(parents=True, exist_ok=True)
                build = [str(TOOLCHAIN / "clang.exe"), "--target=" + target, "-fuse-ld=lld",
                         "-nostdlib", "-static", "-ffreestanding", "-fno-stack-protector", "-fno-pic",
                         "-O2", "-Wall", "-Wextra", "-Werror", "-Wl,--image-base=" + base,
                         "-Wl,-e,_start", "-DNR_PORT=48124", f"-DNR_CASE={case}", f"-DNR_SIGNAL={signal}",
                         str(Path(__file__).with_name("net_closed.c")),
                         str(Path(__file__).with_name("futex_clone.S")), "-o", str(fixture)]
                subprocess.run(build, check=True)
                host = hosts / arch / "dnf-host.exe"
                digest = hashlib.sha256(fixture.read_bytes()).hexdigest()
                record = exercise([str(host), "--run", str(fixture), digest, "net-closed"], case, signal)
                record.update(arch=arch, fixture_sha256=digest, host_sha256=hashlib.sha256(host.read_bytes()).hexdigest())
                if args.linux_distro:
                    def linux_path(path):
                        path = str(path.resolve()).replace("\\", "/")
                        return "/mnt/" + path[0].lower() + path[2:]
                    result = subprocess.run(["wsl.exe", "-d", args.linux_distro, "-u", "root", "--",
                                             "python3", "-B", linux_path(Path(__file__)), "--native", linux_path(fixture),
                                             "--case", str(case), "--signal", str(signal)],
                                            check=True, capture_output=True, text=True, timeout=25)
                    record["linux"] = json.loads(result.stdout)
                records.append(record)
                print(arch, case, signal, "PASS" if record["passed"] else "FAIL",
                      "Linux=" + str(record.get("linux", {}).get("passed")), flush=True)
    receipt = {"os": platform.platform(), "linux_distro": args.linux_distro, "results": records}
    (BUILD / "net-closed-checks.json").write_text(json.dumps(receipt, indent=2), encoding="utf8")
    if not all(r["passed"] and r.get("linux", {"passed": True})["passed"] for r in records):
        raise SystemExit("Closed socket checks failed")


if __name__ == "__main__":
    main()
