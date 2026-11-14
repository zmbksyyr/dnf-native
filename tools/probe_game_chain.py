"""Observe original game/DP2 startup with bridge and a copied private MySQL database."""

import argparse
import datetime
import json
import platform
from pathlib import Path
import re
import secrets
import shutil
import socket
import subprocess
import sys
import time

from probe_mysql import tree_identity
from test_bridge import packet, response, sha
from test_channel import request_stop

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database-source", type=Path, required=True,
                        help="Stopped private restored instance containing data/ and game-db.json")
    parser.add_argument("--timeout", type=float, default=2100)
    parser.add_argument("--dp2-cleanup", action="store_true",
                        help="Use early luv cleanup and Gadget full teardown in the private game copy")
    parser.add_argument("--stop-after-started", type=float, metavar="SECONDS",
                        help="Request SIGUSR2 after observing the original startup marker for this long")
    parser.add_argument("--script-db", action="store_true",
                        help="Run only the original JS init_db interface check, then exit 42")
    args = parser.parse_args()
    if args.script_db and args.stop_after_started is not None:
        parser.error("--script-db exits before natural game startup")
    source = args.database_source.resolve()
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
    mysqlbin = ROOT / ".local/mysql-5.7.44/bin"
    pipe = "DNFGameChain" + stamp
    options = ["--no-defaults", "--protocol=PIPE", "--socket=" + pipe, "--user=root"]

    def sql(query):
        result = subprocess.run([str(mysqlbin / "mysql.exe"), *options, "--batch", "--skip-column-names"],
                                input=query.encode("ascii"), capture_output=True, timeout=10)
        if result.returncode:
            (dbroot / "query-error.private.log").write_bytes(result.stderr)
            raise RuntimeError("Private MySQL query failed; see the private log")
        return result.stdout

    def quote(value):
        return "'" + value.replace("\\", "\\\\").replace("'", "''") + "'"

    host, ld, elf = ROOT / "build/i686/dnf-host.exe", guestroot / "lib/ld-2.17.so", guest / "df_bridge_r"
    receipt = {"recorded_utc": stamp, "os": platform.platform(), "database_source": before,
               "host_sha256": sha(host), "bridge_sha256": sha(elf),
               "scope": "Original JS database interface only" if args.script_db else "Original game startup observation; no business acceptance"}
    database = bridge = None
    try:
        command = [str(mysqlbin / "mysqld.exe"), "--no-defaults", "--basedir=" + str(mysqlbin.parent),
                   "--datadir=" + str(dbroot / "data"), "--lower-case-table-names=1", "--character-set-server=latin1",
                   "--bind-address=127.0.0.1", "--port=3306", "--enable-named-pipe", "--socket=" + pipe, "--console"]
        with (dbroot / "server.private.log").open("wb") as log:
            database = subprocess.Popen(command, stdout=log, stderr=log)
        deadline = time.monotonic() + 15
        while True:
            if database.poll() is not None:
                raise RuntimeError("Private MySQL exited during startup")
            check = subprocess.run([str(mysqlbin / "mysql.exe"), *options, "--execute=SELECT 1"],
                                   capture_output=True, timeout=5)
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
        print("Private game chain:", instance, flush=True)
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
    except Exception as failure:
        receipt["error"] = str(failure)
    finally:
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
                result = subprocess.run([str(mysqlbin / "mysqladmin.exe"), *options, "shutdown"], capture_output=True, timeout=15)
                receipt["mysql_shutdown_exit_code"] = result.returncode
                try:
                    database.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    receipt["mysql_forced_cleanup"] = True
                    database.terminate()
                    database.wait(timeout=5)
            receipt["mysql_exit_code"] = database.returncode
        receipt["database_source_unchanged"] = before == tree_identity(source / "data")
        (instance / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        print("Chain receipt:", instance / "receipt.json", flush=True)
    if receipt.get("error"):
        raise SystemExit(receipt["error"])


if __name__ == "__main__":
    main()
