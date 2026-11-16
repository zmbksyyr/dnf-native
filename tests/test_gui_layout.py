"""Check the real service grid, window bounds, DPI layout and log filtering."""
import argparse
import ctypes as c
from ctypes import wintypes as w
import datetime
import json
from pathlib import Path
import platform
import subprocess
import sys
import traceback

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from bootstrap import BUILD, sha256
from gui_support import user, windows, wait_for, title, rows, screenshot, click
from runtime_support import tree_identity

user.GetClientRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
user.GetWindowRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
user.MapWindowPoints.argtypes = [w.HWND, w.HWND, c.c_void_p, w.UINT]
user.MoveWindow.argtypes = [w.HWND, c.c_int, c.c_int, c.c_int, c.c_int, w.BOOL]
user.IsWindowVisible.argtypes = [w.HWND]


def rect(window, parent=None):
    value = w.RECT()
    user.GetWindowRect(window, c.byref(value))
    if parent:
        user.MapWindowPoints(None, parent, c.byref(value), 2)
    return value


def set_text(window, text):
    buffer = c.create_unicode_buffer(text)
    user.SendMessageW(window, 0x000C, 0, c.addressof(buffer))
    assert title(window) == text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gui", type=Path, required=True)
    parser.add_argument("--instance", type=Path, required=True,
                        help="Stopped private fixture; this test only reads its logs and metadata")
    args = parser.parse_args()
    gui, source = args.gui.resolve(strict=True), args.instance.resolve(strict=True)
    before = tree_identity(source / "data")
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    probe = BUILD / "probes" / stamp / "gui-layout"
    probe.mkdir(parents=True)
    record = {"os": platform.platform(), "gui_sha256": sha256(gui), "checks": {},
              "scope": "Real Win32 layout, mouse selection and filtering. DPI messages exercise layout only, not a physical monitor or cross-OS acceptance."}
    checks = record["checks"]
    process = subprocess.Popen([str(gui), "--instance", str(source)])
    window = None
    try:
        window = wait_for(lambda: next(iter(windows(process.pid, "DNFNativeController")), None), "controller")
        wait_for(lambda: len(rows(window)) == 19, "19 service cards")
        grid = user.GetDlgItem(window, 220)
        scales = [(96, 1440, 940), (96, 1180, 760), (120, 1475, 950)]
        for dpi, width, height in scales:
            suggested = w.RECT(12, 12, 12 + width, 12 + height)
            user.SendMessageW(window, 0x02E0, dpi | dpi << 16, c.addressof(suggested))
            user.MoveWindow(window, 12, 12, width, height, True)
            client = w.RECT()
            user.GetClientRect(window, c.byref(client))
            visible = {}
            for identifier in range(100, 290):
                child = user.GetDlgItem(window, identifier)
                if not child or not user.IsWindowVisible(child):
                    continue
                area = rect(child, window)
                assert 0 <= area.left < area.right <= client.right, (identifier, "horizontal overflow")
                assert 0 <= area.top < area.bottom <= client.bottom, (identifier, "vertical overflow")
                visible[identifier] = [area.left, area.top, area.right, area.bottom]
            tiles = []
            grid_client = w.RECT()
            user.GetClientRect(grid, c.byref(grid_client))
            for index in range(19):
                area = w.RECT()
                user.SendMessageW(grid, 0x0198, index, c.addressof(area))  # LB_GETITEMRECT
                assert 0 <= area.left < area.right <= grid_client.right
                assert 0 <= area.top < area.bottom <= grid_client.bottom
                x, y = (area.left + area.right) // 2, (area.top + area.bottom) // 2
                point = (x & 65535) | (y & 65535) << 16
                assert user.SendMessageW(grid, 0x01A9, 0, point) == index  # LB_ITEMFROMPOINT
                user.PostMessageW(grid, 0x0201, 1, point)
                user.PostMessageW(grid, 0x0202, 0, point)
                wait_for(lambda: user.SendMessageW(grid, 0x188, 0, 0) == index, "mouse selects card", 5)
                selected = rows(window)[index].split("  |  ", 1)[0].removesuffix("（可选）")
                wait_for(lambda: title(user.GetDlgItem(window, 219)).startswith(selected + "  "), "details match card", 5)
                tiles.append([area.left, area.top, area.right, area.bottom])
            assert tiles[0][1] == tiles[6][1] < tiles[7][1] == tiles[13][1] < tiles[14][1] == tiles[18][1]
            for identifier in [102, 223, 224, 220, 202, 111, 113, 114, 206, 283, 284, 285, 286, 287]:
                assert identifier in visible
            assert visible[102][3] < visible[220][1] < visible[202][1] < visible[206][1]
            for identifiers in [[102, 260, 101, 250, 251, 262, 116, 280, 224, 223],
                                [235, 233, 234, 230, 231, 232, 281, 221, 222, 225],
                                [112, 111, 113, 114, 115, 282],
                                [284, 285, 286, 287, 288, 283, 241, 243, 242]]:
                for a, b in zip(identifiers, identifiers[1:]):
                    assert visible[a][2] <= visible[b][0], (dpi, a, b, "overlapping controls")
            screenshot(window, probe / (str(dpi) + "dpi-" + str(width) + ".png"))
        checks["all_cards_and_controls_fit_at_default_minimum_and_125_percent_layout"] = True
        checks["all_19_mouse_hit_rectangles_match_visual_and_detail_selection"] = True
        click(window, 262)
        click(window, 284)
        wait_for(lambda: title(user.GetDlgItem(window, 210)) == "控制台操作记录" and
                 "MySQL" in title(user.GetDlgItem(window, 206)), "console tab displays completed resource check", 5)
        full = title(user.GetDlgItem(window, 206))
        assert full
        edit = user.GetDlgItem(window, 283)
        set_text(edit, "mysql")
        wait_for(lambda: title(user.GetDlgItem(window, 206)) == full, "case-insensitive MySQL match", 5)
        needle = "GUI-NO-SUCH-LOG-LINE-" + stamp
        set_text(edit, needle)
        wait_for(lambda: not title(user.GetDlgItem(window, 206)), "filter removes nonmatching lines", 5)
        set_text(edit, "")
        wait_for(lambda: title(user.GetDlgItem(window, 206)) == full, "clear filter restores raw text", 5)
        set_text(edit, "gui-no-such-log-line-" + stamp)
        assert not title(user.GetDlgItem(window, 206))
        set_text(edit, "")
        checks["log_filter_and_clear_use_actual_console_text"] = True
    except Exception as error:
        record["error"] = type(error).__name__ + ": " + str(error)
        record["traceback"] = traceback.format_exc()
    finally:
        if window:
            user.PostMessageW(window, 0x10, 0, 0)
        process.wait(timeout=30)
        checks["source_database_unchanged"] = tree_identity(source / "data") == before
        record["exit_code"] = process.returncode
        record["passed"] = not record.get("error") and process.returncode == 0 and all(checks.values())
        (probe / "receipt.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
        print("GUI layout:", "PASS" if record["passed"] else "FAIL", probe / "receipt.json", flush=True)
    if not record["passed"]:
        raise SystemExit(record.get("error", "GUI layout failed"))


if __name__ == "__main__":
    main()
