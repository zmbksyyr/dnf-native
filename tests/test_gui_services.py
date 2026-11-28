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
from gui_support import (user, kernel, windows, wait_for, click, ready, ready_stopped, status, title, log_text,
                         stopped_text, responsive, screenshot, select_folder, rows, list_rows, select_service, service_row,
                         hidden_startup, choose_log_source, service_details, database_details)

from gui_support import guest_root
from runtime_support import runtime_diagnostics
from test_gui_keyboard import focus, key as keyboard_key, menu_open

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
    robot_enabled = manifest.getboolean("robot", "enabled", fallback=False)
    original_peer_count = sum(1 for key, name in manifest["services"].items()
                              if key.startswith("service") and name in manifest and
                              manifest[name].get("automatic") == "1")
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
        expected_entries = 19 + int(manifest.getboolean("robot", "enabled", fallback=False))
        assert len(rows(window)) == expected_entries
        checks["service_entries_including_enabled_external_robot"] = True
        print("GUI: service entries", expected_entries, flush=True)
        assert len(list_rows(window)) == expected_entries
        assert not user.GetDlgItem(window, 221)
        select_service(window, "game")
        details = service_details(window)
        assert "game  ·" in details, details
        assert "ELF：" in details and "SHA-256：" in details, details
        assert "就绪层级：未启动" in details and "清单未声明" in details, details
        checks["service_cards_keep_identity_details_and_selection"] = True
        click(window, 223)
        ready(window, "已启动默认服务进程", 90)
        wait_for(lambda: user.IsWindowEnabled(user.GetDlgItem(window, 224)), "whole-stop enabled")
        checks["start_window_response_ms"] = responsive(window)
        receipt["started_rows"] = rows(window)
        assert sum("进程运行中" in row for row in rows(window)) >= original_peer_count
        checks["original_peer_processes"] = original_peer_count
        select_service(window, "channel")
        details = service_details(window)
        assert "运行时通道已接入" in details, details
        assert "就绪层级：运行时通道已接入\r\n" in details, details
        checks["current_launch_runtime_channel_status"] = True
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
        select_service(window, "channel")
        user.ShowWindow(window,4)
        def context_action(letter):
            grid=user.GetDlgItem(window,220)
            focus(window,grid)
            user.PostMessageW(window,0x7B,grid,-1)
            wait_for(lambda:menu_open(window),"service context menu")
            keyboard_key(window,ord(letter))
            wait_for(lambda:not menu_open(window),"context command activated")
        context_action("T")
        ready_stopped(window, "channel", 45)
        checks["individual_channel_normal_stop"] = "代码 0" in service_row(window, "channel")
        select_service(window, "channel")
        # Application files now have their own live view; select the runtime's
        # actual stderr explicitly when checking its guest-exit diagnostic.
        choose_log_source(window, ".stderr.private.log")
        wait_for(lambda: "guest exit status=0" in title(user.GetDlgItem(window, 206)), "actual service log displayed")
        checks["selected_real_log_visible"] = True
        print("GUI: individual stop and log display passed", flush=True)
        # A real hash mismatch in the private ELF must fail visibly, with other services retained.
        damaged = bytearray(original); damaged[-1] ^= 1; elf.write_bytes(damaged)
        click(window, 221)
        wait_for(lambda: status(window).startswith("操作失败"), "ELF identity rejection")
        wait_for(lambda: user.IsWindowEnabled(user.GetDlgItem(window, 220)), "failed start completed")
        checks["changed_elf_rejected_without_stopping_other_services"] = (
            sum("进程运行中" in row for row in rows(window)) == original_peer_count - 1 + int(robot_enabled))
        elf.write_bytes(original)
        baseline = len(log_text(window))
        context_action("S"); ready(window, "channel：进程运行中", 20, since=baseline)
        time.sleep(1)
        checks["individual_channel_restart_protocol"] = channel(7001)
        checks["real_context_menu_start_and_stop_without_toolbar_buttons"] = True
        print("GUI: ELF hash rejection and restart passed", flush=True)
        counts=[int(title(user.GetDlgItem(window,213)).split('/')[0])]
        baseline=len(log_text(window))
        click(window, 224)
        def whole_stop_finished():
            counts.append(int(title(user.GetDlgItem(window,213)).split('/')[0]))
            text=log_text(window)[baseline:]
            if "操作失败" in text: raise RuntimeError(text[-400:])
            return "服务进程已退出，数据库已停止" in text
        wait_for(whole_stop_finished,"whole stop with live count",120)
        wait_for(lambda:title(user.GetDlgItem(window,213)).startswith('0 /'),"zero running count",5)
        expected_live = original_peer_count + int(robot_enabled)
        assert max(counts)==expected_live,counts
        checks["live_count_updates_before_whole_stop_completes"] = any(0<n<expected_live for n in counts)
        receipt["whole_stop_live_counts"] = list(dict.fromkeys(counts))
        receipt["stopped_rows"] = rows(window)
        peers = [name for name in manifest["services"].values() if name in manifest and manifest[name].get("automatic") == "1"]
        assert len(peers) == 15
        assert all(stopped_text(service_row(window, name)) for name in peers)
        # Original foreground CApplication mains return 1 after their normal
        # cleanup too. Require the observed baseline plus actual stop markers.
        code_one = {"manager", "dbmw_guild", "dbmw_mnt", "dbmw_stat", "monitor", "guild", "statics", "coserver"}
        assert all(int(re.search(r"代码 (\d+)$", service_row(window, name))[1]) ==
                   (1 if name in code_one else 0) for name in peers)
        for name in ["manager", "dbmw_guild", "dbmw_mnt", "dbmw_stat", "monitor", "guild", "statics", "coserver"]:
            log = max((instance / "service-logs").glob(name + "-*.stdout.private.log"), key=lambda path: path.stat().st_mtime)
            assert b"Application Stop!" in log.read_bytes(), name + " original stop marker missing"
        checks["reverse_order_whole_stop_completed"] = True
        screenshot(window,probe/"after-whole-stop.png")
        checks["owned_pid_markers_removed_after_stop"] = not list(guest_root(instance).rglob("*.pid"))
        print("GUI: whole-stop completed", flush=True)
        checks["backup_enabled_after_whole_stop"] = bool(user.IsWindowEnabled(user.GetDlgItem(window, 113)))
        # Repeated launch and ordinary window-close must also stop every owned peer.
        click(window, 223); ready(window, "已启动默认服务进程", 90)
        time.sleep(25)
        assert sum("进程运行中" in row for row in rows(window)) >= original_peer_count
        checks["second_launch_channel_protocol"] = channel(7001)
        checks["second_launch_relay_and_monitor_protocol"] = relay(True)
        checks["second_launch_stun_protocol"] = stun()
        auto_backups = probe / "coordinated-backups"
        auto_backups.mkdir()
        click(window, 113)
        select_folder(process, auto_backups)
        wait_for(lambda: "正在停止服务" in log_text(window), "coordinated maintenance starts")
        ready(window, "备份完成", 300)
        assert all(stopped_text(service_row(window, name)) for name in peers)
        choose_log_source(window, "\u63a7\u5236\u53f0\u64cd\u4f5c\u8bb0\u5f55")
        operation_log = title(user.GetDlgItem(window, 206))
        assert "维护前已按逆序停止服务和数据库" in operation_log
        stop_trace = re.findall(r"正在停止服务：([^\r\n]+)", operation_log)
        assert stop_trace[-len(peers):] == list(reversed(peers)), stop_trace
        checks["actual_stop_trace_matches_manifest_reverse_order"] = True
        checks["backup_stops_services_and_database_before_running"] = True
        maintenance = database_details(window)
        assert "备份完成" in maintenance, maintenance
        assert str(auto_backups) in maintenance, maintenance
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
