"""Exercise creation, protocols and cold maintenance in an existing NTFS long directory."""
import argparse
import ctypes as c
import datetime
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from bootstrap import BUILD, sha256
from gui_support import (user, windows, wait_for, click, ready, status, title, rows,
                         select_folder, hidden_startup, screenshot)
from probe_core_protocols import channel, relay, stun
from runtime_support import runtime_diagnostics


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gui", type=Path, required=True)
    args = parser.parse_args()
    gui = args.gui.resolve(strict=True)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    probe = BUILD / "probes" / stamp / "gui-long-path"
    logical = probe / ("long-a-" + "a" * 80) / ("long-b-" + "b" * 80) / "game instance"
    physical = Path("\\\\?\\" + str(logical))
    physical.mkdir(parents=True)
    record = {"recorded_utc": stamp, "os": platform.platform(), "gui_sha256": sha256(gui),
              "script_sha256": sha256(Path(__file__)), "instance_path_length": len(str(logical)),
              "scope": "Real GUI long-directory creation, original protocols, natural game initialization and cold backup/restore; existing filesystem short names required.",
              "forced_cleanup": False, "checks": {}}
    checks = record["checks"]
    process, window, duplicate = None, None, None
    try:
        assert len(str(logical)) >= 260
        kernel = c.WinDLL("kernel32", use_last_error=True)
        kernel.GetShortPathNameW.argtypes = [c.c_wchar_p, c.c_wchar_p, c.c_ulong]
        buffer = c.create_unicode_buffer(32768)
        size = kernel.GetShortPathNameW(str(physical), buffer, len(buffer))
        if not size or size >= len(buffer):
            raise RuntimeError("This NTFS test requires an existing usable short name")
        short = Path(buffer.value[4:] if buffer.value.startswith("\\\\?\\") else buffer.value)
        if len(str(short)) >= 260:
            raise RuntimeError("This volume did not supply a short working-directory path")
        checks["filesystem_short_name_available"] = True
        process = subprocess.Popen([str(gui)], startupinfo=hidden_startup())
        window = wait_for(lambda: next(iter(windows(process.pid, "DNFNativeController")), None), "controller")
        click(window, 260)
        select_folder(process, logical)
        ready(window, "\u6e38\u620f\u5b9e\u4f8b\u521b\u5efa\u5b8c\u6210", 240)
        assert title(user.GetDlgItem(window, 101)) == str(logical)
        assert len(rows(window)) == 19 and not (physical / "instance.pending").exists()
        assert os.path.samefile(physical / "data/auto.cnf", short / "data/auto.cnf")
        checks["folder_picker_created_long_instance_and_preserved_display"] = True
        checks["short_and_long_names_have_same_file_identity"] = True

        duplicate = subprocess.Popen([str(gui), "--instance", str(short)], startupinfo=hidden_startup())
        other = wait_for(lambda: next(iter(windows(duplicate.pid, "DNFNativeController")), None), "duplicate controller")
        wait_for(lambda: "\u53e6\u4e00\u4e2a\u7a97\u53e3" in status(other), "same physical instance lock")
        assert not user.IsWindowEnabled(user.GetDlgItem(other, 111))
        user.PostMessageW(other, 0x10, 0, 0)
        duplicate.wait(timeout=30)
        checks["same_instance_lock_across_long_and_short_names"] = True

        click(window, 223)
        ready(window, "\u5df2\u542f\u52a8\u9ed8\u8ba4\u670d\u52a1\u8fdb\u7a0b", 90)
        start = time.monotonic()
        wait_for(lambda: any(b"server has been started successfully" in p.read_bytes()
                             for p in (physical / "home/neople/game/log").rglob("*.init")),
                 "original game initialization", 180)
        record["game_startup_seconds"] = round(time.monotonic() - start, 3)
        checks["channel_protocol"] = channel(7001)
        checks["relay_monitor_protocol"] = relay(True)
        checks["stun_protocol"] = stun()
        query = subprocess.run([str(gui.parent / "resources/mysql-control.exe"), "mysql", str(logical),
                                "--defaults-file=client.private.cnf", "--batch", "--skip-column-names",
                                "--execute=SELECT VERSION();"], capture_output=True, timeout=15)
        assert query.returncode == 0 and b"5.7.44" in query.stdout
        checks["maintenance_cli_accepts_original_long_path"] = True
        screenshot(window, probe / "running.png")
        print("Long path: creation, lock, original startup and protocols passed", flush=True)

        backups = probe / "backups"
        backups.mkdir()
        click(window, 113)
        select_folder(process, backups)
        ready(window, "\u5907\u4efd\u5b8c\u6210", 300)
        backup = next(p for p in backups.iterdir() if (p / "manifest.txt").is_file())
        assert (backup / "logical.sql").stat().st_size > 0
        game_out = "\n".join(p.read_text(encoding="utf8", errors="replace")
                             for p in (physical / "service-logs").glob("game-*.stdout.private.log"))
        assert all(marker in game_out for marker in ["main() exited OK", "dispose", "unload"])
        checks["coordinated_backup_and_natural_game_plugin_shutdown"] = True
        click(window, 114)
        select_folder(process, backup)
        confirmation = wait_for(lambda: next((h for h in windows(process.pid, "#32770")
                                             if title(h) == "\u786e\u8ba4\u8fd8\u539f"), None),
                                "restore confirmation")
        click(confirmation, 1)
        ready(window, "\u8fd8\u539f\u5df2\u9a8c\u8bc1\u5e76\u63d0\u4ea4", 300)
        assert not (physical / "restore.pending").exists()
        checks["shadow_restore_verified_and_committed"] = True
        click(window, 111)
        ready(window, "\u6570\u636e\u5e93 SQL \u68c0\u67e5\u901a\u8fc7", 90)
        checks["restored_database_identity_and_tls"] = True
        click(window, 112)
        ready(window, "\u6570\u636e\u5e93\u5df2\u6b63\u5e38\u505c\u6b62", 90)
        checks["database_normal_stop"] = True
    except Exception as error:
        record["error"] = type(error).__name__ + ": " + str(error)
        if window and process.poll() is None:
            record["failure_status"] = status(window)
            screenshot(window, probe / "failure.png")
    finally:
        for child in [duplicate, process]:
            if child and child.poll() is None:
                for handle in windows(child.pid, "DNFNativeController"):
                    user.PostMessageW(handle, 0x10, 0, 0)
                try:
                    child.wait(timeout=120)
                except subprocess.TimeoutExpired:
                    record["forced_cleanup"] = True
                    subprocess.run(["taskkill.exe", "/PID", str(child.pid), "/T", "/F"], capture_output=True)
                    child.wait(timeout=15)
        diagnostics = runtime_diagnostics(physical)
        checks["runtime_faults"] = diagnostics["faults"]
        checks["runtime_gaps"] = diagnostics["gaps"]
        known = all(any("nr=" + str(n) + " " in gap for n in [291, 332]) for gap in diagnostics["gaps"])
        record["passed"] = (not record.get("error") and not record["forced_cleanup"] and
                            not diagnostics["faults"] and known and checks.get("database_normal_stop", False))
        record["gui_exit_code"] = process.returncode if process else None
        (probe / "receipt.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf8")
        print("Long-path GUI:", "PASS" if record["passed"] else "FAIL", probe / "receipt.json", flush=True)
    if not record["passed"]:
        raise SystemExit(record.get("error", "Long-path runtime failure"))


if __name__ == "__main__":
    main()
