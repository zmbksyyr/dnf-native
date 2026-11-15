"""Exercise queued keyboard input through the real Win32 GUI and modal dialogs."""
import argparse
import ctypes as c
from ctypes import wintypes as w
import datetime
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from bootstrap import BUILD, sha256
from gui_support import user, kernel, windows, wait_for, title, ready, rows, list_rows, screenshot, select_folder
from runtime_support import tree_identity


class GuiInfo(c.Structure):
    _fields_ = [("cbSize", w.DWORD), ("flags", w.DWORD), ("active", w.HWND),
                ("focus", w.HWND), ("capture", w.HWND), ("menu", w.HWND),
                ("move", w.HWND), ("caret", w.HWND), ("rect", w.RECT)]


user.GetGUIThreadInfo.argtypes = [w.DWORD, c.POINTER(GuiInfo)]
user.AttachThreadInput.argtypes = [w.DWORD, w.DWORD, w.BOOL]
user.SetFocus.argtypes, user.SetFocus.restype = [w.HWND], w.HWND
user.GetNextDlgTabItem.argtypes, user.GetNextDlgTabItem.restype = [w.HWND, w.HWND, w.BOOL], w.HWND
user.GetDlgCtrlID.argtypes = [w.HWND]
user.GetParent.argtypes, user.GetParent.restype = [w.HWND], w.HWND


def focused(window):
    info = GuiInfo(cbSize=c.sizeof(GuiInfo))
    if not user.GetGUIThreadInfo(user.GetWindowThreadProcessId(window, None), c.byref(info)):
        raise c.WinError(c.get_last_error())
    return info.focus


def focus(window, target):
    if not user.IsWindowVisible(target) or not user.IsWindowEnabled(target):
        raise RuntimeError("Keyboard target is hidden or disabled")
    own, other = kernel.GetCurrentThreadId(), user.GetWindowThreadProcessId(window, None)
    if not user.AttachThreadInput(own, other, True):
        raise c.WinError(c.get_last_error())
    try:
        user.SetFocus(target)
    finally:
        user.AttachThreadInput(own, other, False)
    assert focused(window) == target


