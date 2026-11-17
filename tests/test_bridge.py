"""Check original bridge version and script responses against a private MySQL instance."""

import argparse
import datetime
import hashlib
import json
from pathlib import Path
import platform
import socket
import struct
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from probe_core_protocols import receive
from runtime_support import request_stop, unpack_runtime, process_memory

from bootstrap import BUILD

ROOT = Path(__file__).resolve().parents[1]
BRIDGE_SHA256 = "30a5741015b3701cee2d9b388140ac1711345bbe8d879b6f152e74a9334ea6b0"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def packet(category, identifier, body=b""):
    return struct.pack("<BBIIB", category, identifier, 11 + len(body), 0, 0) + body


def response(peer, identifier, size):
    header = receive(peer, 11)
    # These original response constructors set category, ID and size only.
    # The remaining header bytes are uninitialized; they are not an ACK.
    if header[:6] != struct.pack("<BBI", 0x7c, identifier, 11 + size):
        raise RuntimeError("Bridge response type or size differs")
    return receive(peer, size)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db-config", type=Path, required=True,
                        help="Private JSON: host, port, user, password, database (plain bridge credentials)")
    parser.add_argument("--port", type=int, default=7000, help="Private loopback TCP/UDP service port")
    parser.add_argument("--host", type=Path, default=BUILD / "i686/dnf-host.exe")
    parser.add_argument("--client", type=Path, default=BUILD / "mysql-tools/mysql/bin/mysql.exe")
    parser.add_argument("--game-proxy", action="store_true", help="Also call the original game channel proxy through DP2/Frida")
    args = parser.parse_args()
    db = json.loads(args.db_config.read_text(encoding="utf-8"))
    if db["port"] != 3306:
        parser.error("This original bridge ELF connects to MySQL port 3306")
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    instance = BUILD / "probes" / stamp / "bridge"
    instance.mkdir(parents=True)
    unpack_runtime(instance, "i686", ("lib/", "usr/", "home/neople/bridge/"))
    guest = instance / "home/neople/bridge"
    for name in ["log", "pid"]:
        (guest / name).mkdir(exist_ok=True)
    config = guest / "cfg/bridge.cfg"
    config.write_text("[server]\nmax_client = 6500\nthis_ip = 127.0.0.1\nthis_udp_port = " + str(args.port) +
                      "\nthis_tcp_port = " + str(args.port) + "\n[DB]\ndb_ip = " + db["host"] +
                      "\ndb_name = " + db["database"] + "\ndb_id = " + db["user"] + "\ndb_pwd = " +
                      db["password"] + "\n", encoding="ascii")
    # Keep credentials out of process arguments and the receipt. Use the
    # pinned official client to establish an independent expected DB version.
    client_config = instance / "mysql.private.ini"
    client_config.write_text("[client]\nprotocol=TCP\nhost=" + db["host"] + "\nport=3306\nuser=" + db["user"] +
                             "\npassword=\"" + db["password"].replace("\\", "\\\\").replace('"', '\\"') +
                             "\"\n", encoding="ascii")
    query = subprocess.run([str(args.client.resolve(strict=True)), "--defaults-file=" + str(client_config),
                            "--batch", "--skip-column-names", "--database=" + db["database"],
                            "--execute=SELECT script_version FROM ch_script_version;"],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
    (instance / "mysql.private.log").write_bytes(query.stderr)
    if query.returncode or len(query.stdout.splitlines()) != 1:
        raise SystemExit("Private version query failed; see the private instance logs")
    version = query.stdout.splitlines()[0]
    expected_script = (guest / "cfg/channel_info.etc").read_bytes()
    # ReloadScript preserves the original length but replaces a final LF/CRLF
    # with zero bytes before onCS_GET_SCRIPT attaches the stream.
    if expected_script.endswith(b"\r\n"):
        expected_script = expected_script[:-2] + bytes(2)
    elif expected_script.endswith(b"\n"):
        expected_script = expected_script[:-1] + bytes(1)
    elf, ld, host = guest / "df_bridge_r", instance / "lib/ld-2.17.so", args.host.resolve()
    if sha(elf) != BRIDGE_SHA256:
        raise ValueError("This check requires the recorded original bridge ELF")
    command = [str(host), "--root", str(instance), sha(ld), "--env", "HOME=/home/neople",
               "--cwd", "/home/neople/bridge", "--run", str(elf), sha(elf),
               "/home/neople/bridge/df_bridge_r", "bridge", "run"]
    checks = {"different_version": False, "same_version": False, "fragmented_version": False,
              "coalesced_requests": False, "script_responses": 0}
    error, forced, stop_sent, game_proxy, memory = None, False, False, None, None
    with (instance / "stdout.private.log").open("wb") as out, (instance / "stderr.log").open("wb") as err:
        process = subprocess.Popen(command, stdout=out, stderr=err)
        try:
            deadline = time.monotonic() + 15
            while True:
                if process.poll() is not None:
                    raise RuntimeError("Bridge exited during startup")
                try:
                    peer = socket.create_connection(("127.0.0.1", args.port), timeout=0.2)
                    break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise RuntimeError("Bridge TCP startup timeout")
                    time.sleep(0.1)
            with peer:
                peer.settimeout(5)

                def version_response(same):
                    data = response(peer, 6, 20)
                    if struct.unpack_from("<I", data)[0] != int(same) or data[4:].split(b"\0", 1)[0] != version:
                        raise RuntimeError("Bridge database version response differs")

                different = b"!" * 16 if version != b"!" * 16 else b"?" * 16
                peer.sendall(packet(1, 5, different))
                version_response(False)
                checks["different_version"] = True
                same = packet(1, 5, version.ljust(16, b"\0"))
                peer.sendall(same)
                version_response(True)
                checks["same_version"] = True
                peer.sendall(same[:5])
                time.sleep(0.05)
                peer.sendall(same[5:])
                version_response(True)
                checks["fragmented_version"] = True
                for _ in range(3):
                    peer.sendall(packet(2, 9))
                    if response(peer, 10, len(expected_script)) != expected_script:
                        raise RuntimeError("Bridge script bytes differ from its original file")
                    checks["script_responses"] += 1
                peer.sendall(same + packet(2, 9))
                version_response(True)
                if response(peer, 10, len(expected_script)) != expected_script:
                    raise RuntimeError("Bridge coalesced script response differs")
                checks["script_responses"] += 1
                checks["coalesced_requests"] = True
            memory = process_memory(process.pid)
            if args.game_proxy:
                subprocess.run([sys.executable, "-B", str(ROOT / "tests/game_fixture.py"), "--timeout", "40",
                                "--lua-script", str(ROOT / "tests/channel_proxy.lua"),
                                "--frida-script", str(ROOT / "tests/channel_proxy.js"),
                                "--env", "DNF_NATIVE_BRIDGE_PORT=" + str(args.port)], cwd=ROOT, check=True)
                probe = json.loads((BUILD / "game-probe.json").read_text(encoding="utf-8"))
                game_root = Path(probe["command"][2])
                downloaded = game_root / "home/neople/game/channel_info"
                game_proxy = {key: probe[key] for key in ["host_sha256", "elf_sha256", "exit_code", "forced_cleanup",
                                                         "elapsed_seconds", "syscall_gaps", "runtime_gaps"]}
                game_proxy["probe"] = str(game_root.relative_to(BUILD))
                game_proxy["fixture_sha256"] = {name: sha(ROOT / "tests" / name) for name in ["channel_proxy.lua", "channel_proxy.js"]}
                game_proxy["script_matches"] = ((downloaded / "channel_info.etc").read_bytes() == expected_script)
                game_proxy["version_matches"] = ((downloaded / "version").read_bytes() == version)
                game_proxy["log_sha256"] = {name: sha(game_root / name) for name in ["stdout.log", "stderr.log"]}
                game_proxy["passed"] = (probe["exit_code"] == 42 and not probe["forced_cleanup"] and not probe["runtime_gaps"] and
                                        game_proxy["script_matches"] and game_proxy["version_matches"])
                if not game_proxy["passed"]:
                    raise RuntimeError("Original game channel proxy acquisition failed")
        except Exception as failure:
            error = str(failure)
        finally:
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
            ["syscall gap", "unsupported", "unhandled exception", "decode gap", "unimplemented guest", "exhausted"])]
    result = {"recorded_utc": stamp, "os": platform.platform(), "probe": str(instance.relative_to(BUILD)),
              "scope": "Original bridge DB version checks and exact channel-script responses on loopback",
              "command": command, "host_sha256": sha(host), "elf_sha256": sha(elf), "ld_sha256": sha(ld),
               "service_port": args.port, "checks": checks, "script_bytes": len(expected_script), "memory": memory,
              "game_proxy": game_proxy,
              "script_sha256": hashlib.sha256(expected_script).hexdigest(), "error": error, "gaps": gaps,
              "exit_code": process.returncode, "sigterm_event_sent": stop_sent, "forced_cleanup": forced,
              "log_sha256": {p.name: sha(p) for p in instance.glob("*.log")}}
    (BUILD / "bridge-checks.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    passed = not error and not gaps and not forced and stop_sent and process.returncode == 0
    print("Original bridge TCP:", "PASS" if passed else "FAIL", checks, "exit", process.returncode)
    if not passed:
        raise SystemExit("See ../x/dnf-native-output/bridge-checks.json and the private logs")


if __name__ == "__main__":
    main()
