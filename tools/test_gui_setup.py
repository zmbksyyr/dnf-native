"""Create native game instances using the delivered official tools, compatibility hosts and native cabinet."""

import argparse
import configparser
import ctypes as c
import datetime
import json
import os
from pathlib import Path
import platform
import re
import shutil
import socket
import subprocess
import time

from bootstrap import sha256
from probe_mysql import tree_identity
from probe_core_protocols import channel, relay, stun
from test_gui import user, windows, wait_for, click, ready, status, responsive, screenshot, hidden_startup
from test_gui_services import rows, select

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gui", type=Path, default=ROOT / "build/gui-setup/DNF-Native.exe")
    args = parser.parse_args()
    source = args.gui.resolve(strict=True).parent
    with socket.socket() as available:
        available.bind(("127.0.0.1", 3306))
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    probe = ROOT / "build/probes" / stamp / "gui-setup"
    delivery = probe / "本地 程序(1)"
    delivery.mkdir(parents=True)
    shutil.copy2(source / "DNF-Native.exe", delivery / "DNF-Native.exe")
    for name in ["mysql", "runtime", "native"]:
        shutil.copytree(source / name, delivery / name)
    source_before = tree_identity(source / "native")
    copied_before = tree_identity(delivery / "native")
    assert source_before == copied_before
    env = dict(os.environ)
    env["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32")
    for key in ["MYSQL_HOME", "MYSQL_PWD", "MYSQL_TCP_PORT"]:
        env.pop(key, None)
    record = {"recorded_utc": stamp, "os": platform.platform(),
              "scope": "Native GUI local-resource copy, secure application seed initialization, per-instance accounts, 15 original peers and Frida DB interface. No clean multi-OS or client gameplay acceptance.",
              "gui_sha256": sha256(delivery / "DNF-Native.exe"), "mysql_sha256": sha256(delivery / "runtime/mysql-control.exe"),
              "script_sha256": sha256(Path(__file__)), "checks": {}, "forced_cleanup": False}
    checks = record["checks"]
    instance = probe / "游戏 实例(1)"
    process = subprocess.Popen([str(delivery / "DNF-Native.exe"), "--instance", str(instance)], env=env, startupinfo=hidden_startup())
    window = None
    saved_elf = None
    manifest = configparser.ConfigParser(interpolation=None)
    manifest.read(delivery / "native/resources.ini", encoding="utf-16")
    cabinet = manifest["resources"]["version"] == "2"
    modified = delivery / ("native/resources.cab" if cabinet else "runtime/hosts/i686/dnf-host.exe")
    lua = js = None
    lua_before = js_before = None

    def target(path):
        text = c.create_unicode_buffer(str(path))
        user.SendMessageW(user.GetDlgItem(window, 101), 0x000C, 0, c.addressof(text))

    def query(text, root=instance):
        result = subprocess.run([str(delivery / "runtime/mysql-control.exe"), "mysql", str(root), "--defaults-file=client.private.cnf", "--batch", "--skip-column-names"],
                                input=text.encode("ascii"), capture_output=True, env=env, timeout=30)
        if result.returncode:
            (probe / "query-error.private.log").write_bytes(result.stdout + result.stderr)
            raise RuntimeError("Native instance SQL failed; private query log retained")
        return result.stdout

    try:
        window = wait_for(lambda: next(iter(windows(process.pid, "DNFNativeController")), None), "main window")
        assert user.IsWindowEnabled(user.GetDlgItem(window,110)) and not instance.exists()
        checks["opening_gui_does_not_start_work_or_disable_initialize"] = True
        saved_elf = modified.read_bytes()
        broken = bytearray(saved_elf); broken[-1] ^= 1; modified.write_bytes(broken)
        click(window, 110)
        wait_for(lambda: status(window).startswith("操作失败"), "changed local resource rejection")
        assert not instance.exists()
        checks["changed_resource_rejected_before_instance_writes"] = True
        modified.write_bytes(saved_elf); saved_elf = None
        occupied = probe / "已有 文件(keep)"
        occupied.mkdir(); sentinel = occupied / "keep.txt"; sentinel.write_bytes(b"existing user content\x00\xff")
        before = tree_identity(occupied)
        target(occupied); click(window, 110)
        wait_for(lambda: status(window).startswith("操作失败"), "nonempty directory rejection")
        assert tree_identity(occupied) == before
        checks["existing_nonempty_directory_unchanged"] = True
        target(instance); click(window, 110)
        wait_for(lambda: (instance / "instance.pending").exists(), "native copy starts")
        checks["copy_and_initialize_window_response_ms"] = responsive(window)
        assert all(user.IsWindowEnabled(user.GetDlgItem(window,identifier)) for identifier in [220,240,241,243,244,116])
        checks["busy_keeps_service_selection_and_log_controls_available"] = True
        ready(window, "游戏实例创建完成", 240)
        wait_for(lambda: user.IsWindowEnabled(user.GetDlgItem(window, 111)), "created instance controls")
        assert len(rows(window)) == 19 and not (instance / "instance.pending").exists()
        assert not (instance / "seed.sql").exists()
        checks["native_first_run_creates_nineteen_service_instance"] = True
        seed_sha = manifest["resources"]["seed_sha256"]
        assert any(sha256(path) == seed_sha for path in (instance / ".mysql").glob("gui-*.private.log"))
        checks["import_reexport_matches_complete_original_seed_bytes"] = True
        checks["resource_format"] = "cabinet" if cabinet else "directory"
        assert not (instance / ".mysql/setup-seed.sql").exists()
        assert not (instance / ".mysql/setup-services.ini").exists()
        for name in ["game/cfg/siroco11.cfg", "game/cfg/siroco56.cfg", "auction/cfg/bridge.cfg"]:
            config = (instance / "i686/home/neople" / name).read_text(encoding="utf-8")
            assert "Public IP" not in config and "UDP IP" not in config, name
            assert "127.0.0.1" in config, name
            if name.startswith("game/"):
                assert re.search(r"(?m)^system_monitoring_ip\s*=\s*0\.0\.0\.0\s*$", config), name
        checks["original_advertised_address_placeholders_are_local"] = True
        assert not (instance / "hosts").exists(), "Hosts belong to the delivered runtime directory"
        assert "password=uu5!^%jg\n" in (instance / "client.private.cnf").read_text(encoding="utf-8")
        checks["fixed_default_root_password_and_external_hosts"] = True
        before_cfg = {path: path.read_bytes() for path in instance.rglob("*.cfg")}
        ip_text = c.create_unicode_buffer("127.0.0.2")
        user.SendMessageW(user.GetDlgItem(window,250),0x000C,0,c.addressof(ip_text))
        user.SendMessageW(window,0x111,251,user.GetDlgItem(window,251))
        ready(window,"服务 IP 已更新")
        wait_for(lambda: user.IsWindowEnabled(user.GetDlgItem(window,251)),"IP apply finished")
        changed_cfg = {path: path.read_bytes() for path in before_cfg}
        assert any(before_cfg[path] != changed_cfg[path] for path in before_cfg)
        for path, old in before_cfg.items():
            current = path.read_bytes()
            for field in [b"master_db_ip",b"db_ip",b"game_db_ip",b"auction_db_ip",b"system_monitoring_ip"]:
                assert [line for line in old.splitlines() if line.strip().startswith(field+b" ") or line.strip().startswith(field+b"=")] == [line for line in current.splitlines() if line.strip().startswith(field+b" ") or line.strip().startswith(field+b"=")], (path,field)
        assert b"127.0.0.2" in (instance / "i686/home/neople/monitor/cfg/mnt_siroco.cfg").read_bytes()
        backups = list((instance / ".settings").iterdir())
        assert len(backups)==1
        user.SendMessageW(window,0x111,251,user.GetDlgItem(window,251))
        ready(window,"服务 IP 已更新")
        wait_for(lambda: user.IsWindowEnabled(user.GetDlgItem(window,251)),"same IP finished")
        assert list((instance / ".settings").iterdir())==backups
        assert all(path.read_bytes()==text for path,text in changed_cfg.items())
        checks["service_ip_updates_ini_and_positional_records_with_backups"] = True
        checks["service_ip_keeps_database_and_monitor_bypass_and_is_idempotent"] = True
        screenshot(window, probe / "created.png")
        click(window, 111); ready(window, "数据库 SQL 检查通过")
        schemas = query("SELECT TABLE_SCHEMA,COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA NOT IN ('mysql','information_schema','performance_schema','sys') GROUP BY TABLE_SCHEMA ORDER BY TABLE_SCHEMA;")
        counts = {line.split("\t")[0]: int(line.split("\t")[1]) for line in schemas.decode("ascii").splitlines()}
        record["application_table_counts"] = counts
        assert len(counts) == 18 and sum(counts.values()) == 606
        checks["original_eighteen_schemas_and_current_month_tables"] = True
        assert query("SELECT @@sql_mode;").strip() == b"NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION"
        assert query("SELECT account_locked FROM mysql.user WHERE User='game' AND Host='%';").strip() == b"Y"
        query("CALL taiwan_cain.sp_game_channel();")
        assert not query("SELECT PRIVILEGE_TYPE FROM information_schema.USER_PRIVILEGES WHERE GRANTEE=CONCAT(CHAR(39),'game',CHAR(39),'@',CHAR(39),'%',CHAR(39)) AND PRIVILEGE_TYPE<>'USAGE';").strip()
        checks["original_stored_routine_runs_with_locked_schema_definer"] = True
        game_cfg = (instance / "i686/home/neople/game/cfg/siroco11.cfg").read_text(encoding="utf-8")
        encoded = re.search(r"(?m)^master_db_pwd\s*=\s*([0-9a-f]{48})", game_cfg)[1]
        assert query("SELECT COUNT(*) FROM d_taiwan.db_connect WHERE db_ip<>'127.0.0.1' OR db_port<>3306 OR db_userid<>'dnf_game' OR db_passwd<>'" + encoded + "';").strip() == b"0"
        checks["original_credential_encoding_matches_connection_table"] = True
        checks["game_account_has_no_global_grants"] = not query("SELECT PRIVILEGE_TYPE FROM information_schema.USER_PRIVILEGES WHERE GRANTEE=CONCAT(CHAR(39),'dnf_game',CHAR(39),'@',CHAR(39),'127.0.0.1',CHAR(39)) AND PRIVILEGE_TYPE<>'USAGE';").strip()
        checks["ip_locked_while_database_running"] = not user.IsWindowEnabled(user.GetDlgItem(window,251))
        first_accounts = query("SELECT User,Host,authentication_string FROM mysql.user WHERE User IN ('root','dnf_game','dnf_bridge','dnf_frida') ORDER BY User,Host;")
        for index in range(15):
            select(window, index); click(window, 221)
            ready(window, rows(window)[index].split()[0] + "：进程运行中", 30)
        time.sleep(25)
        assert sum("进程运行中" in row for row in rows(window)) == 15, rows(window)
        checks["native_initialized_fifteen_original_peers"] = True
        checks["channel_protocol"] = channel(7001,"127.0.0.2")
        checks["relay_and_actual_monitor_protocol"] = relay(True,"127.0.0.2")
        checks["stun_protocol"] = stun()
        print("Native GUI first run: original seed, generated accounts and 15-peer protocols passed", flush=True)
        # Exercise the original init_db implementation with the newly generated
        # Frida account. This fixture intentionally exits before natural game load.
        lua, js = instance / "i686/dp2/df_game_r.lua", instance / "i686/dp2/df_game_r.js"
        lua_before, js_before = lua.read_bytes(), js.read_bytes()
        js.write_bytes(js_before + b"\nvar savedInit=rpc.exports.init; rpc.exports.init=function(){ savedInit.apply(this,arguments); init_db(); if(mysql_d_taiwan===null || mysql_frida===null || api_MySQL_exec(mysql_frida,'SELECT 1')!==1) throw new Error('Original script database test failed'); console.log('dnf-native-original-script-db-ok'); };\n")
        lua.write_text("local frida=require('df.frida'); frida.load(); print('dnf-native-original-script-db-returned'); os.exit(42)\n", encoding="ascii")
        select(window, 15); click(window, 221)
        wait_for(lambda: "已退出" in rows(window)[15], "Frida database fixture exit", 60)
        assert "代码 42" in rows(window)[15]
        assert query("SHOW TABLES FROM frida;").decode("ascii").splitlines() == ["data", "dgnfinishtime", "init_sp_tp"]
        checks["original_frida_database_interface_with_generated_account"] = True
        lua.write_bytes(lua_before); js.write_bytes(js_before); lua_before = js_before = None
        click(window, 224); ready(window, "服务进程已退出，数据库已停止", 150)
        assert not list((instance / "i686").rglob("*.pid"))
        checks["all_owned_peers_and_database_stopped_normally"] = True
        second = probe / "游戏 实例(2)"
        target(second); click(window, 110); ready(window, "游戏实例创建完成", 240)
        click(window, 111); ready(window, "数据库 SQL 检查通过")
        second_accounts = query("SELECT User,Host,authentication_string FROM mysql.user WHERE User IN ('root','dnf_game','dnf_bridge','dnf_frida') ORDER BY User,Host;", second)
        first_rows, second_rows = first_accounts.splitlines(), second_accounts.splitlines()
        assert len(first_rows) == len(second_rows) == 4
        for a,b in zip(first_rows,second_rows):
            assert a.split(b"\t")[:2] == b.split(b"\t")[:2]
            assert (a.split(b"\t")[2] == b.split(b"\t")[2]) == a.startswith(b"root\t")
        checks["default_root_matches_and_three_service_secrets_differ"] = True
        user.PostMessageW(window, 0x10, 0, 0); process.wait(timeout=90)
        assert process.returncode == 0
        checks["second_instance_normal_close"] = True
        checks["delivery_resources_unchanged"] = tree_identity(delivery / "native") == copied_before
        checks["source_resources_unchanged"] = tree_identity(source / "native") == source_before
        assert all(value is not False for value in checks.values())
        record["first_instance"] = str(instance.relative_to(ROOT))
        record["second_instance"] = str(second.relative_to(ROOT))
    except Exception as error:
        record["error"] = type(error).__name__ + ": " + str(error)
    finally:
        if saved_elf is not None:
            modified.write_bytes(saved_elf)
        if lua_before is not None:
            lua.write_bytes(lua_before); js.write_bytes(js_before)
        if process.poll() is None:
            if window:
                user.PostMessageW(window, 0x10, 0, 0)
            try:
                process.wait(timeout=240)
            except subprocess.TimeoutExpired:
                record["forced_cleanup"] = True
                subprocess.run(["taskkill.exe", "/PID", str(process.pid), "/T", "/F"], capture_output=True, timeout=15)
                process.wait(timeout=10)
        record["exit_code"] = process.returncode
        (probe / "receipt.json").write_text(json.dumps(record, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")
        print("Native GUI setup receipt:", probe / "receipt.json", flush=True)
    if record.get("error") or record["forced_cleanup"]:
        raise SystemExit(json.dumps(record.get("error", "forced cleanup"), ensure_ascii=True))


if __name__ == "__main__":
    main()