def key(window, value):
    target = focused(window)
    assert target
    # Queue keydown/up to the verified focus. The product's actual message
    # loop, IsDialogMessage and native controls perform navigation/activation.
    # This does not synthesize BN_CLICKED or change another process's input.
    user.PostMessageW(target, 0x100, value, 1)
    user.PostMessageW(target, 0x101, value, 0xC0000001)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gui", type=Path, required=True)
    parser.add_argument("--instance", type=Path, required=True, help="Stopped private instance to copy")
    args = parser.parse_args()
    gui, source = args.gui.resolve(strict=True), args.instance.resolve(strict=True)
    before = tree_identity(source / "data")
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    probe = BUILD / "probes" / stamp / "gui-keyboard"
    instance = probe / "instance"
    shutil.copytree(source, instance, ignore=shutil.ignore_patterns(".mysql", "service-logs"))
    record = {"recorded_utc": stamp, "os": platform.platform(), "gui_sha256": sha256(gui),
              "script_sha256": sha256(Path(__file__)), "checks": {}, "forced_cleanup": False,
              "scope": "Real Win32 queued keyboard input, focus traversal, modal cancellation and database lifecycle; no physical keyboard, client business or cross-OS acceptance."}
    checks = record["checks"]
    process = subprocess.Popen([str(gui), "--instance", str(instance)])
    window = None
    try:
        window = wait_for(lambda: next(iter(windows(process.pid, "DNFNativeController")), None), "controller")
        user.ShowWindow(window, 4)
        assert len(rows(window)) == 19
        button_types = {identifier: user.GetWindowLongW(user.GetDlgItem(window, identifier), -16) & 15
                        for identifier in range(100, 275) if user.GetDlgItem(window, identifier) and
                        user.SendMessageW(user.GetDlgItem(window, identifier), 0x87, 0, 0) & 0x2000}
        path = user.GetDlgItem(window, 101)
        focus(window, path)
        visited = []
        checks["tab_traverses_enabled_visible_controls"] = visited
        while True:
            current = focused(window)
            expected = user.GetNextDlgTabItem(window, current, False)
            visited.append(user.GetDlgCtrlID(current))
            key(window, 9)
            wait_for(lambda: focused(window) == expected,
                     "Tab from " + str(user.GetDlgCtrlID(current)) + " to " + str(user.GetDlgCtrlID(expected)), 5)
            if expected == path:
                break
            assert len(visited) < 60, visited
        assert len(set(visited)) == len(visited) and {102, 111, 113, 114, 220, 242, 260, 261, 270, 271}.issubset(visited)

        focus(window, user.GetDlgItem(window, 102))
        key(window, 13)
        picker = wait_for(lambda: next(iter(windows(process.pid, "#32770")), None), "Enter opens folder picker", 5)
        wait_for(lambda: user.IsWindowVisible(picker), "folder picker shown", 5)
        key(picker, 27)
        wait_for(lambda: not user.IsWindow(picker), "Escape cancels folder picker", 5)
        assert title(path) == str(instance)
        checks["enter_activates_focused_button_and_escape_cancels_picker"] = True

        box = user.GetDlgItem(window, 220)
        user.SendMessageW(box, 0x186, 0, 0)  # First group header.
        focus(window, box)
        key(window, 40)
        wait_for(lambda: user.SendMessageW(box, 0x188, 0, 0) == 1, "Down selects a service", 5)
        selected_name = list_rows(window)[1].split("  |  ", 1)[0]
        wait_for(lambda: title(user.GetDlgItem(window, 219)).startswith(selected_name + "  "), "service details follow keyboard selection", 5)
        key(window, 38)
        wait_for(lambda: user.SendMessageW(box, 0x188, 0, 0) == 0, "Up selects group header", 5)
        focus(window, user.GetDlgItem(window, 271))
        key(window, 13)
        wait_for(lambda: len(rows(window)) == 11, "Enter collapses selected group", 5)
        key(window, 13)
        wait_for(lambda: len(rows(window)) == 19, "Enter expands selected group", 5)
        checks["arrows_update_service_details_and_enter_toggles_group"] = True

        combo = user.GetDlgItem(window, 270)
        focus(window, combo)
        key(window, 40)
        wait_for(lambda: len(rows(window)) == 0, "Down filters running services", 5)
        key(window, 38)
        wait_for(lambda: len(rows(window)) == 19, "Up restores all services", 5)
        key(window, 0x73)  # F4 opens the native drop-down.
        wait_for(lambda: user.SendMessageW(combo, 0x157, 0, 0), "filter drop-down opens", 5)
        key(window, 27)
        wait_for(lambda: not user.SendMessageW(combo, 0x157, 0, 0), "Escape closes filter drop-down", 5)
        checks["native_filter_arrows_and_escape"] = True

        follow = user.GetDlgItem(window, 242)
        previous = user.SendMessageW(follow, 0xF0, 0, 0)
        focus(window, follow)
        key(window, 32)
        wait_for(lambda: user.SendMessageW(follow, 0xF0, 0, 0) != previous, "Space toggles checkbox", 5)
        key(window, 32)
        wait_for(lambda: user.SendMessageW(follow, 0xF0, 0, 0) == previous, "Space restores checkbox", 5)
        checks["space_toggles_native_checkbox"] = True

        cold_data = tree_identity(instance / "data")
        cancelled_restore = probe / "cancelled-restore"
        cancelled_restore.mkdir()
        for identifier, caption in [(261, "\u5220\u9664\u5b9e\u4f8b"), (114, "\u786e\u8ba4\u8fd8\u539f")]:
            for cancel_key in [13, 27]:
                focus(window, user.GetDlgItem(window, identifier))
                key(window, 13)
                if identifier == 114:
                    select_folder(process, cancelled_restore)
                dialog = wait_for(lambda: next((h for h in windows(process.pid, "#32770")
                                               if title(h) == caption), None), "maintenance confirmation", 5)
                wait_for(lambda: user.IsWindowVisible(dialog) and focused(dialog) == user.GetDlgItem(dialog, 2),
                         "visible destructive confirmation defaults to Cancel", 5)
                key(dialog, cancel_key)
                wait_for(lambda: not user.IsWindow(dialog), "cancel confirmation with key " + str(cancel_key), 5)
                assert (instance / "data/auto.cnf").is_file() and title(path) == str(instance)
                assert tree_identity(instance / "data") == cold_data
                assert not (instance / "restore.pending").exists()
        checks["delete_restore_default_to_cancel_and_enter_escape_preserve_cold_data"] = True

        focus(window, user.GetDlgItem(window, 111))
        key(window, 13)
        ready(window, "\u6570\u636e\u5e93 SQL \u68c0\u67e5\u901a\u8fc7", 90)
        focus(window, user.GetDlgItem(window, 112))
        key(window, 13)
        ready(window, "\u6570\u636e\u5e93\u5df2\u6b63\u5e38\u505c\u6b62", 90)
        checks["enter_runs_database_identity_tls_start_and_normal_stop"] = True
        assert all(user.GetWindowLongW(user.GetDlgItem(window, identifier), -16) & 15 == expected
                   for identifier, expected in button_types.items()), "Keyboard navigation changed button rendering types"
        checks["keyboard_preserves_owner_draw_and_checkbox_rendering_types"] = True
        screenshot(window, probe / "keyboard.png")
    except Exception as error:
        record["error"] = type(error).__name__ + ": " + str(error)
    finally:
        for dialog in windows(process.pid, "#32770"):
            user.PostMessageW(dialog, 0x10, 0, 0)
        if window and process.poll() is None:
            user.PostMessageW(window, 0x10, 0, 0)
        try:
            process.wait(timeout=120)
        except subprocess.TimeoutExpired:
            record["forced_cleanup"] = True
            subprocess.run(["taskkill.exe", "/PID", str(process.pid), "/T", "/F"], capture_output=True)
            process.wait(timeout=15)
        checks["source_data_unchanged"] = tree_identity(source / "data") == before
        record["gui_exit_code"] = process.returncode
        record["passed"] = (not record.get("error") and not record["forced_cleanup"] and
                            checks["source_data_unchanged"] and process.returncode == 0)
        (probe / "receipt.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf8")
        print("GUI keyboard:", "PASS" if record["passed"] else "FAIL", probe / "receipt.json", flush=True)
    if not record["passed"]:
        raise SystemExit(record.get("error", "GUI keyboard failure"))


if __name__ == "__main__":
    main()
