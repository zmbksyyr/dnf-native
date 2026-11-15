"""Verify that GUI backup coordinates an orderly stop of owned services."""

import argparse
import ctypes as c
import datetime
import json
from pathlib import Path
import platform
import subprocess

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from bootstrap import external_output, sha256, BUILD
from gui_support import user, windows, wait_for, click, ready, title, select_folder, screenshot
from gui_support import rows, choose_log_source

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--instance", type=Path, required=True)
    parser.add_argument("--gui", type=Path, required=True)
    args = parser.parse_args()
    instance, gui = external_output(args.instance).resolve(strict=True), args.gui.resolve(strict=True)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    probe = BUILD / "probes" / stamp / "gui-maintenance"
    probe.mkdir(parents=True)
    receipt = {"recorded_utc": stamp, "os": platform.platform(),
               "gui_sha256": sha256(gui), "script_sha256": sha256(Path(__file__)),
               "instance": str(instance), "checks": {}, "forced_cleanup": False}
    process = subprocess.Popen([str(gui), "--instance", str(instance)])
    window = None
    try:
        window = wait_for(lambda: next(iter(windows(process.pid, "DNFNativeController")), None), "main window")
        click(window, 223)
        ready(window, "已启动默认服务进程", 120)
        wait_for(lambda: user.IsWindowEnabled(user.GetDlgItem(window, 224)), "whole-stop enabled")
        running = sum("进程运行中" in row for row in rows(window))
        if running < 15:
            raise RuntimeError(f"Expected at least 15 owned peer services, got {running}")
        if user.IsWindowEnabled(user.GetDlgItem(window, 112)) or not user.IsWindowEnabled(user.GetDlgItem(window, 113)):
            raise RuntimeError("Coordinated backup controls are not in the expected running state")
        receipt["checks"]["backup_enabled_while_owned_services_run"] = True

        target = probe / "backups"
        target.mkdir()
        click(window, 113)
        select_folder(process, target)
        ready(window, "备份完成", 300)
        stopped = rows(window)
        if not all("已退出" in row for row in stopped[:15]):
            raise RuntimeError("Backup completed before every owned service stopped")
        choose_log_source(window, "\u63a7\u5236\u53f0\u64cd\u4f5c")
        output = title(user.GetDlgItem(window, 206))
        if "维护前已按逆序停止服务和数据库" not in output:
            raise RuntimeError("Maintenance log did not record the coordinated stop")
        if not list(target.glob("backup-*/manifest.txt")):
            raise RuntimeError("Coordinated backup did not create a manifest")
        receipt["checks"]["backup_stops_services_before_offline_operation"] = True
        screenshot(window, probe / "after-coordinated-backup.png")
    except Exception as error:
        receipt["error"] = type(error).__name__ + ": " + str(error)
        if window:
            screenshot(window, probe / "failure.private.png")
    finally:
        if process.poll() is None:
            if window:
                user.PostMessageW(window, 0x10, 0, 0)
            try:
                process.wait(timeout=180)
            except subprocess.TimeoutExpired:
                receipt["forced_cleanup"] = True
                subprocess.run(["taskkill.exe", "/PID", str(process.pid), "/T", "/F"], capture_output=True, timeout=15)
                process.wait(timeout=10)
        receipt["exit_code"] = process.returncode
        (probe / "receipt.json").write_text(json.dumps(receipt, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")
        print("GUI maintenance receipt:", probe / "receipt.json", flush=True)
    if receipt.get("error") or receipt["forced_cleanup"]:
        raise SystemExit(json.dumps(receipt.get("error", "forced cleanup"), ensure_ascii=True))


if __name__ == "__main__":
    main()
