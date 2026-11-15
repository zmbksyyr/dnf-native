"""Observe original game/DP2 startup with bridge and a copied private MySQL database."""

import argparse
import ctypes
import datetime
import json
import os
import platform
from pathlib import Path
import re
import secrets
import shutil
import socket
import subprocess
import sys
import threading
import time

from probe_mysql import tree_identity
from test_bridge import packet, response, sha
from test_channel import request_stop

ROOT = Path(__file__).resolve().parents[1]


def udp_owner(port):
    api = ctypes.WinDLL("iphlpapi").GetExtendedUdpTable
    size = ctypes.c_ulong()
    api(None, ctypes.byref(size), False, 2, 1, 0)
    buffer = ctypes.create_string_buffer(size.value)
    if api(buffer, ctypes.byref(size), False, 2, 1, 0):
        return None
    words = memoryview(buffer).cast("B").cast("I")
    for index in range(words[0]):
        if socket.ntohs(words[2 + index * 3] & 0xffff) == port:
            return words[3 + index * 3]
    return None


def start_core(instance, db, services, relay_s2s_port=8200):
    from probe_monitor import private_elf, UDP_STOP_PATCHES, STATISTICS_PATCHES
    monitor_patches = UDP_STOP_PATCHES + STATISTICS_PATCHES
    entries = [("manager", "manager", "nofork"), ("dbmw_guild", "dbmw_gld_siroco", "nofork"),
               ("dbmw_mnt", "dbmw_mnt_siroco", "nofork"), ("dbmw_stat", "dbmw_stat_siroco", "nofork"),
               ("monitor", "mnt_siroco", "nofork"), ("guild", "gld_siroco", "nofork"),
               ("statics", "stat_siroco", "nofork"), ("coserver", "coserver", "nofork"),
               ("community", "community", "nofork"), ("auction", "auction_siroco", "run"),
               ("point", "point_siroco", "run"), ("channel", "channel", "run"),
               ("relay", "relay_200", "run"), ("stun", "test", None)]
    for name, config_name, action in entries:
        private = instance / "core" / name
        arch = "x86_64" if name == "stun" else "i686"
        for directory in (["lib64", "usr"] if name == "stun" else ["lib", "usr"]):
            shutil.copytree(ROOT / ".local/sysroot" / arch / directory, private / directory)
        guest = private / "home/neople" / name
        shutil.copytree(ROOT / "originals/dp123/home/neople" / name, guest)
        for directory in ["log", "pid"]:
            (guest / directory).mkdir(exist_ok=True)
        for config in (guest / "cfg").glob("*.cfg"):
            text = config.read_text(encoding="utf-8")
            text = re.sub(r"(?<!\d)(?:\d{1,3}\.){3}\d{1,3}(?!\d)", "127.0.0.1", text)
            if name == "channel":
                text = re.sub(r"(?m)^this_ip\s*=.*$", "this_ip = 127.0.0.1", text)
            if name in ["channel", "relay"]:
                text = re.sub(r"(?m)^max_client\s*=.*$", "max_client = 16", text)
            if name == "relay":
                text = re.sub(r"(?m)^udp_port_to_monitor\s*=.*$", "udp_port_to_monitor = " + str(relay_s2s_port), text)
            fields = []
            if name.startswith("dbmw") and config.stem == config_name:
                fields = [("master_db_" + key, value) for key, value in
                          [("ip", "host"), ("port", "port"), ("acc", "user"), ("pwd", "password"), ("name", "database")]]
            elif name in ["auction", "point"] and config.stem == config_name:
                fields = [(prefix + "_" + key, value) for prefix in ["game_db", "auction_db"]
                          for key, value in [("ip", "host"), ("port", "port"), ("acc", "user"), ("pwd", "password")]]
            for field, key in fields:
                text, count = re.subn(r"(?m)^" + field + r"[^\S\r\n]*=.*$",
                                      lambda match: field + " = " + str(db[key]), text)
                if count != 1:
                    raise RuntimeError("Original configuration field not found: " + field)
            config.write_text(text, encoding="utf-8")
        elf = guest / ("df_dbmw_r" if name.startswith("dbmw") else "df_" + name + "_r")
        original_sha = sha(elf)
        if name == "monitor":
            private_elf(ROOT / "originals/dp123/home/neople/monitor/df_monitor_r", elf, monitor_patches)
        arguments = [config_name] + ([action] if action else [])
        if name in ["auction", "point"]:
            arguments = ["./cfg/" + config_name + ".cfg", "run", "./" + elf.name]
        host = ROOT / "build" / arch / "dnf-host.exe"
        ld = private / ("lib64" if name == "stun" else "lib") / "ld-2.17.so"
        command = [str(host), "--root", str(private), sha(ld), "--env", "HOME=/home/neople", "--cwd",
                   "/home/neople/" + name, "--run", str(elf), sha(elf), "/home/neople/" + name + "/" + elf.name, *arguments]
        record = {"name": name, "host_sha256": sha(host), "original_elf_sha256": original_sha,
                  "executed_elf_sha256": sha(elf), "arguments": arguments,
                  "configuration_sha256": {str(p.relative_to(guest)): sha(p)
                                            for p in sorted((guest / "cfg").rglob("*")) if p.is_file()}}
        if name == "monitor":
            record["patches"] = monitor_patches
        with (private / "stdout.private.log").open("wb") as out, (private / "stderr.log").open("wb") as err:
            process = subprocess.Popen(command, stdout=out, stderr=err)
        services.append((process, private, record))
        print("Core started", name, "pid", process.pid, flush=True)
        try:
            process.wait(timeout=20 if name in ["auction", "point"] else 1)
        except subprocess.TimeoutExpired:
            continue
        raise RuntimeError(name + " exited before game startup")


