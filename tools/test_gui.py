"""Drive the real Win32 controls against fresh private MySQL instances."""

import argparse
import ctypes as c
from ctypes import wintypes as w
import datetime
import json
import os
from pathlib import Path
import platform
import secrets
import shutil
import socket
import struct
import subprocess
import time
import zlib

from bootstrap import sha256
from probe_mysql import tree_identity

ROOT = Path(__file__).resolve().parents[1]
user = c.WinDLL("user32", use_last_error=True)
kernel = c.WinDLL("kernel32", use_last_error=True)
user.GetDlgItem.argtypes, user.GetDlgItem.restype = [w.HWND, c.c_int], w.HWND
user.SendMessageW.argtypes, user.SendMessageW.restype = [w.HWND, w.UINT, c.c_size_t, c.c_ssize_t], c.c_ssize_t
user.PostMessageW.argtypes = [w.HWND, w.UINT, c.c_size_t, c.c_ssize_t]
user.GetWindowTextLengthW.argtypes = [w.HWND]
user.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
user.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
user.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
user.IsWindowEnabled.argtypes = [w.HWND]
user.GetWindowLongW.argtypes = [w.HWND,c.c_int]
user.GetWindowLongW.restype = w.LONG
user.IsWindow.argtypes = [w.HWND]
user.SendMessageTimeoutW.argtypes = [w.HWND, w.UINT, c.c_size_t, c.c_ssize_t, w.UINT, w.UINT, c.POINTER(c.c_size_t)]
callback_type = c.WINFUNCTYPE(w.BOOL, w.HWND, c.c_ssize_t)
user.EnumWindows.argtypes = [callback_type, c.c_ssize_t]


def title(window):
    # GetWindowText deliberately omits another process's EDIT contents. These
    # system messages marshal the Unicode buffer for the real child control.
    text = c.create_unicode_buffer(user.SendMessageW(window, 0x000E, 0, 0) + 1)
    user.SendMessageW(window, 0x000D, len(text), c.addressof(text))
    return text.value


def windows(pid, class_name=None):
    found = []

    @callback_type
    def visit(window, unused):
        owner = w.DWORD()
        user.GetWindowThreadProcessId(window, c.byref(owner))
        name = c.create_unicode_buffer(256)
        user.GetClassNameW(window, name, len(name))
        if owner.value == pid and (class_name is None or name.value == class_name):
            found.append(window)
        return True

    user.EnumWindows(visit, 0)
    return found


def wait_for(check, description, timeout=120):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = check()
        if result:
            return result
        time.sleep(0.05)
    raise RuntimeError("GUI timeout: " + description)


def click(window, identifier):
    button = user.GetDlgItem(window, identifier)
    if not button:
        raise RuntimeError("Requested GUI button is disabled: " + str(identifier))
    wait_for(lambda: user.IsWindowEnabled(button), "button enabled: " + str(identifier), 5)
    if user.IsWindowVisible(button):
        user.PostMessageW(button, 0x00F5, 0, 0)  # BM_CLICK through the actual control.
    else:
        # BM_CLICK hit-testing ignores controls in a hidden test window.
        # Deliver its normal BN_CLICKED notification after checking enablement.
        if user.GetWindowLongW(button,-16) & 15 == 3:  # BS_AUTOCHECKBOX
            user.SendMessageW(button,0xF1,0 if user.SendMessageW(button,0xF0,0,0) else 1,0)
        user.PostMessageW(window, 0x111, identifier, button)


def status(window):
    return title(user.GetDlgItem(window, 204))


def ready(window, expected, timeout=120):
    def check():
        text = status(window)
        if text.startswith("操作失败"):
            raise RuntimeError(text)
        return expected in text
    wait_for(check, expected, timeout)


def responsive(window):
    result = c.c_size_t()
    started = time.monotonic()
    if not user.SendMessageTimeoutW(window, 0, 0, 0, 2, 1000, c.byref(result)):
        raise RuntimeError("Win32 window did not respond while an operation was running")
    return round((time.monotonic() - started) * 1000, 3)


