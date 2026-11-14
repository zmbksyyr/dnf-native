"""Prepare a new private original-ELF instance for the native GUI (development)."""

import argparse
import configparser
import datetime
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import socket
import subprocess
import time

from bootstrap import sha256
from probe_monitor import private_elf, UDP_STOP_PATCHES, STATISTICS_PATCHES
from probe_mysql import tree_identity

ROOT = Path(__file__).resolve().parents[1]
# Dependency order, argv, shutdown signal. Security components remain optional.
ENTRIES = [
    ("bridge", "bridge", "run", 15), ("manager", "manager", "nofork", 15),
    ("dbmw_guild", "dbmw_gld_siroco", "nofork", 15),
    ("dbmw_mnt", "dbmw_mnt_siroco", "nofork", 15),
    ("dbmw_stat", "dbmw_stat_siroco", "nofork", 15),
    ("monitor", "mnt_siroco", "nofork", 15), ("guild", "gld_siroco", "nofork", 15),
    ("statics", "stat_siroco", "nofork", 15), ("coserver", "coserver", "nofork", 15),
    ("community", "community", "nofork", 12), ("auction", "auction_siroco", "run", 12),
    ("point", "point_siroco", "run", 12), ("channel", "channel", "run", 15),
    ("relay", "relay_200", "run", 15), ("stun", "test", None, 15),
    ("game", "siroco11", "nofork", 12),
    ("zergsvr", None, None, 15), ("secagent", None, None, 15), ("gunnersvr", None, None, 15),
]


def prepare_item_reset(script, game):
    """Replace only the pinned package's hot JS reset; preserve its semantics."""
    source_sha = sha256(script)
    game_sha = sha256(game)
    if (source_sha != "180bb49b77eeb96c88473d668b95eb0fe383c7a0b050fc5815ccbf39a8534f0b" or
            game_sha != "da417c5e65918f655afcddb6edfdaf2fa1e77b2f208e5aa397f021fa23b79979"):
        raise RuntimeError("Native item reset requires the pinned original game and script")
    old = ("Interceptor.replace(ptr(0x080CB7D8), new NativeCallback(function (Inven_Item) {//装备全字节删除\n"
           "        var MReset = Memory.alloc(61)\n"
           "        Memory.copy(Inven_Item, MReset, 61)\n"
           "        return Inven_Item;\n"
           "    }, 'pointer', ['pointer']));")
    dispose = "dispose: function () {\n"
    text = script.read_text(encoding="utf-8")
    if text.count(old) != 1 or text.count(dispose) != 1:
        raise RuntimeError("Packaged item reset or dispose block differs")
    helper = ROOT / "runtime/frida_item_reset.js"
    text = text.replace(old, "dnf_native_install_item_reset();")
    text = text.replace(dispose, dispose + "        dnf_native_restore_item_reset();\n")
    script.write_text(helper.read_text(encoding="utf-8") + "\n" + text, encoding="utf-8", newline="\n")
    return {"source_script_sha256": source_sha, "game_sha256": game_sha,
            "helper_sha256": sha256(helper), "prepared_script_sha256": sha256(script),
            "address": "0x080cb7d8", "original_bytes": "5589e583ec188b4508c600008b4508c64001008b4508",
            "replacement_bytes": "578b7c240889fa31c0b93d000000fcf3aa89d05fc390",
            "scope": "Same 61-byte reset as packaged JS, restored during dispose; disk ELF unchanged"}


