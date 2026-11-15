"""Exercise native GUI orchestration against an already prepared private instance."""

import argparse
import configparser
import ctypes as c
import datetime
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess
import time
import traceback

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from bootstrap import sha256, BUILD
from probe_core_protocols import channel, relay, stun
from gui_support import (user, kernel, windows, wait_for, click, ready, status, title,
                         responsive, screenshot, select_folder, rows, list_rows, select_service, service_row, hidden_startup, choose_log_source)

from gui_support import guest_root
from runtime_support import runtime_diagnostics

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--instance", type=Path, required=True)
    parser.add_argument("--gui", type=Path, default=BUILD / "package/DNF-Native.exe")
    args = parser.parse_args()
    source, gui = args.instance.resolve(strict=True), args.gui.resolve(strict=True)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    probe = BUILD / "probes" / stamp / "gui-services"
    instance = probe / "instance"
    shutil.copytree(source, instance, ignore=shutil.ignore_patterns(".mysql", "service-logs"))
    manifest = configparser.ConfigParser(interpolation=None)
    manifest.read(instance / "services.ini", encoding="utf-16")
    manifest["game"]["automatic"] = "0"
    with (instance / "services.ini").open("w", encoding="utf-16") as stream:
        manifest.write(stream)
    receipt = {"recorded_utc": stamp, "os": platform.platform(), "gui_sha256": sha256(gui),
               "script_sha256": sha256(Path(__file__)), "source_services_sha256": sha256(source / "services.ini"),
               "scope": "Real GUI orchestration and original 15-peer protocols; game and security topology not exercised.",
               "checks": {}, "forced_cleanup": False}
    process = subprocess.Popen([str(gui), "--instance", str(instance)], startupinfo=hidden_startup())
    window = None
    elf = guest_root(instance) / "home/neople/channel/df_channel_r"
    original = elf.read_bytes()
    checks = receipt["checks"]
    previous_server_logs = set((instance / ".mysql").glob("gui-*-server.private.log"))
    try:
        window = wait_for(lambda: next(iter(windows(process.pid, "DNFNativeController")), None), "main window")
        assert len(rows(window)) == 19
        checks["nineteen_service_entries"] = True
        print("GUI: 19 original service entries", flush=True)
        # Headers are view rows, so verify both row types through the real controls.
        assert len(list_rows(window)) == 23
        assert not user.IsWindowEnabled(user.GetDlgItem(window, 221))
        click(window, 271)
        wait_for(lambda: len(rows(window)) == 11, "core group collapsed")
        click(window, 271)
        wait_for(lambda: len(rows(window)) == 19, "core group expanded")
        select_service(window, "game")
        def filtered(index):
            combo = user.GetDlgItem(window, 270)
            user.SendMessageW(combo, 0x14E, index, 0)
            user.SendMessageW(window, 0x111, 270 | (1 << 16), combo)
        for index, count in [(2, 8), (3, 4), (4, 3), (5, 4), (1, 0), (0, 19)]:
            filtered(index)
            assert len(rows(window)) == count, (index, list_rows(window))
        assert title(user.GetDlgItem(window, 219)).startswith("game  ·")
        assert "ELF：" in title(user.GetDlgItem(window, 272))
        assert "SHA-256：" in title(user.GetDlgItem(window, 272))
        assert "接口/协议/业务：未验证" in title(user.GetDlgItem(window, 273))
        assert "清单未声明" in title(user.GetDlgItem(window, 273))
        checks["four_groups_filters_identity_details_and_selection_retained"] = True
        click(window, 223)
        ready(window, "已启动默认服务进程", 90)
        wait_for(lambda: user.IsWindowEnabled(user.GetDlgItem(window, 224)), "whole-stop enabled")
        checks["start_window_response_ms"] = responsive(window)
        receipt["started_rows"] = rows(window)
        assert sum("进程运行中" in row for row in rows(window)) >= 15
        checks["fifteen_original_peer_processes"] = True
        select_service(window, "channel")
        assert "运行时通道已接入" in title(user.GetDlgItem(window, 273))
        assert "接口/协议/业务：未验证" in title(user.GetDlgItem(window, 273))
        checks["runtime_channel_does_not_claim_protocol_or_business_readiness"] = True
        checks["direct_stop_disabled_and_both_maintenance_actions_enabled_while_services_run"] = (
            not user.IsWindowEnabled(user.GetDlgItem(window, 112)) and
            user.IsWindowEnabled(user.GetDlgItem(window, 113)) and
            user.IsWindowEnabled(user.GetDlgItem(window, 114)) and
            not user.IsWindowEnabled(user.GetDlgItem(window, 101)))
        if not checks["direct_stop_disabled_and_both_maintenance_actions_enabled_while_services_run"]:
            raise RuntimeError("GUI did not expose the coordinated backup action while services were active")
        time.sleep(25)  # Original auction/point need their own startup work.
        checks["channel_protocol"] = channel(7001)
        checks["relay_and_real_monitor_rejection"] = relay(True)
        checks["stun_protocol"] = stun()
        print("GUI: channel, relay/monitor and STUN protocols passed", flush=True)
        screenshot(window, probe / "running.png")
        # Stop/restart only channel; the database and peers remain owned and active.
        select_service(window, "channel"); click(window, 222); ready(window, "channel：已退出", 45)
        checks["individual_channel_normal_stop"] = "代码 0" in service_row(window, "channel")
        click(window, 225)
        # Application files now have their own live view; select the runtime's
        # actual stderr explicitly when checking its guest-exit diagnostic.
        choose_log_source(window, "\u6807\u51c6\u9519\u8bef")
        wait_for(lambda: "guest exit status=0" in title(user.GetDlgItem(window, 206)), "actual service log displayed")
        checks["selected_real_log_visible"] = True
        print("GUI: individual stop and log display passed", flush=True)
        # A real hash mismatch in the private ELF must fail visibly, with other services retained.
        damaged = bytearray(original); damaged[-1] ^= 1; elf.write_bytes(damaged)
        click(window, 221)
        wait_for(lambda: status(window).startswith("操作失败"), "ELF identity rejection")
        wait_for(lambda: user.IsWindowEnabled(user.GetDlgItem(window, 221)), "failed start completed")
        checks["changed_elf_rejected_without_stopping_other_services"] = sum("进程运行中" in row for row in rows(window)) == 14
        elf.write_bytes(original)
        click(window, 221); ready(window, "channel：进程运行中", 20)
        time.sleep(1)
        checks["individual_channel_restart_protocol"] = channel(7001)
        print("GUI: ELF hash rejection and restart passed", flush=True)
        click(window, 224)
        ready(window, "服务进程已退出，数据库已停止", 120)
        receipt["stopped_rows"] = rows(window)
        peers = [name for name in manifest["services"].values() if name in manifest and manifest[name].get("automatic") == "1"]
        assert len(peers) == 15
        assert all("已退出" in service_row(window, name) for name in peers)
        # Original foreground CApplication mains return 1 after their normal
        # cleanup too. Require the observed baseline plus actual stop markers.
        code_one = {"manager", "dbmw_guild", "dbmw_mnt", "dbmw_stat", "monitor", "guild", "statics", "coserver"}
        assert all("代码 " + str(1 if name in code_one else 0) in service_row(window, name) for name in peers)
        for name in ["manager", "dbmw_guild", "dbmw_mnt", "dbmw_stat", "monitor", "guild", "statics", "coserver"]:
            log = max((instance / "service-logs").glob(name + "-*.stdout.private.log"), key=lambda path: path.stat().st_mtime)
            assert b"Application Stop!" in log.read_bytes(), name + " original stop marker missing"
        checks["reverse_order_whole_stop_completed"] = True
        checks["owned_pid_markers_removed_after_stop"] = not list(guest_root(instance).rglob("*.pid"))
        print("GUI: whole-stop completed", flush=True)
        checks["backup_enabled_after_whole_stop"] = bool(user.IsWindowEnabled(user.GetDlgItem(window, 113)))
        # Repeated launch and ordinary window-close must also stop every owned peer.
        click(window, 223); ready(window, "已启动默认服务进程", 90)
        time.sleep(25)
        assert sum("进程运行中" in row for row in rows(window)) >= 15
        checks["second_launch_channel_protocol"] = channel(7001)
        checks["second_launch_relay_and_monitor_protocol"] = relay(True)
        checks["second_launch_stun_protocol"] = stun()
        auto_backups = probe / "coordinated-backups"
        auto_backups.mkdir()
        click(window, 113)
        select_folder(process, auto_backups)
        wait_for(lambda: "正在停止服务" in status(window), "coordinated maintenance starts")
        ready(window, "备份完成", 300)
        assert all("已退出" in service_row(window, name) for name in peers)
        choose_log_source(window, "\u63a7\u5236\u53f0\u64cd\u4f5c")
        operation_log = title(user.GetDlgItem(window, 206))
        assert "维护前已按逆序停止服务和数据库" in operation_log
        stop_trace = re.findall(r"正在停止服务：([^\r\n]+)", operation_log)
        assert stop_trace[-len(peers):] == list(reversed(peers)), stop_trace
        checks["actual_stop_trace_matches_manifest_reverse_order"] = True
        checks["backup_stops_services_and_database_before_running"] = True
        assert "备份完成" in title(user.GetDlgItem(window, 274))
        assert str(auto_backups) in title(user.GetDlgItem(window, 274))
        checks["maintenance_identity_target_and_result_visible"] = True
        owned = [int(re.search(r"PID (\d+)", row)[1]) for row in rows(window)[:15] if re.search(r"PID (\d+)", row)]
        screenshot(window, probe / "before-close.png")
        user.PostMessageW(window, 0x10, 0, 0)
        process.wait(timeout=120)
        assert process.returncode == 0
        kernel.OpenProcess.argtypes, kernel.OpenProcess.restype = [c.c_ulong, c.c_int, c.c_ulong], c.c_void_p
        kernel.WaitForSingleObject.argtypes, kernel.WaitForSingleObject.restype = [c.c_void_p, c.c_ulong], c.c_ulong
        kernel.CloseHandle.argtypes = [c.c_void_p]
        for pid in owned:
            handle = kernel.OpenProcess(0x100000, False, pid)
            if handle:
                try:
                    assert kernel.WaitForSingleObject(handle, 0) == 0
                finally:
                    kernel.CloseHandle(handle)
        checks["close_normally_stops_all_owned_elf_hosts"] = True
        server_logs = set((instance / ".mysql").glob("gui-*-server.private.log")) - previous_server_logs
        checks["database_normal_shutdown_after_peers"] = len(server_logs) == 2 and all(b"Shutdown complete" in path.read_bytes() for path in server_logs)
        diagnostics = runtime_diagnostics(instance)
        checks["runtime_faults"] = diagnostics["faults"]
        checks["runtime_gaps"] = diagnostics["gaps"]
        assert not diagnostics["gaps"]
        for key, value in checks.items():
            if value is False:
                raise RuntimeError("Failed check: " + key)
    except Exception as error:
        receipt["error"] = type(error).__name__ + ": " + str(error)
        receipt["traceback"] = traceback.format_exc()
        if window and process.poll() is None:
            receipt["failure_rows"] = rows(window)
            receipt["failure_status"] = status(window)
            screenshot(window, probe / "failure.png")
    finally:
        if process.poll() is None:
            if window:
                user.PostMessageW(window, 0x10, 0, 0)
            try:
                process.wait(timeout=120)
            except subprocess.TimeoutExpired:
                receipt["forced_cleanup"] = True
                subprocess.run(["taskkill.exe", "/PID", str(process.pid), "/T", "/F"], capture_output=True, timeout=15)
                process.wait(timeout=10)
        # Stop the hosts before restoring the deliberately damaged ELF.  The
        # previous order masked the real probe failure with a Windows sharing
        # violation from a still-running host.
        elf.write_bytes(original)
        diagnostics = runtime_diagnostics(instance)
        checks["runtime_faults"] = diagnostics["faults"]
        checks["runtime_gaps"] = diagnostics["gaps"]
        if diagnostics["faults"] and not receipt.get("error"):
            receipt["error"] = "Original service exception: " + diagnostics["faults"][0]
        receipt["exit_code"] = process.returncode
        receipt["service_logs"] = {path.name: sha256(path) for path in sorted((instance / "service-logs").glob("*.log"))}
        (probe / "receipt.json").write_text(json.dumps(receipt, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")
        print("GUI service receipt:", probe / "receipt.json", flush=True)
    if receipt.get("error") or receipt["forced_cleanup"]:
        raise SystemExit(json.dumps(receipt.get("error", "forced cleanup"), ensure_ascii=True))


if __name__ == "__main__":
    main()