def select_folder(process, target):
    dialog = wait_for(lambda: next((item for item in windows(process.pid, "#32770") if title(item) != "确认还原"), None), "folder chooser")
    # BFFM_SETSELECTION is a WM_USER message: Windows does not marshal its
    # string across processes. Allocate only the path in this own test GUI.
    text = c.create_unicode_buffer(str(target))
    kernel.OpenProcess.argtypes, kernel.OpenProcess.restype = [w.DWORD, w.BOOL, w.DWORD], w.HANDLE
    kernel.VirtualAllocEx.argtypes, kernel.VirtualAllocEx.restype = [w.HANDLE, c.c_void_p, c.c_size_t, w.DWORD, w.DWORD], c.c_void_p
    kernel.WriteProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)]
    kernel.VirtualFreeEx.argtypes = [w.HANDLE, c.c_void_p, c.c_size_t, w.DWORD]
    kernel.CloseHandle.argtypes = [w.HANDLE]
    handle = kernel.OpenProcess(0x28, False, process.pid)
    if not handle:
        raise c.WinError(c.get_last_error())
    address = kernel.VirtualAllocEx(handle, None, c.sizeof(text), 0x3000, 4)
    try:
        written = c.c_size_t()
        if not address or not kernel.WriteProcessMemory(handle, address, text, c.sizeof(text), c.byref(written)):
            raise c.WinError(c.get_last_error())
        user.ShowWindow(dialog,0)
        user.SendMessageW(dialog, 0x400 + 103, 1, address)
    finally:
        if address:
            kernel.VirtualFreeEx(handle, address, 0, 0x8000)
        kernel.CloseHandle(handle)
    wait_for(lambda: user.IsWindowEnabled(user.GetDlgItem(dialog, 1)), "selected folder")
    click(dialog, 1)
    wait_for(lambda: not user.IsWindow(dialog), "folder chooser close")


def screenshot(window, path):
    # Capture only our window; encode RGB PNG with the Python standard library.
    gdi = c.WinDLL("gdi32", use_last_error=True)
    user.GetWindowRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
    user.GetDC.argtypes, user.GetDC.restype = [w.HWND], w.HDC
    user.ReleaseDC.argtypes = [w.HWND, w.HDC]
    user.PrintWindow.argtypes = [w.HWND, w.HDC, w.UINT]
    gdi.CreateCompatibleDC.argtypes, gdi.CreateCompatibleDC.restype = [w.HDC], w.HDC
    gdi.CreateCompatibleBitmap.argtypes, gdi.CreateCompatibleBitmap.restype = [w.HDC, c.c_int, c.c_int], w.HBITMAP
    gdi.SelectObject.argtypes, gdi.SelectObject.restype = [w.HDC, w.HANDLE], w.HANDLE
    gdi.GetDIBits.argtypes = [w.HDC, w.HBITMAP, w.UINT, w.UINT, c.c_void_p, c.c_void_p, w.UINT]
    gdi.DeleteObject.argtypes = [w.HANDLE]
    gdi.DeleteDC.argtypes = [w.HDC]
    hidden = not user.IsWindowVisible(window)
    if hidden:
        user.ShowWindow(window,4)  # Brief capture without activation.
        user.UpdateWindow(window)
    rect = w.RECT()
    user.GetWindowRect(window, c.byref(rect))
    width, height = rect.right - rect.left, rect.bottom - rect.top
    dc = user.GetDC(window)
    memory = gdi.CreateCompatibleDC(dc)
    bitmap = gdi.CreateCompatibleBitmap(dc, width, height)
    previous = gdi.SelectObject(memory, bitmap)
    try:
        if not user.PrintWindow(window, memory, 0):
            raise c.WinError(c.get_last_error())
        header = c.create_string_buffer(struct.pack("<IiiHHIIiiII", 40, width, -height, 1, 32, 0, width * height * 4, 0, 0, 0, 0))
        pixels = c.create_string_buffer(width * height * 4)
        gdi.SelectObject(memory, previous)
        if gdi.GetDIBits(dc, bitmap, 0, height, pixels, header, 0) != height:
            raise c.WinError(c.get_last_error())
        bgra = pixels.raw
        rows = bytearray()
        for y in range(height):
            row = bgra[y * width * 4:(y + 1) * width * 4]
            rgb = bytearray(width * 3)
            rgb[0::3], rgb[1::3], rgb[2::3] = row[2::4], row[1::4], row[0::4]
            rows += b"\0" + rgb
        def chunk(name, data):
            return struct.pack(">I", len(data)) + name + data + struct.pack(">I", zlib.crc32(name + data))
        path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                         chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
    finally:
        gdi.SelectObject(memory, previous)
        gdi.DeleteObject(bitmap)
        gdi.DeleteDC(memory)
        user.ReleaseDC(window, dc)
        if hidden:
            user.ShowWindow(window,0)