def copy_service_resources(output, db, bridge_password, peers_only=False):
    records = []
    for arch in ["i686", "x86_64"]:
        root = output / arch
        for folder in (["lib", "usr"] if arch == "i686" else ["lib64", "usr"]):
            shutil.copytree(ROOT / ".local/sysroot" / arch / folder, root / folder)
        (output / "hosts" / arch).mkdir(parents=True)
        shutil.copy2(ROOT / "build" / arch / "dnf-host.exe", output / "hosts" / arch / "dnf-host.exe")
    root = output / "i686"
    for folder in ["tmp", "dev/shm/sec"]:
        (root / folder).mkdir(parents=True)
    shutil.copytree(ROOT / "originals/dp123/usr/local/share/GeoIP", root / "usr/local/share/GeoIP")
    for name in ["libnxencryption.so", "libGeoIP.so.1"]:
        shutil.copy2(ROOT / "originals/dp123/lib" / name, root / "lib" / name)
    shutil.copytree(ROOT / "originals/dp123/dp2", root / "dp2")
    with (root / "dp2/df_game_r.lua").open("ab") as script:
        script.write(b"\n" + (ROOT / "runtime/dp2_cleanup.lua").read_bytes())
    config = root / "dp2/frida/frida.config"
    settings = json.loads(config.read_text(encoding="utf-8")); settings["teardown"] = "full"
    config.write_text(json.dumps(settings, indent=2) + "\n", encoding="utf-8")
    shutil.copytree(ROOT / "originals/dp123/home/neople/secsvr", root / "home/neople/secsvr")
    ini = configparser.ConfigParser(interpolation=None)
    ini["services"] = {"version": "1", "count": str(len(ENTRIES))}
    for index, (name, config_name, action, signal) in enumerate(ENTRIES):
        arch = "x86_64" if name == "stun" else "i686"
        service_root = output / arch
        optional = index >= 16
        guest_dir = "/home/neople/" + ("secsvr/" + ("gunnersvr" if name == "gunnersvr" else "zergsvr") if optional else name)
        guest = service_root / guest_dir.lstrip("/")
        if not optional:
            shutil.copytree(ROOT / "originals/dp123/home/neople" / name, guest,
                            ignore=shutil.ignore_patterns("log", "pid", "history"))
        for folder in ["log", "pid", "history"]:
            (guest / folder).mkdir(exist_ok=True)
        elf = guest / (name if optional else "df_dbmw_r" if name.startswith("dbmw") else "df_" + name + "_r")
        original_sha = sha256(elf)
        patches = UDP_STOP_PATCHES + STATISTICS_PATCHES if name == "monitor" else []
        if patches:
            private_elf(ROOT / "originals/dp123/home/neople/monitor/df_monitor_r", elf, patches)
        for path in ([] if optional else (guest / "cfg").glob("*.cfg")):
            text = path.read_text(encoding="utf-8")
            text = re.sub(r"(?<!\d)(?:\d{1,3}\.){3}\d{1,3}(?!\d)", "127.0.0.1", text)
            text = text.replace("Public IP", "127.0.0.1").replace("UDP IP", "127.0.0.1")
            if name == "game":
                # Original game drops monitoring peers before client auth.
                # A local client must not match its default 127.0.0.1 peer.
                text = re.sub(r"(?m)^system_monitoring_ip\s*=.*$", "system_monitoring_ip = 0.0.0.0", text)
                if not re.search(r"(?m)^system_monitoring_ip\s*=", text):
                    text += "\nsystem_monitoring_ip = 0.0.0.0\n"
            if name == "channel":
                text = re.sub(r"(?m)^this_ip\s*=.*$", "this_ip = 127.0.0.1", text)
            if name in ["channel", "relay"]:
                text = re.sub(r"(?m)^max_client\s*=.*$", "max_client = 16", text)
            fields = []
            if (name.startswith("dbmw") or name == "game") and path.stem == config_name:
                fields = [("master_db_" + key, value) for key, value in
                          [("ip", "host"), ("port", "port"), ("acc", "user"), ("pwd", "password"), ("name", "database")]]
            elif name in ["auction", "point"] and path.stem == config_name:
                fields = [(prefix + "_" + key, value) for prefix in ["game_db", "auction_db"]
                          for key, value in [("ip", "host"), ("port", "port"), ("acc", "user"), ("pwd", "password")]]
            for field, key in fields:
                text, count = re.subn(r"(?m)^" + field + r"[^\S\r\n]*=.*$", lambda match: field + " = " + str(db[key]), text)
                if count != 1:
                    raise RuntimeError("Original service configuration field is missing: " + field)
            path.write_text(text, encoding="utf-8")
        if name == "bridge":
            (guest / "cfg/bridge.cfg").write_text(
                "[server]\nmax_client = 6500\nthis_ip = 127.0.0.1\nthis_udp_port = 7000\nthis_tcp_port = 7000\n"
                "[DB]\ndb_ip = 127.0.0.1\ndb_name = d_channel\ndb_id = dnf_bridge\ndb_pwd = " + bridge_password + "\n", encoding="ascii")
        arguments = [guest_dir + "/" + elf.name, config_name] + ([action] if action else [])
        if name in ["auction", "point"]:
            arguments = [guest_dir + "/" + elf.name, "./cfg/" + config_name + ".cfg", "run", "./" + elf.name]
        elif optional:
            arguments = [guest_dir + "/" + elf.name] + ({"gunnersvr": ["-t7", "-i570016"], "zergsvr": ["-t30", "-i570011"]}.get(name, []))
        loader = service_root / ("lib64" if arch == "x86_64" else "lib") / "ld-2.17.so"
        ini["services"]["service" + str(index)] = name
        section = {"host": "hosts/" + arch + "/dnf-host.exe", "root": arch, "cwd": guest_dir,
                   "elf": guest_dir + "/" + elf.name, "elf_sha256": sha256(elf), "loader_sha256": sha256(loader),
                   "stop_signal": str(signal), "automatic": str(int(not optional and not (peers_only and name == "game"))),
                   "env0": "HOME=/home/neople"}
        if not optional and name != "stun":
            section["pid_file"] = guest_dir + "/pid/" + config_name + ".pid"
        if name == "game":
            section.update(env1="LD_PRELOAD=/dp2/libdp2pre.so",
                           env2="LD_LIBRARY_PATH=/lib/i686/nosegneg:/dp2/lib:/home/neople/game")
        section.update({"arg" + str(i): value for i, value in enumerate(arguments)})
        ini[name] = section
        records.append({"name": name, "original_elf_sha256": original_sha, "executed_elf_sha256": sha256(elf),
                                   "host_sha256": sha256(output / section["host"]), "loader_sha256": sha256(loader), "patches": patches})
        if name == "game":
            records[-1]["frida_item_reset"] = prepare_item_reset(root / "dp2/df_game_r.js", elf)
    with (output / "services.ini").open("w", encoding="utf-16") as file:
        ini.write(file)
    return records


