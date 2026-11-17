"""Exercise real market maintenance, schema keys and rollback in a private copy."""
import argparse
import configparser
import ctypes as c
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

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from bootstrap import BUILD, sha256
from runtime_support import tree_identity
from gui_support import user, windows, wait_for, click, ready, ready_stopped, title, log_text, hidden_startup, screenshot, select_service

GOLD="taiwan_cain_auction_gold"
CERA="taiwan_cain_auction_cera"

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--instance",type=Path,required=True)
    parser.add_argument("--gui",type=Path,required=True)
    args=parser.parse_args()
    source=args.instance.resolve(strict=True); gui=args.gui.resolve(strict=True)
    with socket.socket() as s: s.bind(("127.0.0.1",3306))
    work=BUILD/"probes"/datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")/"gui-auction"
    instance=work/"instance"; before=tree_identity(source/"data")
    shutil.copytree(source,instance,ignore=shutil.ignore_patterns(".mysql","service-logs",".instance.lock"))
    config=configparser.ConfigParser(interpolation=None); config.read(instance/"services.ini",encoding="utf-16")
    config["auction"].update({"extend_levels":"1","extend_refine":"1","dp2_guard":"0","auto_check":"1"})
    config["plugins"]={"mode":"frida"}
    for name in config["services"]:
        if name.startswith("service") and config["services"][name] in config:
            config[config["services"][name]]["automatic"]="0"
    with (instance/"services.ini").open("w",encoding="utf-16") as f: config.write(f)
    tool=gui.parent/"resources/mysql-control.exe"
    record={"os":platform.platform(),"gui_sha256":sha256(gui),"scope":"Real Win32 actions and original MySQL5.7 on private copy; original ELF function tests are separate","checks":{}}
    checks=record["checks"]; sql_count=0; process=server=window=market=None
    def sql(text,fail=False):
        nonlocal sql_count
        r=subprocess.run([str(tool),"mysql",str(instance),"--defaults-file=client.private.cnf","--batch","--raw","--skip-column-names"],input=text.encode(),capture_output=True,cwd=instance,timeout=120)
        sql_count+=1; (work/f"sql-{sql_count}.private.log").write_bytes(r.stdout+r.stderr)
        if not fail: assert r.returncode==0,"Private SQL failed: "+str(sql_count)
        return r.stdout.decode("utf-8","replace").replace("\r","")
    def count(db): return int(sql(f"SELECT COUNT(*) FROM {db}.auction_main;"))
    def insert(db,order,item=16400,refine=8):
        sql(f"INSERT INTO {db}.auction_main(auction_id,occ_time,expire_time,owner_id,owner_name,owner_type,owner_nexon_id,buyer_id,buyer_name,price,instant_price,seal_flag,item_id,add_info,upgrade,amplify_option,amplify_value,seal_cnt,endurance,extend_info,black_point,unit_price,random_option,roi_high_key,roi_low_key,seperate_upgrade,item_guid) VALUES({order},NOW(),2147483647,100,'fixture',0,'fixture',0,'',100,200,0,{item},1,0,0,0,0,100,0,0,200,'',0,0,{refine},'');")
    def operation(identifier,fail=False):
        base=len(log_text(window))
        # Completion is the actual worker enabling its owned maintenance window.
        click(market,identifier)
        wait_for(lambda:not user.IsWindowEnabled(market),"maintenance begins",10)
        wait_for(lambda:user.IsWindowEnabled(market),"maintenance ends",180)
        report=log_text(window)[base:]
        if fail: assert "操作失败" in report,report
        else: assert "双拍卖" in report and "操作失败" not in report,report
        return report
    try:
        with (work/"seed-server.private.log").open("wb") as log:
            server=subprocess.Popen([str(tool),"mysqld",str(instance),"--defaults-file=server.cnf","--console"],stdout=log,stderr=log,cwd=instance)
        def connected():
            return subprocess.run([str(tool),"mysql",str(instance),"--defaults-file=client.private.cnf","--execute=SELECT 1"],capture_output=True,timeout=10).returncode==0
        wait_for(connected,"private database",45)
        for db in [GOLD,CERA]: sql(f"DELETE FROM {db}.auction_main; DELETE FROM {db}.auction_system_main; DELETE FROM {db}.auction_average_price; DELETE FROM {db}.auction_roi_average_price;")
        insert(GOLD,91000000001,2147483000,0)
        sql("SHUTDOWN;");server.wait(timeout=60);server=None
        process=subprocess.Popen([str(gui),"--instance",str(instance)],startupinfo=hidden_startup())
        window=wait_for(lambda:next(iter(windows(process.pid,"DNFNativeController")),None),"controller")
        click(window,111);ready(window,"数据库 SQL 检查通过",60)
        click(window,300)
        market=wait_for(lambda:next(iter(windows(process.pid,"DnfNativeMarketMaintenance")),None),"market window")
        # Keep the test hidden except for explicit window-only captures.
        user.ShowWindow(market,0)
        report=operation(310);assert "2147483000" in report and "锻造上限 10" in report
        checks["pvf_and_stored_order_check"] = True
        paths=[instance/f"home/neople/{name}/iteminfo.dat" for name in ["auction","point"]]
        original=[sha256(p) for p in paths]
        operation(311,fail=True)
        assert [sha256(p) for p in paths]==original and count(GOLD)==1
        checks["conflicting_index_sync_preserves_orders_and_files"]=True
        operation(314)
        assert count(GOLD)==0
        operation(311)
        assert sha256(paths[0])==sha256(paths[1]) and sha256(paths[0])!=original[0]
        assert "2675336 " in paths[0].read_text(encoding="utf-8")
        checks["sync_both_indexes_and_currency_definitions"]=True
        # An untracked old dat cannot be relabelled as the newly selected PVF.
        synced=[sha256(p) for p in paths]
        operation(312,fail=True)
        assert [sha256(p) for p in paths]==synced
        checks["restore_rejects_untracked_legacy_index"]=True
        marker=instance/".mysql/market-index.ini"
        previous_hash="f"*64
        marker.write_text(marker.read_text().replace("pvf="+record.get("pvf_sha256",sha256(instance/"home/neople/game/Script.pvf")),"pvf="+previous_hash),encoding="utf-8")
        operation(311)
        saved=configparser.ConfigParser(interpolation=None)
        saved.read(instance/"services.ini",encoding="utf-16")
        old_index=Path(saved["auction"]["previous_index"])
        assert (old_index/"pvf.sha256").read_text()==previous_hash
        operation(312,fail=True)
        assert [sha256(p) for p in paths]==synced
        checks["restore_rejects_other_pvf_and_preserves_old_binding"]=True
        sql(f"INSERT INTO {GOLD}.auction_average_price(item_id,upgrade,seperate_upgrade,average_price) VALUES(2147483000,0,0,99);")
        report=operation(310)
        assert "auction_average_price / item 2147483000" in report
        operation(311,fail=True)
        assert [sha256(p) for p in paths]==synced
        assert int(sql(f"SELECT COUNT(*) FROM {GOLD}.auction_average_price;"))==1
        operation(316)
        checks["orphan_price_blocks_sync_without_implicit_deletion"]=True
        for db in [GOLD,CERA]:
            sql(f"INSERT INTO {db}.auction_average_price(item_id,upgrade,seperate_upgrade,average_price) VALUES(16400,0,7,107),(16400,0,8,108),(16400,0,10,110),(16400,1,0,200);")
            assert sql(f"SELECT GROUP_CONCAT(average_price ORDER BY upgrade,seperate_upgrade) FROM {db}.auction_average_price WHERE item_id=16400;")=="107,108,110,200\n"
            sql(f"INSERT INTO {db}.auction_roi_average_price(item_id,upgrade,seperate_upgrade,roi_high_key,roi_low_key,average_price) VALUES(16400,0,8,1,1,108),(16400,0,10,1,1,110);")
            sql(f"INSERT INTO {db}.auction_manual_average_price(item_id,upgrade,seperate_upgrade,average_price,is_apply) VALUES(16400,0,8,888,0);")
            sql(f"UPDATE {db}.auction_average_price AS A JOIN {db}.auction_manual_average_price AS B ON A.item_id=B.item_id AND A.upgrade=B.upgrade AND A.seperate_upgrade=B.seperate_upgrade SET A.average_price=B.average_price WHERE B.is_apply=0;")
            assert sql(f"SELECT GROUP_CONCAT(average_price ORDER BY upgrade,seperate_upgrade) FROM {db}.auction_average_price WHERE item_id=16400;")=="107,888,110,200\n"
        checks["normal_roi_and_manual_price_keys_keep_refinements_distinct"]=True
        for db in [GOLD,CERA]:
            insert(db,91000000011)
            insert(db,91000000012,2147483000,0)
            insert(db,91000000013,16400,11)
        operation(314)
        assert count(GOLD)==1 and count(CERA)==1
        checks["cleanup_incompatible_orders_in_both_markets"]=True
        operation(315);assert count(GOLD)==0 and count(CERA)==0
        checks["clear_orders_covers_both_markets"]=True
        operation(316)
        assert int(sql(f"SELECT COUNT(*) FROM {GOLD}.auction_average_price;"))==0
        assert int(sql(f"SELECT COUNT(*) FROM {CERA}.auction_average_price;"))==0
        checks["clear_prices_covers_both_markets"]=True
        insert(GOLD,91000000021)
        insert(CERA,91000000021)
        # MySQL trigger deliberately fails the second DB after the first DB
        # has actually deleted its row, forcing the production recovery path.
        sql(f"CREATE TRIGGER {CERA}.native_test_fail BEFORE DELETE ON {CERA}.auction_main FOR EACH ROW SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='isolated rollback experiment';")
        operation(315,fail=True)
        assert count(GOLD)==1 and count(CERA)==1
        sql(f"DROP TRIGGER {CERA}.native_test_fail;")
        checks["second_market_failure_restores_first_and_second_database"]=True
        # Current PVF must match the saved index before restore.
        operation(311); previous=[sha256(p) for p in paths]
        later=configparser.ConfigParser(interpolation=None);later.read(instance/"services.ini",encoding="utf-16")
        later["gameplay"]={"max_level":"85"}
        with (instance/"services.ini").open("w",encoding="utf-16") as f:later.write(f)
        js_path=instance/"dp2/df_game_r.js"
        js_path.write_text(js_path.read_text(encoding="utf-8")+"\n// Independent later user change.\n",encoding="utf-8")
        operation(312);assert [sha256(p) for p in paths]==previous
        later.read(instance/"services.ini",encoding="utf-16")
        assert later["gameplay"]["max_level"]=="85" and "Independent later user change" in js_path.read_text(encoding="utf-8")
        checks["previous_index_restore_checks_matching_pvf"]=True
        checks["index_restore_preserves_unrelated_settings_and_script"]=True
        user.SendMessageW(user.GetDlgItem(market,303),0xF1,1,0)
        operation(313)
        js=(instance/"dp2/df_game_r.js").read_text(encoding="utf-8")
        assert "nativeAuctionDepth" in js and "HEX(jewel_data)" in js and "bytes[0] === 0" in js
        checks["dp2_search_protection_saved_by_apply"]=True
        operation(317)
        assert count(GOLD)==0 and count(CERA)==0
        checks["reset_covers_both_markets"]=True
        for p in (instance/".mysql").glob("market-*/*.sql.sha256"):
            assert sha256(Path(str(p)[:-7]))==p.read_text().strip()
        checks["official_sql_backups_have_matching_hashes"]=True
        screenshot(market,work/"market-window.png")
        user.PostMessageW(market,0x10,0,0);wait_for(lambda:not user.IsWindow(market),"close maintenance window")
        screenshot(window,work/"controller-window.png")
        # Actual original service startup must replay supported high-level,
        # high-refine stored orders twice, rather than merely keep a PID alive.
        high_level=next(int(line.split()[0]) for line in paths[0].read_text(encoding="utf-8").splitlines()
                        if re.match(r"^\d+ 2 (?:[01] ){11}85 ",line) and line.endswith("11105"))
        for db in [GOLD,CERA]:
            insert(db,91000000031,high_level,10)
            sql(f"UPDATE {db}.auction_main SET owner_id=100,owner_name='fixture',owner_nexon_id='fixture',price=100,instant_price=200,unit_price=200 WHERE auction_id=91000000031;")
        for launch in [1,2]:
            for name in ["auction","point"]:
                old=set((instance/"service-logs").glob(f"{name}-*.stdout.private.log"))
                statistics=instance/f"home/neople/{name}/log/statistics/SecSt_{30803 if name=='auction' else 30603}.log"
                old_stat=statistics.read_bytes() if statistics.exists() else b""
                select_service(window,name); click(window,221)
                ready(window,name+"：进程运行中",30)
                fresh=wait_for(lambda:next(iter(set((instance/"service-logs").glob(f"{name}-*.stdout.private.log"))-old),None),"fresh market stdout",20)
                # Original stdout is fully buffered with these two small services.
                # Wait for fresh application statistics, then inspect flushed
                # initialization and replay errors after normal shutdown.
                wait_for(lambda:statistics.exists() and statistics.read_bytes()!=old_stat and b"[REGIST_ITEM" in statistics.read_bytes(),"fresh original market work-loop statistics",45)
                click(window,222);ready_stopped(window,name,45)
                assert b"Server Frame Start Ver1.0" in fresh.read_bytes() and b"0904patch_bug" not in fresh.read_bytes()
                err=Path(str(fresh).replace(".stdout.",".stderr.")).read_bytes()
                assert b"guest exit status=0" in err and b"unhandled exception" not in err
                assert count(GOLD)==1 and count(CERA)==1
        checks["original_markets_replay_level85_refine10_orders_on_two_normal_starts"]=True
        click(window,112);wait_for(lambda:user.IsWindowEnabled(user.GetDlgItem(window,111)),"database stopped",60)
        user.PostMessageW(window,0x10,0,0);process.wait(timeout=20);process=None
        record["passed"]=True
    except Exception:
        record["passed"]=False;record["error"]=traceback.format_exc()
    finally:
        if process:
            if window and user.IsWindow(window) and (not market or not user.IsWindow(market) or user.IsWindowEnabled(market)):
                if market and user.IsWindow(market):user.PostMessageW(market,0x10,0,0)
                user.PostMessageW(window,0x10,0,0)
            try:process.wait(timeout=90)
            except subprocess.TimeoutExpired:process.terminate();process.wait(timeout=10)
        if server:
            sql("SHUTDOWN;",fail=True)
            try:server.wait(timeout=30)
            except subprocess.TimeoutExpired:server.terminate();server.wait(timeout=10)
        checks["source_database_unchanged"]=tree_identity(source/"data")==before
        record["passed"]=record.get("passed",False) and checks["source_database_unchanged"]
        (work/"receipt.json").write_text(json.dumps(record,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
        print("GUI auction:","PASS" if record["passed"] else "FAIL",work/"receipt.json",flush=True)
        if not record["passed"]:print(record.get("error","source changed"),flush=True)
    if not record["passed"]:raise SystemExit(1)

if __name__=="__main__":main()
