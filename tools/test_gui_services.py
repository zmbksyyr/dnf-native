"""Exercise native GUI orchestration against an already prepared private instance."""

import argparse
import ctypes as c
import datetime
import json
from pathlib import Path
import platform
import re
import subprocess
import time

from bootstrap import sha256
from probe_core_protocols import channel, relay, stun
from test_gui import user, kernel, windows, wait_for, click, ready, status, title, responsive, screenshot

ROOT = Path(__file__).resolve().parents[1]


def rows(window):
    box = user.GetDlgItem(window, 220)
    result = []
    for index in range(user.SendMessageW(box, 0x18B, 0, 0)):  # LB_GETCOUNT
        text = c.create_unicode_buffer(user.SendMessageW(box, 0x18A, index, 0) + 1)
        user.SendMessageW(box, 0x189, index, c.addressof(text))
        result.append(text.value)
    return result


def select(window, index):
    user.SendMessageW(user.GetDlgItem(window, 220), 0x186, index, 0)
    user.SendMessageW(window, 0x111, 220 | (1 << 16), user.GetDlgItem(window, 220))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--instance", type=Path, required=True)
    parser.add_argument("--gui", type=Path, default=ROOT / "build/gui-services/DNF-Native.exe")
    args = parser.parse_args()
    instance, gui = args.instance.resolve(strict=True), args.gui.resolve(strict=True)
    prepared = json.loads((instance / "prepare-receipt.json").read_text(encoding="utf-8"))
    if not prepared["peers_only"]:
        raise SystemExit("This short probe requires a --peers-only private instance")
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    probe = ROOT / "build/probes" / stamp / "gui-services"
    probe.mkdir(parents=True)
    receipt = {"recorded_utc": stamp, "os": platform.platform(), "gui_sha256": sha256(gui),
               "script_sha256": sha256(Path(__file__)), "prepare_receipt_sha256": sha256(instance / "prepare-receipt.json"),
               "scope": "Real GUI orchestration and original 15-peer protocols; game and security topology not exercised.",
               "checks": {}, "forced_cleanup": False}
    process = subprocess.Popen([str(gui), "--instance", str(instance)])
    window = None
    elf = instance / "i686/home/neople/channel/df_channel_r"
    original = elf.read_bytes()
    checks = receipt["checks"]
    previous_server_logs = set((instance / ".mysql").glob("gui-*-server.private.log"))
    try:
        window = wait_for(lambda: next(iter(windows(process.pid, "DNFNativeController")), None), "main window")
        assert len(rows(window)) == 19
        checks["nineteen_service_entries"] = True
        print("GUI: 19 original service entries", flush=True)
        click(window, 223)
        ready(window, "已启动默认服务进程", 90)
        wait_for(lambda: user.IsWindowEnabled(user.GetDlgItem(window, 224)), "whole-stop enabled")
        checks["start_window_response_ms"] = responsive(window)
        receipt["started_rows"] = rows(window)
        assert sum("进程运行中" in row for row in rows(window)) == 15
        checks["fifteen_original_peer_processes"] = True
        checks["database_stop_and_backup_disabled_while_services_run"] = all(not user.IsWindowEnabled(user.GetDlgItem(window, i)) for i in [112, 113, 114, 101])
        if not checks["database_stop_and_backup_disabled_while_services_run"]:
            raise RuntimeError("Database maintenance was enabled while writers were active")
        time.sleep(25)  # Original auction/point need their own startup work.
        checks["channel_protocol"] = channel(7001)
        checks["relay_and_real_monitor_rejection"] = relay(True)
        checks["stun_protocol"] = stun()
        print("GUI: channel, relay/monitor and STUN protocols passed", flush=True)
        screenshot(window, probe / "running.png")
        # Stop/restart only channel; the database and peers remain owned and active.
        select(window, 12); click(window, 222); ready(window, "channel：已退出", 45)
        checks["individual_channel_normal_stop"] = "代码 0" in rows(window)[12]
        click(window, 225)
        # Application files now have their own live view; select the runtime's
        # actual stderr explicitly when checking its guest-exit diagnostic.
        combo = user.GetDlgItem(window, 240)
        for log_index in range(user.SendMessageW(combo, 0x146, 0, 0)):
            text = c.create_unicode_buffer(user.SendMessageW(combo, 0x149, log_index, 0) + 1)
            user.SendMessageW(combo, 0x148, log_index, c.addressof(text))
            if '标准错误' in text.value:
                user.SendMessageW(combo, 0x14E, log_index, 0)
                user.SendMessageW(window, 0x111, 240 | (1 << 16), combo)
                break
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
        assert all("已退出" in row for row in rows(window)[:15])
        # Original foreground CApplication mains return 1 after their normal
        # cleanup too. Require the observed baseline plus actual stop markers.
        assert all("代码 " + str(1 if 1 <= index <= 8 else 0) in row for index, row in enumerate(rows(window)[:15]))
        for name in ["manager", "dbmw_guild", "dbmw_mnt", "dbmw_stat", "monitor", "guild", "statics", "coserver"]:
            log = max((instance / "service-logs").glob(name + "-*.stdout.private.log"), key=lambda path: path.stat().st_mtime)
            assert b"Application Stop!" in log.read_bytes(), name + " original stop marker missing"
        checks["reverse_order_whole_stop_completed"] = True
        checks["owned_pid_markers_removed_after_stop"] = not list((instance / "i686").rglob("*.pid"))
        print("GUI: whole-stop completed", flush=True)
        checks["backup_enabled_only_after_whole_stop"] = bool(user.IsWindowEnabled(user.GetDlgItem(window, 113)))
        # Repeated launch and ordinary window-close must also stop every owned peer.
        click(window, 223); ready(window, "已启动默认服务进程", 90)
        time.sleep(25)
        assert sum("进程运行中" in row for row in rows(window)) == 15
        checks["second_launch_channel_protocol"] = channel(7001)
        checks["second_launch_relay_and_monitor_protocol"] = relay(True)
        checks["second_launch_stun_protocol"] = stun()
        owned = [int(re.search(r"PID (\d+)", row)[1]) for row in rows(window)[:15]]
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
        gaps = []
        for path in (instance / "service-logs").glob("*.stderr.private.log"):
            for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
                if any(text in line for text in ["syscall gap", "unhandled exception", "unsupported", "exhausted"]):
                    gaps.append(line)
        checks["runtime_gaps"] = sorted(set(gaps))
        assert not gaps
        for key, value in checks.items():
            if value is False:
                raise RuntimeError("Failed check: " + key)
    except Exception as error:
        receipt["error"] = type(error).__name__ + ": " + str(error)
    finally:
        elf.write_bytes(original)
        if process.poll() is None:
            if window:
                user.PostMessageW(window, 0x10, 0, 0)
            try:
                process.wait(timeout=120)
            except subprocess.TimeoutExpired:
                receipt["forced_cleanup"] = True
                subprocess.run(["taskkill.exe", "/PID", str(process.pid), "/T", "/F"], capture_output=True, timeout=15)
                process.wait(timeout=10)
        receipt["exit_code"] = process.returncode
        receipt["service_logs"] = {path.name: sha256(path) for path in sorted((instance / "service-logs").glob("*.log"))}
        (probe / "receipt.json").write_text(json.dumps(receipt, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")
        print("GUI service receipt:", probe / "receipt.json", flush=True)
    if receipt.get("error") or receipt["forced_cleanup"]:
        raise SystemExit(json.dumps(receipt.get("error", "forced cleanup"), ensure_ascii=True))


if __name__ == "__main__":
    main()