def prepare(output, database_source, mysql, peers_only=False):
    output, source, mysql = output.resolve(), database_source.resolve(strict=True), mysql.resolve(strict=True)
    # Bridge's original DB connection uses port 3306. Do not redirect another instance.
    with socket.socket() as available:
        available.bind(("127.0.0.1", 3306))
    before = tree_identity(source / "data")
    output.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ)
    env["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32")
    for name in ["MYSQL_HOME", "MYSQL_PWD", "MYSQL_TCP_PORT"]:
        env.pop(name, None)
    account = subprocess.check_output(["whoami.exe"], env=env, text=True).strip()
    subprocess.run(["icacls.exe", str(output), "/inheritance:r", "/grant:r", account + ":(OI)(CI)F"],
                   env=env, capture_output=True, check=True)
    shutil.copytree(source / "data", output / "data")
    if before != tree_identity(output / "data"):
        raise RuntimeError("Private database copy differs from source")
    for name in ["server.cnf", "client.private.cnf"]:
        text = (source / name).read_text(encoding="utf-8")
        text, count = re.subn(r"(?m)^port=\d+", "port=3306", text)
        if count != 1:
            raise RuntimeError("Source database port setting is missing")
        if name == "server.cnf":
            text = re.sub(r"(?m)^sql_mode\s*=.*$", "sql_mode=NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION", text)
            if not re.search(r"(?m)^sql_mode\s*=", text):
                text += "\nsql_mode=NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION\n"
        (output / name).write_text(text, encoding="utf-8")
    db = json.loads((source / "game-db.json").read_text(encoding="utf-8"))
    db["port"] = 3306
    record = {"recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "scope": "Private original service instance; not minimized packaging, security topology or gameplay acceptance.",
              "source_database": before, "peers_only": peers_only, "services": [], "forced_cleanup": False}
    bridge_password = secrets.token_hex(16)
    record["services"] = copy_service_resources(output, db, bridge_password, peers_only)
    # Adapt the copied application's connection table and bridge account. Source stays cold.
    server = None
    def sql(query):
        result = subprocess.run([str(mysql), "mysql", str(output), "--defaults-file=client.private.cnf", "--batch", "--skip-column-names"],
                                input=query.encode("ascii"), capture_output=True, env=env, timeout=30)
        if result.returncode:
            (output / "prepare-query.private.log").write_bytes(result.stdout + result.stderr)
            raise RuntimeError("Private instance SQL failed; inspect prepare-query.private.log")
        return result.stdout
    try:
        with (output / "prepare-server.private.log").open("wb") as log:
            server = subprocess.Popen([str(mysql), "mysqld", str(output), "--defaults-file=server.cnf", "--console"], stdout=log, stderr=log, env=env)
        deadline = time.monotonic() + 45
        while True:
            try:
                sql("SELECT 1;"); break
            except RuntimeError:
                if server.poll() is not None or time.monotonic() >= deadline:
                    raise
                time.sleep(0.1)
        sql("CREATE USER 'dnf_bridge'@'127.0.0.1' IDENTIFIED BY '" + bridge_password +
            "'; GRANT SELECT ON d_channel.* TO 'dnf_bridge'@'127.0.0.1'; UPDATE d_taiwan.db_connect SET db_ip='127.0.0.1',db_port=3306;")
        sql("CREATE USER 'game'@'%' IDENTIFIED BY '" + secrets.token_hex(24) + "' ACCOUNT LOCK;"
            "GRANT ALL ON taiwan_cain.* TO 'game'@'%';GRANT ALL ON taiwan_billing.* TO 'game'@'%';")
        script = (ROOT / "originals/dp123/dp2/df_game_r.js").read_text(encoding="utf-8")
        quote = lambda value: "'" + value.replace("\\", "\\\\").replace("'", "''") + "'"
        user = re.search(r"var db_account_ptr = Memory.allocUtf8String\('([^']*)'\)", script)[1]
        password = re.search(r"var db_password_ptr = Memory.allocUtf8String\('([^']*)'\)", script)[1]
        account_sql = quote(user) + "@'127.0.0.1'"
        sql("CREATE USER " + account_sql + " IDENTIFIED BY " + quote(password) +
            "; GRANT ALL ON d_taiwan.* TO " + account_sql + "; CREATE DATABASE IF NOT EXISTS frida; GRANT ALL ON frida.* TO " + account_sql + ";")
        month = sql("SELECT DATE_FORMAT(NOW(),'%Y%m');").decode("ascii").strip()
        for schema in ["taiwan_cain_auction_gold", "taiwan_cain_auction_cera"]:
            for table in ["auction_history", "auction_history_buyer"]:
                sql("CREATE TABLE IF NOT EXISTS " + schema + "." + table + "_" + month + " LIKE " + schema + "." + table + ";")
        sql("CREATE TABLE IF NOT EXISTS taiwan_cain_log.log_response_time_" + month +
            " LIKE taiwan_cain_log.log_response_time_202004;")
    finally:
        if server and server.poll() is None:
            try:
                sql("SHUTDOWN;")
            except RuntimeError:
                pass  # Evaluate the actual server's normal shutdown independently.
            try:
                server.wait(timeout=60)
            except subprocess.TimeoutExpired:
                record["forced_cleanup"] = True
                server.terminate(); server.wait(timeout=10)
        record["source_database_unchanged"] = before == tree_identity(source / "data")
        record["database_shutdown_complete"] = server is not None and server.returncode == 0 and b"Shutdown complete" in (output / "prepare-server.private.log").read_bytes()
        record["sources"] = {name: sha256(ROOT / name) for name in ["tools/prepare_service_instance.py", "runtime/dp2_cleanup.lua", "runtime/frida_item_reset.js"]}
        (output / "prepare-receipt.json").write_text(json.dumps(record, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
    if record["forced_cleanup"] or not record["source_database_unchanged"] or not record["database_shutdown_complete"]:
        raise RuntimeError("Private instance preparation did not complete cleanly")
    print("Prepared private service instance:", json.dumps(str(output), ensure_ascii=True), flush=True)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="A new directory; existing directories are refused")
    parser.add_argument("--database-source", type=Path, required=True, help="Stopped secure private seed with game-db.json")
    parser.add_argument("--mysql-exe", type=Path, default=ROOT / "build/gui-services/mysql-standalone.exe")
    parser.add_argument("--peers-only", action="store_true", help="Diagnosis: omit game from whole-start; all 19 entries remain selectable")
    args = parser.parse_args()
    prepare(args.output, args.database_source, args.mysql_exe, args.peers_only)


if __name__ == "__main__":
    main()