def hidden_startup():
    startup = subprocess.STARTUPINFO()
    startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    return startup


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=ROOT / "build/gui/DNF-Native.exe")
    args = parser.parse_args()
    exe = args.exe.resolve(strict=True)
    mysql = exe.parent / "runtime/mysql-control.exe"
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    probe = ROOT / "build/probes" / stamp / "gui"
    instance = probe / "实例 (native)"
    backups = probe / "backups"
    backups.mkdir(parents=True)
    env = dict(os.environ)
    env["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32")
    for name in ["MYSQL_HOME", "MYSQL_PWD", "MYSQL_TCP_PORT"]:
        env.pop(name, None)
    receipt = {"recorded_utc": stamp, "os": platform.platform(), "probe": str(probe.relative_to(ROOT)),
               "scope": "Real Win32 database controls, asynchronous operations, native backup/restore and owned server lifecycle; no original service GUI or gameplay acceptance.",
               "gui_sha256": sha256(exe), "mysql_sha256": sha256(mysql), "script_sha256": sha256(Path(__file__)),
               "checks": {}, "forced_cleanup": False}
    checks = receipt["checks"]
    processes = []
    index = 0

    def invoke(operation, arguments=(), data=None, target=instance):
        nonlocal index
        result = subprocess.run([str(mysql), operation, str(target), *map(str, arguments)], input=data,
                                capture_output=True, env=env, timeout=180)
        index += 1
        (probe / f"cli-{index}.private.log").write_bytes(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(f"Fixture native command {index} failed; see private log")
        return result.stdout

    def query(text):
        return invoke("mysql", ["--defaults-file=client.private.cnf", "--batch", "--skip-column-names"], text.encode("ascii"))

    try:
        process = subprocess.Popen([str(exe), "--instance", str(instance)], env=env, startupinfo=hidden_startup())
        processes.append(process)
        window = wait_for(lambda: next(iter(windows(process.pid, "DNFNativeController")), None), "main window")
        empty = c.create_unicode_buffer("")
        user.SendMessageW(user.GetDlgItem(window, 101), 0x000C, 0, c.addressof(empty))
        ready(window, "请选择实例目录")
        if user.IsWindowEnabled(user.GetDlgItem(window, 110)) or user.IsWindowEnabled(user.GetDlgItem(window, 111)):
            raise RuntimeError("Empty instance selection enabled a database operation")
        chosen = c.create_unicode_buffer(str(instance))
        user.SendMessageW(user.GetDlgItem(window, 101), 0x000C, 0, c.addressof(chosen))
        ready(window, "尚未初始化")
        checks["instance_path_can_be_cleared_and_selected_again"] = True
        with socket.socket() as reserved:
            reserved.bind(("127.0.0.1", 0))
            port = reserved.getsockname()[1]
        text = c.create_unicode_buffer(str(port))
        user.SendMessageW(user.GetDlgItem(window, 103), 0x000C, 0, c.addressof(text))
        click(window, 110)
        wait_for(lambda: "正在" in status(window), "initialization starts")
        checks["initialize_window_response_ms"] = responsive(window)
        ready(window, "安全初始化完成", 180)
        checks["gui_secure_initialization"] = True
        click(window, 111)
        ready(window, "数据库 SQL 检查通过")
        checks["gui_start_checks_sql_identity_and_tls"] = True
        query("CREATE DATABASE gui_probe; CREATE TABLE gui_probe.rows_test(id INT PRIMARY KEY,body VARBINARY(20));"
              "INSERT INTO gui_probe.rows_test VALUES(1,0x000D0AFF),(2,0xE4B8ADE69687);")
        expected = query("SELECT id,HEX(body) FROM gui_probe.rows_test ORDER BY id;")
        screenshot(window, probe / "database-running.png")
        receipt["running_screenshot_sha256"] = sha256(probe / "database-running.png")

        # A second GUI targets a different UUID on this same occupied port.
        # Its failed startup must leave the first owned database usable.
        other = probe / "other-instance"
        invoke("initialize", [port], target=other)
        shutil.copy2(instance / "client.private.cnf", other / "client.private.cnf")
        conflict = subprocess.Popen([str(exe), "--instance", str(other)], env=env, startupinfo=hidden_startup())
        processes.append(conflict)
        other_window = wait_for(lambda: next(iter(windows(conflict.pid, "DNFNativeController")), None), "second window")
        click(other_window, 111)
        wait_for(lambda: status(other_window).startswith("操作失败"), "occupied-port startup fails")
        if query("SELECT id,HEX(body) FROM gui_probe.rows_test ORDER BY id;") != expected:
            raise RuntimeError("Failed foreign startup affected the running database")
        user.PostMessageW(other_window, 0x0010, 0, 0)
        conflict.wait(timeout=20)
        checks["different_uuid_on_same_port_does_not_stop_first_instance"] = True

        click(window, 112)
        ready(window, "数据库已正常停止")
        checks["gui_normal_shutdown"] = True
        click(window, 113)
        select_folder(process, backups)
        wait_for(lambda: "正在备份" in status(window), "backup starts")
        checks["backup_window_response_ms"] = responsive(window)
        user.PostMessageW(window, 0x0010, 0, 0)
        ready(window, "备份完成")
        if not user.IsWindow(window):
            raise RuntimeError("Closing a busy window interrupted its backup")
        checks["busy_close_waits_without_canceling_backup"] = True
        backup = wait_for(lambda: next(iter(backups.glob("backup-*/manifest.txt")), None), "backup files").parent
        receipt["backup_sql_sha256"] = sha256(backup / "logical.sql")
        click(window, 111)
        ready(window, "数据库 SQL 检查通过")
        query("UPDATE gui_probe.rows_test SET body=0xAB; CREATE DATABASE gui_added_after_backup;")
        click(window, 112)
        ready(window, "数据库已正常停止")
        corrupt = probe / "corrupt-backup"
        shutil.copytree(backup, corrupt)
        with (corrupt / "logical.sql").open("ab") as stream:
            stream.write(b"\nINVALID BACKUP;\n")
        before_rejection = tree_identity(instance / "data")
        click(window, 114)
        select_folder(process, corrupt)
        confirmation = wait_for(lambda: next((item for item in windows(process.pid, "#32770") if title(item) == "确认还原"), None), "corrupt restore confirmation")
        click(confirmation, 6)
        wait_for(lambda: status(window).startswith("操作失败"), "GUI reports corrupt backup failure")
        if ("Backup hash" not in title(user.GetDlgItem(window, 206)) or
                tree_identity(instance / "data") != before_rejection):
            raise RuntimeError("GUI did not retain the failed restore diagnostic and unchanged active data")
        checks["gui_corrupt_restore_reports_error_without_data_writes"] = True
        click(window, 114)
        select_folder(process, backup)
        confirmation = wait_for(lambda: next((item for item in windows(process.pid, "#32770") if title(item) == "确认还原"), None), "restore confirmation")
        click(confirmation, 6)  # IDYES, only this private test instance.
        wait_for(lambda: "正在预备份" in status(window), "restore starts")
        checks["restore_window_response_ms"] = responsive(window)
        ready(window, "还原已验证并提交", 300)
        checks["gui_backup_and_restore_native_results"] = True
        click(window, 111)
        ready(window, "数据库 SQL 检查通过")
        if query("SELECT id,HEX(body) FROM gui_probe.rows_test ORDER BY id;") != expected:
            raise RuntimeError("GUI restore did not recover the binary row bytes")
        if query("SELECT COUNT(*) FROM information_schema.SCHEMATA WHERE SCHEMA_NAME='gui_added_after_backup';").strip() != b"0":
            raise RuntimeError("GUI restore retained a database created after the backup")
        checks["restored_rows_and_removed_extra_schema"] = True
        if not title(user.GetDlgItem(window, 206)).strip():
            raise RuntimeError("GUI operation log is empty")
        screenshot(window, probe / "after-restore.png")
        receipt["restored_screenshot_sha256"] = sha256(probe / "after-restore.png")
        user.PostMessageW(window, 0x0010, 0, 0)
        process.wait(timeout=90)
        if process.returncode:
            raise RuntimeError("GUI failed while closing its running database")
        logs = list((instance / ".mysql").glob("gui-*-server.private.log"))
        if not logs or any(b"Shutdown complete" not in log.read_bytes() for log in logs):
            raise RuntimeError("GUI did not normally stop every owned test server")
        checks["close_window_normally_stops_owned_database"] = True
        checks["gui_process_exit_code"] = process.returncode

        # Present an actual cold rename-window state, reopen the GUI, and use
        # its recovery button. The controller must not offer a regular start.
        before_recovery = tree_identity(instance / "data")
        leaf = "restore-" + secrets.token_hex(24)
        operation = instance / ".mysql" / leaf
        operation.mkdir()
        (instance / "data").rename(operation / "previous-data")
        (instance / "restore.pending").write_text(leaf, encoding="ascii")
        reopened = subprocess.Popen([str(exe), "--instance", str(instance)], env=env, startupinfo=hidden_startup())
        processes.append(reopened)
        reopened_window = wait_for(lambda: next(iter(windows(reopened.pid, "DNFNativeController")), None), "recovery window")
        ready(reopened_window, "需要恢复未完成的还原")
        if user.IsWindowEnabled(user.GetDlgItem(reopened_window, 111)):
            raise RuntimeError("GUI allowed a normal start with an interrupted restore")
        click(reopened_window, 115)
        ready(reopened_window, "恢复完成")
        if ((instance / "restore.pending").exists() or tree_identity(instance / "data") != before_recovery):
            raise RuntimeError("GUI recovery did not restore the previous exact cold data")
        checks["gui_pending_restore_blocks_start_and_recovery_restores_cold_data"] = True
        user.PostMessageW(reopened_window, 0x0010, 0, 0)
        reopened.wait(timeout=20)
    except Exception as error:
        receipt["error"] = str(error)
    finally:
        for process in processes:
            if process.poll() is None:
                for window in windows(process.pid, "DNFNativeController"):
                    user.PostMessageW(window, 0x0010, 0, 0)
                try:
                    process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    receipt["forced_cleanup"] = True
                    subprocess.run(["taskkill.exe", "/PID", str(process.pid), "/T", "/F"], capture_output=True, timeout=10)
                    process.wait(timeout=10)
        receipt["limits"] = ["Developer Windows 10 10.0.19044 only, not clean Win7/10/11 or ordinary-user acceptance.",
                  "This probe exercises MySQL controls; original service orchestration is verified separately. This database probe does not exercise gameplay or service IP changes.",
                             "This isolated database probe starts maintenance with the database already stopped; running-service coordination is covered separately.",
                             "No final complete offline ZIP or gameplay acceptance."]
        (probe / "receipt.json").write_text(json.dumps(receipt, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(receipt, ensure_ascii=True))
    if receipt.get("error") or receipt["forced_cleanup"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