def stop_core(services):
    for process, private, record in reversed(services):
        record["forced_cleanup"] = False
        record["exited_before_stop"] = process.poll() is not None
        if process.poll() is None:
            signal = 12 if record["name"] in ["auction", "point", "community"] else 15
            record["stop_signal"] = signal
            request_stop(process.pid, signal)
            if record["name"] == "stun":
                # Default SIGTERM is observed once its blocking recv wakes.
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as wake:
                    wake.sendto(b"\x01", ("127.0.0.1", 2311))
            try:
                process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                record["forced_cleanup"] = True
                process.terminate()
                process.wait(timeout=5)
        record["exit_code"] = process.returncode
        stderr = (private / "stderr.log").read_text(encoding="utf-8", errors="replace")
        stdout = (private / "stdout.private.log").read_text(encoding="utf-8", errors="replace")
        record["gaps"] = [line for line in stderr.splitlines()
                          if any(t in line for t in ["syscall gap", "unhandled exception", "unsupported", "exhausted"])]
        markers = ["DBMW_ALL_DB Open Success", "Application Init() Success!", "Application Load() Success!",
                   "Application End!", "Server Terminated", "Guild And Guild Member DB Save Success!",
                   "Community Server(IP:127.0.0.1, PORT:31100)", "Recv SIGTERM signal", "Application Stop!"]
        record["markers"] = [marker for marker in markers if marker in stdout]
        record["logs"] = {name: sha(private / name) for name in ["stderr.log", "stdout.private.log"]}
        print("Core exit", record["name"], process.returncode, "forced", record["forced_cleanup"], flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database-source", type=Path, required=True,
                        help="Stopped private restored instance containing data/ and game-db.json")
    parser.add_argument("--mysql-standalone", type=Path,
                        default=ROOT / "build/mysql-tools/runtime/mysql-control.exe",
                        help="Independent single EXE used for every official MySQL tool invocation")
    parser.add_argument("--timeout", type=float, default=2100)
    parser.add_argument("--dp2-cleanup", action="store_true",
                        help="Use early luv cleanup and Gadget full teardown in the private game copy")
    parser.add_argument("--stop-after-started", type=float, metavar="SECONDS",
                        help="Request SIGUSR2 after observing the original startup marker for this long")
    parser.add_argument("--script-db", action="store_true",
                        help="Run only the original JS init_db interface check, then exit 42")
    parser.add_argument("--core", action="store_true",
                        help="Observe the other 14 core services alongside bridge/game; requires log review, not business acceptance")
    parser.add_argument("--peers-only-seconds", type=float,
                        help="Observe bridge and the other 14 core services for a short diagnosis, without game")
    parser.add_argument("--trace-relay-monitor", action="store_true",
                        help="Diagnostic only: forward actual monitor replies from 8200 to private relay 18200 and record headers")
    args = parser.parse_args()
    if args.peers_only_seconds is not None:
        if args.peers_only_seconds <= 0 or args.script_db or args.stop_after_started is not None:
            parser.error("--peers-only-seconds requires a positive duration and no game observation options")
        args.core = True
    if args.script_db and args.stop_after_started is not None:
        parser.error("--script-db exits before natural game startup")
    if args.core and args.script_db:
        parser.error("--core requires natural game startup")
    if args.trace_relay_monitor and not args.core:
        parser.error("--trace-relay-monitor requires core peers")
    source = args.database_source.resolve()
    mysql_exe = args.mysql_standalone.resolve(strict=True)
    # Both ports are fixed by the current original bridge/game configuration.
    for port in [3306, 7000]:
        with socket.socket() as check:
            check.bind(("127.0.0.1", port))
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    instance = ROOT / "build/probes" / stamp / "game-chain"
    dbroot, guestroot = instance / "mysql", instance / "bridge"
    dbroot.mkdir(parents=True)
    before = tree_identity(source / "data")
    shutil.copytree(source / "data", dbroot / "data")
    if before != tree_identity(dbroot / "data"):
        raise RuntimeError("Private database copy differs from its source")
    for name in ["lib", "usr"]:
        shutil.copytree(ROOT / ".local/sysroot/i686" / name, guestroot / name)
    guest = guestroot / "home/neople/bridge"
    shutil.copytree(ROOT / "originals/dp123/home/neople/bridge", guest)
    for name in ["log", "pid"]:
        (guest / name).mkdir(exist_ok=True)
    password = secrets.token_hex(16)
    (guest / "cfg/bridge.cfg").write_text(
        "[server]\nmax_client = 6500\nthis_ip = 127.0.0.1\nthis_udp_port = 7000\nthis_tcp_port = 7000\n"
        "[DB]\ndb_ip = 127.0.0.1\ndb_name = d_channel\ndb_id = dnf_bridge\ndb_pwd = " + password + "\n",
        encoding="ascii")
    mysql_env = dict(os.environ)
    mysql_env["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32")
    for name in ["MYSQL_HOME", "MYSQL_PWD", "MYSQL_TCP_PORT"]:
        mysql_env.pop(name, None)
    pipe = "DNFGameChain" + stamp
    options = ["--no-defaults", "--protocol=PIPE", "--socket=" + pipe, "--user=root"]
    if (source / "client.private.cnf").is_file():
        shutil.copy2(source / "client.private.cnf", dbroot / "client.private.cnf")
        options = ["--defaults-file=client.private.cnf", "--protocol=PIPE", "--socket=" + pipe, "--ssl-mode=DISABLED"]

    def mysql_command(tool):
        return [str(mysql_exe), tool, str(dbroot)]

    def sql(query):
        result = subprocess.run(mysql_command("mysql") + options + ["--batch", "--skip-column-names"],
                                input=query.encode("ascii"), capture_output=True, env=mysql_env, timeout=10)
        if result.returncode:
            (dbroot / "query-error.private.log").write_bytes(result.stderr)
            raise RuntimeError("Private MySQL query failed; see the private log")
        return result.stdout

    def quote(value):
        return "'" + value.replace("\\", "\\\\").replace("'", "''") + "'"

    host, ld, elf = ROOT / "build/i686/dnf-host.exe", guestroot / "lib/ld-2.17.so", guest / "df_bridge_r"
    receipt = {"recorded_utc": stamp, "os": platform.platform(), "database_source": before,
               "probe_script_sha256": sha(Path(__file__)),
               "host_sha256": sha(host), "bridge_sha256": sha(elf),
               "mysql_bootstrap_sha256": sha(mysql_exe),
               "mysql_private_credentials": (source / "client.private.cnf").is_file(),
               "scope": "Original JS database interface only" if args.script_db else "Original game startup observation; no business acceptance"}
    if args.peers_only_seconds is not None:
        receipt["scope"] = "Fifteen original core peers without game; initialization and database diagnosis only"
    database = bridge = None
    relay_proxy = proxy_thread = None
    proxy_stop = threading.Event()
    core_services = []
    if args.core:
        receipt["core_services"] = []
        receipt["protocol_observations"] = {}
        receipt["core_configuration"] = {"addresses": "loopback", "channel_relay_max_client": 16,
                                         "monitor": "private UDP shutdown and initial minute counter patches", "stun": "test mode; default signal exit"}
    def check_core_protocols(label):
        path = instance / ("protocols-" + label + ".json")
        result = subprocess.run([sys.executable, str(ROOT / "tools/probe_core_protocols.py"),
                                 "--channel-port", "7001", "--relay-monitor-auth", "--output", str(path)],
                                cwd=ROOT, capture_output=True, timeout=45)
        receipt["protocol_observations"][label] = json.loads(path.read_text(encoding="utf-8"))
        if result.returncode:
            raise RuntimeError("Core protocol observation failed: " + label)

    try:
        prepared = subprocess.run(mysql_command("prepare"), capture_output=True, env=mysql_env,
                                  timeout=60, check=True)
        payload = Path(prepared.stdout.decode("utf-8").strip())
        receipt["mysql_official_tool_sha256"] = {name: sha(payload / "bin" / (name + ".exe"))
                                                 for name in ["mysqld", "mysql", "mysqladmin", "mysqldump"]}
        command = mysql_command("mysqld") + ["--no-defaults", "--lower-case-table-names=1", "--character-set-server=latin1",
                   "--bind-address=127.0.0.1", "--port=3306", "--enable-named-pipe", "--socket=" + pipe, "--console"]
        with (dbroot / "server.private.log").open("wb") as log:
            database = subprocess.Popen(command, stdout=log, stderr=log, env=mysql_env)
        deadline = time.monotonic() + 15
        while True:
            if database.poll() is not None:
                raise RuntimeError("Private MySQL exited during startup")
            check = subprocess.run(mysql_command("mysql") + options + ["--execute=SELECT 1"],
                                   capture_output=True, env=mysql_env, timeout=5)
            if check.returncode == 0:
                break
            if time.monotonic() >= deadline:
                raise RuntimeError("Private MySQL startup timeout")
            time.sleep(0.1)
        sql("CREATE USER 'dnf_bridge'@'127.0.0.1' IDENTIFIED BY " + quote(password) +
            "; GRANT SELECT ON d_channel.* TO 'dnf_bridge'@'127.0.0.1';")
        script = (ROOT / "originals/dp123/dp2/df_game_r.js").read_text(encoding="utf-8")
        # This script has its own account, independent of game master_db/db_connect.
        user = re.search(r"var db_account_ptr = Memory.allocUtf8String\('([^']*)'\)", script)[1]
        script_password = re.search(r"var db_password_ptr = Memory.allocUtf8String\('([^']*)'\)", script)[1]
        account = quote(user) + "@'127.0.0.1'"
        sql("CREATE USER " + account + " IDENTIFIED BY " + quote(script_password) +
            "; GRANT ALL ON d_taiwan.* TO " + account + "; GRANT ALL ON frida.* TO " + account + ";")
        command = [str(host), "--root", str(guestroot), sha(ld), "--env", "HOME=/home/neople", "--cwd",
                   "/home/neople/bridge", "--run", str(elf), sha(elf), "/home/neople/bridge/df_bridge_r", "bridge", "run"]
        with (guestroot / "stdout.private.log").open("wb") as out, (guestroot / "stderr.log").open("wb") as err:
            bridge = subprocess.Popen(command, stdout=out, stderr=err)
        deadline = time.monotonic() + 15
        while True:
            if bridge.poll() is not None:
                raise RuntimeError("Bridge exited during startup")
            try:
                peer = socket.create_connection(("127.0.0.1", 7000), timeout=0.2)
                break
            except OSError:
                if time.monotonic() >= deadline:
                    raise RuntimeError("Bridge TCP startup timeout")
                time.sleep(0.1)
        with peer:
            peer.settimeout(5)
            peer.sendall(packet(2, 9))
            expected = (guest / "cfg/channel_info.etc").read_bytes()
            receipt["bridge_script_matches"] = response(peer, 10, len(expected)) == expected
            if not receipt["bridge_script_matches"]:
                raise RuntimeError("Bridge script differs from its original file")
        command = [sys.executable, str(ROOT / "tools/probe_game.py"), "--preload", "--timeout", str(args.timeout)]
        if args.dp2_cleanup:
            command.append("--dp2-cleanup")
        if args.stop_after_started is not None:
            command += ["--stop-after-started", str(args.stop_after_started)]
        if args.script_db:
            js, lua = instance / "script-db.js", instance / "script-db.lua"
            js.write_text(script + "\nvar savedInit=rpc.exports.init; rpc.exports.init=function(){ "
                          "savedInit.apply(this,arguments); init_db(); if(mysql_d_taiwan===null || mysql_frida===null || "
                          "api_MySQL_exec(mysql_frida,'SELECT 1')!==1) throw new Error('Original script database test failed'); "
                          "console.log('dnf-native-original-script-db-ok'); };\n", encoding="utf-8")
            lua.write_text("local frida=require('df.frida'); frida.load(); "
                           "print('dnf-native-original-script-db-returned'); os.exit(42)\n", encoding="utf-8")
            command += ["--lua-script", str(lua), "--frida-script", str(js)]
        else:
            db = json.loads((source / "game-db.json").read_text(encoding="utf-8"))
            db["port"] = 3306
            sql("UPDATE d_taiwan.db_connect SET db_ip='127.0.0.1', db_port=3306;")
            config = dbroot / "game-db.json"
            config.write_text(json.dumps(db), encoding="utf-8")
            command += ["--db-config", str(config)]
        if args.core:
            month = sql("SELECT DATE_FORMAT(NOW(),'%Y%m');").decode("ascii").strip()
            receipt["monthly_history_tables"] = []
            for schema in ["taiwan_cain_auction_gold", "taiwan_cain_auction_cera"]:
                for table in ["auction_history", "auction_history_buyer"]:
                    name = schema + "." + table + "_" + month
                    sql("CREATE TABLE IF NOT EXISTS " + name + " LIKE " + schema + "." + table + ";")
                    receipt["monthly_history_tables"].append(name)
            if args.peers_only_seconds is not None:
                receipt["statistics_since"] = sql("SELECT NOW();").decode("ascii").strip()
            if args.trace_relay_monitor:
                relay_proxy = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                relay_proxy.bind(("127.0.0.1", 8200))
                relay_proxy.settimeout(0.2)
                receipt["relay_monitor_trace"] = []

                def forward_monitor():
                    while not proxy_stop.is_set():
                        try:
                            data, address = relay_proxy.recvfrom(4096)
                        except socket.timeout:
                            continue
                        except ConnectionResetError:
                            # Monitor may send before relay binds 18200. Windows
                            # reports that earlier ICMP refusal on this UDP recv.
                            continue
                        owner = udp_owner(address[1])
                        service = next((record["name"] for process, private, record in core_services if process.pid == owner), None)
                        receipt["relay_monitor_trace"].append({"source_port": address[1], "source_service": service,
                                                                "bytes": len(data), "header_hex": data[:15].hex()})
                        relay_proxy.sendto(data, ("127.0.0.1", 18200))

                proxy_thread = threading.Thread(target=forward_monitor)
                proxy_thread.start()
            start_core(instance, db, core_services, 18200 if args.trace_relay_monitor else 8200)
            check_core_protocols("before_observation")
        print("Private game chain:", instance, flush=True)
        if args.peers_only_seconds is not None:
            deadline = time.monotonic() + args.peers_only_seconds
            while time.monotonic() < deadline:
                for process, private, record in core_services:
                    if process.poll() is not None:
                        raise RuntimeError(record["name"] + " exited during peer observation")
                time.sleep(0.2)
            receipt["peer_observation_seconds"] = args.peers_only_seconds
            receipt["occupation_statistics"] = {}
            for schema in ["taiwan_cain", "taiwan_cain_log"]:
                rows = sql("SELECT occ_time,num_occupations_charscreen,num_occupations_seriaroom,"
                           "num_login_per_min,num_logout_per_min FROM " + schema + ".log_num_occupations "
                           "WHERE occ_time >= " + quote(receipt["statistics_since"]) +
                           " ORDER BY occ_time LIMIT 5;")
                receipt["occupation_statistics"][schema] = rows.decode("ascii").splitlines()
        else:
            subprocess.run(command, cwd=ROOT, check=True)
            probe = json.loads((ROOT / "build/game-probe.json").read_text(encoding="utf-8"))
            (instance / "game-receipt.json").write_text(json.dumps(probe, indent=2) + "\n", encoding="utf-8")
            receipt["game"] = {key: probe[key] for key in ["host_sha256", "elf_sha256", "exit_code", "forced_cleanup",
                                                          "elapsed_seconds", "syscall_gaps", "runtime_gaps", "dp2_cleanup",
                                                          "startup_seen_after_seconds", "stop_signal",
                                                          "observation_after_start_seconds", "shutdown_markers"]}
            receipt["game"]["probe"] = str(Path(probe["command"][2]).relative_to(ROOT))
            if args.script_db:
                receipt["script_db_tables"] = sql("SHOW TABLES FROM frida;").decode("ascii").splitlines()
                if probe["exit_code"] != 42 or probe["forced_cleanup"] or receipt["script_db_tables"] != ["data", "dgnfinishtime", "init_sp_tp"]:
                    raise RuntimeError("Original JS database interface check failed")
            elif args.stop_after_started is not None:
                if (probe["exit_code"] != 0 or probe["forced_cleanup"] or probe["stop_signal"] != 12 or
                        probe["shutdown_markers"] != ["RECV SIGUSR2", "ALL USER LOGOUT COMPLETE", "GAME SERVER SHUTDOWN COMPLETE"]):
                    raise RuntimeError("Original game did not complete startup and SIGUSR2 shutdown")
        if args.core:
            check_core_protocols("after_observation")
    except Exception as failure:
        receipt["error"] = str(failure)
    finally:
        stop_core(core_services)
        if proxy_thread:
            proxy_stop.set()
            proxy_thread.join(timeout=2)
            relay_proxy.close()
        if args.core:
            receipt["core_services"] = [record for process, private, record in core_services]
        if bridge:
            if bridge.poll() is None:
                request_stop(bridge.pid)
                try:
                    bridge.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    receipt["bridge_forced_cleanup"] = True
                    bridge.terminate()
                    bridge.wait(timeout=5)
            receipt["bridge_exit_code"] = bridge.returncode
        if database:
            if database.poll() is None:
                result = subprocess.run(mysql_command("mysql") + options, input=b"SHUTDOWN;\n",
                                        capture_output=True, env=mysql_env, timeout=15)
                receipt["mysql_shutdown_method"] = "SQL SHUTDOWN"
                receipt["mysql_shutdown_exit_code"] = result.returncode
                receipt["mysql_shutdown_client_lost_ack"] = result.returncode == 1 and b"ERROR 2013 (HY000)" in result.stderr
                (dbroot / "shutdown.private.log").write_bytes(result.stdout + result.stderr)
                try:
                    database.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    receipt["mysql_forced_cleanup"] = True
                    subprocess.run(["taskkill.exe", "/PID", str(database.pid), "/T", "/F"],
                                   capture_output=True, timeout=10)
                    database.wait(timeout=5)
            receipt["mysql_exit_code"] = database.returncode
            receipt["mysql_shutdown_complete"] = database.returncode == 0 and b"Shutdown complete" in (dbroot / "server.private.log").read_bytes()
        receipt["database_source_unchanged"] = before == tree_identity(source / "data")
        (instance / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        print("Chain receipt:", instance / "receipt.json", flush=True)
    if receipt.get("error"):
        raise SystemExit(receipt["error"])
    if (not receipt.get("mysql_shutdown_complete") or
            (receipt.get("mysql_shutdown_exit_code") != 0 and not receipt.get("mysql_shutdown_client_lost_ack")) or
            receipt.get("mysql_forced_cleanup") or receipt.get("bridge_exit_code") != 0 or
            receipt.get("bridge_forced_cleanup") or not receipt["database_source_unchanged"]):
        raise SystemExit("Database or bridge lifecycle failed; inspect its receipt and private logs")
    if any(record["exited_before_stop"] or record["forced_cleanup"] or record["gaps"]
           for process, private, record in core_services):
        raise SystemExit("Core peer observation has failures; inspect its receipt and application logs")


if __name__ == "__main__":
    main()
