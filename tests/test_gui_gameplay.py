"""Exercise monthly migration/rollback and real game level restarts in a private copy."""
import argparse
import configparser
import ctypes as c
from ctypes import wintypes as w
import datetime
import json
from pathlib import Path
import platform
import re
import shutil
import socket
import subprocess
import sys
import time
import traceback

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from bootstrap import BUILD, sha256
from runtime_support import tree_identity, runtime_diagnostics
from gui_support import (user, kernel, windows, wait_for, click, ready, title, hidden_startup,
                         select_service, service_row, screenshot, guest_root, open_features, close_features, log_text)

LEVELS = [50,55,60,65,70,75,80,85,86,90,95,99,100]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--instance", type=Path, required=True, help="Stopped fixture, copied before use")
    parser.add_argument("--gui", type=Path, required=True)
    args = parser.parse_args()
    source, gui = args.instance.resolve(strict=True), args.gui.resolve(strict=True)
    with socket.socket() as available:
        available.bind(("127.0.0.1",3306))
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    probe = BUILD / "probes" / stamp / "gui-gameplay"
    instance = probe / "instance"
    source_before = tree_identity(source / "data")
    shutil.copytree(source,instance,ignore=shutil.ignore_patterns(".mysql","service-logs",".instance.lock"))
    record = {"os":platform.platform(),"gui_sha256":sha256(gui),"scope":"Original game initialization/restart and MySQL schema writes", "checks":{},"launches":[]}
    checks = record["checks"]
    tool = gui.parent / "resources/mysql-control.exe"
    manifest = configparser.ConfigParser(interpolation=None)
    manifest.read(instance / "services.ini",encoding="utf-16")
    manifest["game"]["automatic"] = "0"
    manifest["gameplay"] = {"max_level":"70"}
    manifest["plugins"] = {"mode":"frida"}
    with (instance / "services.ini").open("w",encoding="utf-16") as out:
        manifest.write(out)
    game = guest_root(instance) / "home/neople/game/df_game_r"
    original = sha256(game)
    sql_index = 0

    def sql(query=None, script=None):
        nonlocal sql_index
        data = script.read_bytes() if script else query.encode()
        result = subprocess.run([str(tool),"mysql",str(instance),"--defaults-file=client.private.cnf","--batch","--raw","--skip-column-names"],
                                input=data,capture_output=True,cwd=instance,timeout=120)
        sql_index += 1
        (probe / ("sql-"+str(sql_index)+".private.log")).write_bytes(result.stdout+result.stderr)
        assert result.returncode == 0, "SQL failed; private log " + str(sql_index)
        return result.stdout.decode("utf-8","replace").replace("\r","")

    process = server = window = None
    try:
        with (probe / "seed-server.private.log").open("wb") as out:
            server = subprocess.Popen([str(tool),"mysqld",str(instance),"--defaults-file=server.cnf","--console"],stdout=out,stderr=out,cwd=instance)
        def connected():
            result = subprocess.run([str(tool),"mysql",str(instance),"--defaults-file=client.private.cnf","--execute=SELECT 1"],capture_output=True,timeout=10)
            return result.returncode == 0
        wait_for(connected,"private seed database",45)
        months = sql("SELECT DATE_FORMAT(NOW(),'%Y%m'),DATE_FORMAT(DATE_ADD(NOW(),INTERVAL 1 MONTH),'%Y%m');").strip().split("\t")
        month = months[0]
        tables = []
        for db in ("taiwan_cain_auction_gold","taiwan_cain_auction_cera"):
            for family in ("auction_history","auction_history_buyer"):
                name = db+"."+family+"_"+month
                # This private copy may already have current repaired month
                # tables. Recreate the legacy fixture before testing migration.
                sql("DROP TABLE IF EXISTS "+name+"; CREATE TABLE "+name+" LIKE "+db+"."+family+"; INSERT INTO "+name+"(auction_id,occ_time) VALUES(91000000001,NOW());")
                columns = sql("SHOW COLUMNS FROM "+name+";")
                tables.append((name,columns,sql("SELECT * FROM "+name+" WHERE auction_id=91000000001;")))
        log = "taiwan_cain_log.log_response_time_"+month
        sql("DROP TABLE IF EXISTS "+log+"; CREATE TABLE "+log+" LIKE taiwan_cain_log.log_response_time_202004; INSERT INTO "+log+"(level,occ_date) VALUES(123,NOW());")
        log_row = sql("SELECT * FROM "+log+" WHERE level=123;")
        assert "item_guid" not in tables[0][1] and "commission" not in tables[1][1]
        sql("SHUTDOWN;"); server.wait(timeout=60); server = None
        process = subprocess.Popen([str(gui),"--instance",str(instance)],startupinfo=hidden_startup())
        window = wait_for(lambda: next(iter(windows(process.pid,"DNFNativeController")),None),"controller")
        feature=open_features(window)
        assert user.SendMessageW(user.GetDlgItem(feature,233),0x146,0,0) == len(LEVELS)
        close_features(feature)
        click(window,111); ready(window,"数据库 SQL 检查通过",60)
        for db in ("taiwan_cain_auction_gold","taiwan_cain_auction_cera"):
            for m in months:
                history = db+".auction_history_"+m
                buyer = db+".auction_history_buyer_"+m
                assert len(sql("SHOW COLUMNS FROM "+history+";").splitlines()) == 26
                assert len(sql("SHOW COLUMNS FROM "+buyer+";").splitlines()) == 8
                sql("INSERT INTO "+history+"(auction_id,start_time,occ_time,event_type,owner_id,buyer_id,price,seal_flag,item_id,add_info,upgrade,seal_cnt,endurance,extend_info,owner_postal_id,buyer_postal_id,amplify_option,amplify_value,item_guid,unit_price,roi_high_key,roi_low_key,commission,owner_type,random_option,seperate_upgrade) VALUES(91000000002,NOW(),NOW(),1,1,2,10,0,16400,0,0,0,1,0,0,0,0,0,X'01',10,0,0,1,0,X'00',0);")
                sql("INSERT INTO "+buyer+"(auction_id,occ_time,pre_buyer_id,buyer_id,pre_price,price,pre_buyer_postal_id,commission) VALUES(91000000002,NOW(),1,2,1,10,0,1);")
        for m in months:
            sql("INSERT INTO taiwan_cain_log.log_response_time_"+m+"(channel_no,occ_time,packet_id,packet_count,total_response_time,avg_response_time) VALUES(11,NOW(),1,2,4,2);")
        checks["both_months_and_both_auction_databases_accept_original_column_lists"] = True
        backups = list((instance / ".mysql").glob("monthly-*.private.sql"))
        assert len(backups) == 4, len(backups)  # Two history, gold buyer, mislabeled response log.
        for backup in backups:
            assert sha256(backup) == backup.with_suffix(backup.suffix+".sha256").read_text().strip()
            assert backup.with_suffix(backup.suffix+".rollback.sql").is_file()
        for name,columns,row in tables:
            names = ["`"+line.split("\t")[0]+"`" for line in columns.splitlines()]
            assert sql("SELECT "+",".join(names)+" FROM "+name+" WHERE auction_id=91000000001;") == row
        old_log = sql("SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA='taiwan_cain_log' AND TABLE_NAME LIKE 'log_response_time_"+month+"_native_%_old';").strip()
        assert old_log and sql("SELECT * FROM taiwan_cain_log."+old_log+" WHERE level=123;") == log_row
        checks["legacy_rows_and_verified_official_backups_retained"] = True
        for backup in backups:
            sql(script=backup.with_suffix(backup.suffix+".rollback.sql"))
        for name,columns,row in tables:
            assert sql("SHOW COLUMNS FROM "+name+";") == columns
            assert sql("SELECT * FROM "+name+" WHERE auction_id=91000000001;") == row
        assert sql("SELECT * FROM "+log+" WHERE level=123;") == log_row
        checks["generated_rollback_restores_legacy_schema_and_rows"] = True
        click(window,112); wait_for(lambda: user.IsWindowEnabled(user.GetDlgItem(window,111)),"database stopped",60)
        click(window,111); ready(window,"数据库 SQL 检查通过",60)
        backups_after_repair = len(list((instance / ".mysql").glob("monthly-*.private.sql")))
        click(window,112); wait_for(lambda: user.IsWindowEnabled(user.GetDlgItem(window,111)),"database stopped",60)
        click(window,111); ready(window,"数据库 SQL 检查通过",60)
        assert len(list((instance / ".mysql").glob("monthly-*.private.sql"))) == backups_after_repair
        checks["maintenance_is_idempotent"] = True
        click(window,223); ready(window,"已启动默认服务进程",120)
        select_service(window,"game"); click(window,221); ready(window,"game：进程运行中",30)
        kernel.OpenProcess.argtypes, kernel.OpenProcess.restype = [w.DWORD,w.BOOL,w.DWORD],w.HANDLE
        kernel.ReadProcessMemory.argtypes = [w.HANDLE,c.c_void_p,c.c_void_p,c.c_size_t,c.POINTER(c.c_size_t)]
        kernel.CloseHandle.argtypes = [w.HANDLE]

        def pids():
            result = {}
            for name in ("game","monitor","guild","auction","point"):
                select_service(window,name)
                text = service_row(window,name)
                match = re.search(r"PID\s+(\d+)",text)
                assert match, text
                result[name] = int(match[1])
            select_service(window,"game")
            return result

        prior = None
        for level in (70,85,70):
            if prior:
                feature=open_features(window)
                combo=user.GetDlgItem(feature,233)
                user.SendMessageW(combo,0x14E,LEVELS.index(level),0)
                user.SendMessageW(feature,0x111,233 | (1<<16),combo)
                click(feature,231)  # 85: Lua only; 70: restore Frida in the same apply.
                if level == 85:
                    # Reject an unknown module before disrupting the running game.
                    module = guest_root(instance) / "dp2/lua/df/frida.lua"
                    module_bytes = module.read_bytes()
                    config_bytes = (instance / "services.ini").read_bytes()
                    module.write_bytes(b"-- unsupported private test module\n")
                    base = len(log_text(window))
                    try:
                        click(feature,234)
                        wait_for(lambda: "Frida module entry differs" in log_text(window)[base:], "module preflight rejection", 10)
                        wait_for(lambda: user.IsWindowEnabled(user.GetDlgItem(feature,233)), "operation completed")
                        assert (instance / "services.ini").read_bytes() == config_bytes
                        assert pids() == prior
                        assert len(list((instance / "service-logs").glob("game-*.stdout.private.log"))) == 1
                    finally:
                        module.write_bytes(module_bytes)
                    # Failed operations reset controls to the saved values.
                    user.SendMessageW(combo,0x14E,LEVELS.index(level),0)
                    user.SendMessageW(feature,0x111,233 | (1<<16),combo)
                    if user.SendMessageW(user.GetDlgItem(feature,231),0xF0,0,0): click(feature,231)
                    checks["unknown_lua_module_rejected_before_game_stop_or_config_write"] = True
                base = len(log_text(window))
                click(feature,234); ready(window,"运行设置已应用：等级上限 "+str(level),210,since=base)
                assert not user.IsWindowEnabled(user.GetDlgItem(feature,234))
                close_features(feature)
            started = time.monotonic()
            wait_for(lambda: "应用已初始化" in service_row(window,"game"),"original game initialized at "+str(level),180)
            current = pids()
            if prior:
                assert current["game"] != prior["game"]
                assert all(current[n] == prior[n] for n in ("monitor","guild","auction","point"))
            process_handle = kernel.OpenProcess(0x410,False,current["game"])
            assert process_handle
            try:
                for address,value in ((0x08360c3b,level),(0x08666e9c,level-1),(0x081db4eb,0x26)):
                    byte = c.c_ubyte(); count = c.c_size_t()
                    assert kernel.ReadProcessMemory(process_handle,address,c.byref(byte),1,c.byref(count)) and count.value==1 and byte.value==value
            finally:
                kernel.CloseHandle(process_handle)
            manifest.read(instance / "services.ini",encoding="utf-16")
            assert manifest.getint("gameplay","max_level")==level and sha256(game)==original
            expected_mode = "lua" if level == 85 else "frida"
            assert manifest["plugins"]["mode"] == expected_mode
            from test_gui_plugins import loaded_plugins
            assert loaded_plugins(current["game"],instance) == {"dp2":True,"frida":expected_mode=="frida"}
            record["launches"].append({"level":level,"pids":current,"initialized_wait_seconds":round(time.monotonic()-started,3)})
            screenshot(window,probe / ("level-"+str(len(record["launches"]))+".png"))
            print("GUI game maximum",level,"PASS",flush=True)
            prior=current
        checks["combined_level_and_plugin_apply_restarts_game_once_and_preserves_peer_pids"] = True
        backups = list((instance / ".mysql").glob("runtime-settings-*"))
        assert len(backups) == 2
        for backup,old_level,old_mode in zip(sorted(backups),[70,85],["frida","lua"]):
            prior_config = configparser.ConfigParser(interpolation=None)
            prior_config.read(backup / "services.ini",encoding="utf-16")
            assert prior_config.getint("gameplay","max_level") == old_level
            assert prior_config["plugins"]["mode"] == old_mode
        checks["unified_settings_backups_preserve_previous_levels_and_plugin_modes"] = True
        checks["live_bytes_and_saved_level_match_and_disk_elf_unchanged"] = True
        click(window,224); ready(window,"服务进程已退出，数据库已停止",210)
        outputs = list((instance / "service-logs").glob("game-*.stdout.private.log"))
        assert len(outputs)==3 and all(b"main() exited OK" in p.read_bytes() for p in outputs)
        checks["three_games_exit_normally"] = True
        user.PostMessageW(window,0x10,0,0); process.wait(timeout=30)
    except Exception as error:
        record["error"] = type(error).__name__+": "+str(error)
        record["traceback"] = traceback.format_exc()
        if window: screenshot(window,probe / "failure.private.png")
    finally:
        if server and server.poll() is None:
            sql("SHUTDOWN;"); server.wait(timeout=60)
        if process and process.poll() is None:
            if window: user.PostMessageW(window,0x10,0,0)
            process.wait(timeout=240)
        checks["source_data_unchanged"] = tree_identity(source / "data")==source_before
        record["diagnostics"] = runtime_diagnostics(instance)
        record["passed"] = not record.get("error") and all(checks.values()) and not record["diagnostics"]["faults"]
        (probe / "receipt.json").write_text(json.dumps(record,indent=2)+"\n")
        print("GUI gameplay rules:","PASS" if record["passed"] else "FAIL",probe / "receipt.json",flush=True)
    if not record["passed"]: raise SystemExit(record.get("error","Gameplay rule checks failed"))


if __name__ == "__main__":
    main()
