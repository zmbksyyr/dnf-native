"""Exercise the delivered ZIP resources and real Win32 instance lifecycle."""

import argparse
import ctypes as c
import datetime
import json
import os
from pathlib import Path
import platform
import subprocess
import time
import traceback
import zipfile

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from bootstrap import sha256, BUILD
from probe_core_protocols import channel, relay, stun
from gui_support import (user, windows, wait_for, click, ready, status, title,
                      select_folder, screenshot, hidden_startup, responsive)
from gui_support import rows, select_service

from gui_support import guest_root
from runtime_support import runtime_diagnostics

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gui", type=Path, required=True)
    args = parser.parse_args()
    gui = args.gui.resolve(strict=True)
    resources = gui.parent / "resources"
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    probe = BUILD / "probes" / stamp / "instance-workflow"
    probe.mkdir(parents=True)
    record = {"recorded_utc": stamp, "os": platform.platform(), "gui_sha256": sha256(gui),
              "script_sha256": sha256(Path(__file__)), "checks": {}, "forced_cleanup": False}
    checks = record["checks"]
    before = {p.name: sha256(p) for p in resources.iterdir() if p.is_file()}
    process = subprocess.Popen([str(gui)], startupinfo=hidden_startup())
    window = None
    renamed = None
    damaged_resource = None
    second_process = None

    def choose(path):
        text = c.create_unicode_buffer(str(path))
        user.SendMessageW(user.GetDlgItem(window, 101), 0xC, 0, c.addressof(text))

    def create(path):
        path.mkdir()
        click(window, 260)
        select_folder(process, path)
        wait_for(lambda: (path / "instance.pending").exists(), "ZIP extraction begins")
        checks["creation_response_ms"] = responsive(window)
        ready(window, "游戏实例创建完成", 240)
        wait_for(lambda: user.IsWindowEnabled(user.GetDlgItem(window, 111)), "created instance controls")
        assert title(user.GetDlgItem(window, 101)) == str(path)
        assert len(rows(window)) == 19
        assert (path / "mysql/bin/mysqld.exe").is_file() and (path / "data/auto.cnf").is_file()
        assert not (path / "instance.pending").exists()
        assert not (path / ".mysql/setup-seed.sql").exists()
        assert not (path / ".mysql/setup-services.ini").exists()
        for tool in ["mysqld", "mysql", "mysqladmin", "mysqldump"]:
            r = subprocess.run([str(path / "mysql/bin" / (tool + ".exe")), "--version"], capture_output=True, timeout=15)
            assert not r.returncode and b"5.7.44" in r.stdout
        return path

    def confirm_delete(yes):
        click(window, 261)
        dialog = wait_for(lambda: next((w for w in windows(process.pid, "#32770") if title(w) == "删除实例"), None), "delete confirmation")
        click(dialog, 1 if yes else 2)

    def create_failure(path):
        click(window, 260)
        select_folder(process, path)
        wait_for(lambda: status(window).startswith("操作失败"), "creation rejection")
        wait_for(lambda: user.IsWindowEnabled(user.GetDlgItem(window, 260)), "failed creation finished")

    try:
        window = wait_for(lambda: next(iter(windows(process.pid, "DNFNativeController")), None), "controller")
        assert user.IsWindowEnabled(user.GetDlgItem(window, 260))
        assert "资源齐全" in title(user.GetDlgItem(window, 218))
        assert not title(user.GetDlgItem(window, 101))
        checks["cold_start_idle_and_create_available"] = True
        for name in ["mysql.zip", "native.zip", "dnf-host-x86.exe", "dnf-host-x64.exe", "mysql-control.exe", "resources.ini"]:
            p = resources / name
            missing = p.with_suffix(p.suffix + ".test-missing")
            p.rename(missing); renamed = missing
            click(window, 262)
            assert name in title(user.GetDlgItem(window, 218))
            assert not user.IsWindowEnabled(user.GetDlgItem(window, 260))
            renamed.rename(p); renamed = None
            click(window, 262)
        checks["each_missing_resource_named_and_creation_disabled"] = True
        with zipfile.ZipFile(resources / "native.zip") as z:
            names = z.namelist()
            assert "home/neople/game/df_game_r" in names
            assert "home/neople/game/Script.pvf" in names
            assert "dp2/frida/frida.so" in names
            assert not any("/dracut/" in n or "/gdb/" in n or n.endswith((".pyc", ".pyo")) for n in names)
            assert z.testzip() is None
            record["native_file_count"] = len(names)
        checks["native_zip_has_browsable_paths_and_trimmed_files"] = True
        damaged_resource = resources / "native.zip"
        with damaged_resource.open("r+b") as stream:
            stream.seek(-1, 2); saved_byte = stream.read(1)
            stream.seek(-1, 2); stream.write(bytes([saved_byte[0] ^ 1]))
        rejected = probe / "damaged resource"
        rejected.mkdir()
        create_failure(rejected)
        assert not list(rejected.iterdir()), "Resource identity rejection wrote into target directory"
        with damaged_resource.open("r+b") as stream:
            stream.seek(-1, 2); stream.write(saved_byte)
        damaged_resource = None
        checks["changed_zip_rejected_before_instance_writes"] = True
        occupied = probe / "occupied"
        occupied.mkdir(); (occupied / "keep.txt").write_bytes(b"user contents\x00\xff")
        create_failure(occupied)
        assert list(occupied.iterdir()) == [occupied / "keep.txt"]
        assert (occupied / "keep.txt").read_bytes() == b"user contents\x00\xff"
        checks["nonempty_creation_target_unchanged"] = True
        first = create(probe / "实例 A (native)")
        assert not (first / "i686").exists() and not (first / "x86_64").exists()
        assert (first / "home/neople/game/df_game_r").is_file()
        assert (first / "home/neople/game/Script.pvf").is_file()
        assert (first / "home/neople/stun").is_dir()
        assert (first / "lib/ld-2.17.so").is_file()
        assert (first / "lib64").is_dir()
        checks["unified_game_assets_stun_and_both_library_architectures"] = True
        checks["first_created_selected_and_official_tools_released"] = True
        screenshot(window, probe / "created.png")
        click(window, 111); ready(window, "数据库 SQL 检查通过")
        assert not user.IsWindowEnabled(user.GetDlgItem(window, 261))
        assert not user.IsWindowEnabled(user.GetDlgItem(window, 260))
        tool = resources / "mysql-control.exe"
        assert (first / "login.private.cnf").is_file()
        result = subprocess.run([str(tool), "mysql", str(first), "--defaults-file=login.private.cnf", "--batch", "--skip-column-names",
                                 "--execute=SELECT CURRENT_USER(); SELECT COUNT(*) FROM d_taiwan.accounts; SELECT COUNT(*) FROM d_taiwan.member_info; SELECT COUNT(*) FROM d_taiwan.member_white_account; SELECT COUNT(*) FROM taiwan_login.member_login;"],
                                capture_output=True, timeout=30)
        assert result.returncode == 0 and result.stdout.splitlines()[0] == b"game@%", "Launcher game account cannot authenticate/read login tables"
        checks["game_launcher_account_unlocked_and_login_tables_readable"] = True
        import configparser
        login = configparser.ConfigParser(interpolation=None)
        login.read(first / "login.private.cnf", encoding="utf-8-sig")
        assert {key: login["client"][key] for key in ("host", "port", "user", "password")} == {
            "host": "127.0.0.1", "port": "3306", "user": "game", "password": "uu5!^%jg"}
        result = subprocess.run([str(tool), "mysql", str(first), "--defaults-file=login.private.cnf", "--batch", "--skip-column-names",
            "--execute=SELECT COUNT(*) FROM d_taiwan.accounts a JOIN d_taiwan.member_info i ON i.m_id=a.UID JOIN d_taiwan.member_white_account w ON w.m_id=a.UID JOIN taiwan_login.member_login l ON l.m_id=a.UID WHERE a.accountname='test' AND a.password=MD5('123456');"],
            capture_output=True, timeout=30)
        assert result.returncode == 0 and result.stdout.strip() == b"1"
        checks["requested_default_connection_and_test_account_four_tables"] = True
        result = subprocess.run([str(tool), "mysql", str(first), "--defaults-file=client.private.cnf", "--batch", "--skip-column-names",
                                 "--execute=CREATE DATABASE instance_probe; CREATE TABLE instance_probe.saved(id INT PRIMARY KEY); INSERT INTO instance_probe.saved VALUES(73);"],
                                capture_output=True, timeout=30)
        assert result.returncode == 0
        checks["database_runs_from_instance_and_delete_disabled_while_running"] = True
        click(window, 112); ready(window, "数据库已正常停止")
        second = create(probe / "实例 B (native)")
        checks["second_created_and_automatically_selected"] = True
        second_process = subprocess.Popen([str(gui), "--instance", str(second)], startupinfo=hidden_startup())
        other_window = wait_for(lambda: next(iter(windows(second_process.pid, "DNFNativeController")), None), "second controller")
        assert "另一个窗口" in status(other_window), "Instance ownership rejection is missing"
        assert not title(user.GetDlgItem(other_window, 101))
        assert not user.IsWindowEnabled(user.GetDlgItem(other_window, 261))
        assert not user.IsWindowEnabled(user.GetDlgItem(other_window, 111))
        user.PostMessageW(other_window, 0x10, 0, 0); second_process.wait(timeout=20)
        assert second_process.returncode == 0
        second_process = None
        checks["second_controller_cannot_start_or_delete_owned_instance"] = True
        choose(first); click(window, 111); ready(window, "数据库 SQL 检查通过")
        result = subprocess.run([str(tool), "mysql", str(first), "--defaults-file=client.private.cnf", "--batch", "--skip-column-names",
                                 "--execute=SELECT id FROM instance_probe.saved;"], capture_output=True, timeout=30)
        assert result.returncode == 0 and result.stdout.strip() == b"73"
        click(window, 112); ready(window, "数据库已正常停止")
        checks["switch_to_existing_preserves_saved_database"] = True
        choose(second)
        confirm_delete(False)
        assert second.is_dir()
        checks["delete_cancel_preserves_entire_instance"] = True
        extra = second / "user files/deep"
        extra.mkdir(parents=True); (extra / "keep.txt").write_text("entire folder must be removed", encoding="ascii")
        os.chmod(extra / "keep.txt", 0o444)
        external = probe / "external files"
        external.mkdir(); (external / "keep.txt").write_bytes(b"outside instance")
        subprocess.run(["cmd.exe", "/c", "mklink", "/J", str(second / "external link"), str(external)], check=True, capture_output=True)
        confirm_delete(True); ready(window, "实例文件夹已整体删除", 60)
        assert not second.exists() and first.exists()
        assert (external / "keep.txt").read_bytes() == b"outside instance"
        assert not title(user.GetDlgItem(window, 101)) and not rows(window)
        checks["delete_removes_whole_tree_and_clears_selection"] = True
        assert before == {p.name: sha256(p) for p in resources.iterdir() if p.is_file()}
        checks["shared_resources_unchanged"] = True
        record["first_instance"] = str(first)
        choose(first)
        click(window, 111); ready(window, "数据库 SQL 检查通过")
        # Use the instance's actual startup order; visual grouping has its own order.
        import configparser
        services = configparser.ConfigParser(interpolation=None)
        services.read(first / "services.ini", encoding="utf-16")
        for index in range(15):
            name = services["services"]["service" + str(index)]
            select_service(window, name); click(window, 221)
            ready(window, name + "：进程运行中", 30)
        time.sleep(20)
        record["started_rows"] = rows(window)
        assert sum("进程运行中" in row for row in rows(window)) == 15, "Original peer exited: " + repr(rows(window))
        checks["trimmed_peer_protocols"] = {"channel": channel(7001), "relay": relay(True), "stun": stun()}
        # Use the real game with the retained DP2/Lua/Frida resources.
        select_service(window, "game"); click(window, 221); ready(window, "game：进程运行中", 30)
        log_dir = guest_root(first) / "home/neople/game/log"
        start = time.monotonic()
        wait_for(lambda: any(b"server has been started successfully" in p.read_bytes() for p in log_dir.rglob("*.init")), "trimmed game natural startup", 180)
        checks["trimmed_game_natural_startup_seconds"] = round(time.monotonic() - start, 3)
        screenshot(window, probe / "running.png")
        click(window, 224); ready(window, "服务进程已退出，数据库已停止", 240)
        stdout = "\n".join(p.read_text(encoding="utf-8", errors="replace") for p in (first / "service-logs").glob("game-*.stdout.private.log"))
        logs = stdout + "\n".join(p.read_text(encoding="utf-8", errors="replace") for p in log_dir.rglob("*.log"))
        assert all(s in logs for s in ["ALL USER LOGOUT COMPLETE", "GAME SERVER SHUTDOWN COMPLETE", "dispose", "unload"])
        checks["game_dp2_frida_startup_and_normal_shutdown"] = True
    except Exception as error:
        record["error"] = type(error).__name__ + ": " + str(error)
        record["traceback"] = traceback.format_exc()
        if window: screenshot(window, probe / "failure.png")
    finally:
        if damaged_resource is not None:
            with damaged_resource.open("r+b") as stream:
                stream.seek(-1, 2); stream.write(saved_byte)
        if renamed is not None: renamed.rename(resources / renamed.name.removesuffix(".test-missing"))
        if second_process is not None and second_process.poll() is None:
            for other in windows(second_process.pid, "#32770"): click(other, 1)
            for other in windows(second_process.pid, "DNFNativeController"): user.PostMessageW(other, 0x10, 0, 0)
            second_process.wait(timeout=20)
        if process.poll() is None:
            if window: user.PostMessageW(window, 0x10, 0, 0)
            try: process.wait(timeout=240)
            except subprocess.TimeoutExpired:
                record["forced_cleanup"] = True
                subprocess.run(["taskkill.exe", "/PID", str(process.pid), "/T", "/F"], capture_output=True, timeout=15)
                process.wait(timeout=10)
        if record.get("first_instance"):
            diagnostics = runtime_diagnostics(Path(record["first_instance"]))
            checks["runtime_faults"] = diagnostics["faults"]
            checks["runtime_gaps"] = diagnostics["gaps"]
            if diagnostics["faults"] and not record.get("error"):
                record["error"] = "Original service exception: " + diagnostics["faults"][0]
        record["exit_code"] = process.returncode
        (probe / "receipt.json").write_text(json.dumps(record, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")
        print("Instance workflow receipt:", probe / "receipt.json", flush=True)
    if record.get("error") or record["forced_cleanup"]:
        raise SystemExit(json.dumps(record.get("error", "forced cleanup"), ensure_ascii=True))


if __name__ == "__main__":
    main()
