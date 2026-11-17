"""Exercise repeated selection, resize rendering, switch focus and remembered instances."""
import argparse
import configparser
import ctypes as c
from ctypes import wintypes as w
import datetime
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import threading
import time
import traceback

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from bootstrap import BUILD, sha256
from gui_support import user, windows, wait_for, title, rows, screenshot, click, select_folder
from test_gui_keyboard import focus, key, menu_open

user.GetClientRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
user.GetDC.argtypes, user.GetDC.restype = [w.HWND], w.HDC
user.ReleaseDC.argtypes = [w.HWND, w.HDC]
user.ClientToScreen.argtypes = [w.HWND, c.POINTER(w.POINT)]
user.MoveWindow.argtypes = [w.HWND, c.c_int, c.c_int, c.c_int, c.c_int, w.BOOL]
user.GetSysColor.argtypes, user.GetSysColor.restype = [c.c_int], w.DWORD
user.GetCursorPos.argtypes = [c.POINTER(w.POINT)]
user.SetCursorPos.argtypes = [c.c_int, c.c_int]
user.SetForegroundWindow.argtypes = [w.HWND]
user.IsWindowVisible.argtypes = [w.HWND]
user.GetMenuItemCount.argtypes = [w.HMENU]
user.GetMenuItemID.argtypes = [w.HMENU, c.c_int]
user.GetMenuItemID.restype = w.UINT
user.GetMenuStringW.argtypes = [w.HMENU, w.UINT, w.LPWSTR, c.c_int, w.UINT]
gdi = c.WinDLL("gdi32", use_last_error=True)
gdi.CreateCompatibleDC.argtypes, gdi.CreateCompatibleDC.restype = [w.HDC], w.HDC
gdi.CreateDIBSection.argtypes, gdi.CreateDIBSection.restype = [w.HDC, c.c_void_p, w.UINT, c.POINTER(c.c_void_p), w.HANDLE, w.DWORD], w.HBITMAP
gdi.SelectObject.argtypes, gdi.SelectObject.restype = [w.HDC, w.HANDLE], w.HANDLE
gdi.BitBlt.argtypes = [w.HDC, c.c_int, c.c_int, c.c_int, c.c_int, w.HDC, c.c_int, c.c_int, w.DWORD]
gdi.DeleteObject.argtypes = [w.HANDLE]
gdi.DeleteDC.argtypes = [w.HDC]


class Capture:
    def __init__(self, window):
        import struct
        area = w.RECT()
        user.GetClientRect(window, c.byref(area))
        self.width, self.height = area.right, area.bottom
        self.window = window
        dc = user.GetDC(None)
        self.memory, self.bits = gdi.CreateCompatibleDC(dc), c.c_void_p()
        header = c.create_string_buffer(struct.pack("<IiiHHIIiiII", 40, self.width, -self.height, 1, 32, 0, 0, 0, 0, 0, 0))
        self.bitmap = gdi.CreateDIBSection(dc, header, 0, c.byref(self.bits), None, 0)
        user.ReleaseDC(None, dc)
        assert self.bitmap
        self.old = gdi.SelectObject(self.memory, self.bitmap)

    def pixels(self):
        # Observe the presented pixels. Holding a window DC across frames can
        # itself delay WS_EX_COMPOSITED until the DC is released.
        point = w.POINT()
        user.ClientToScreen(self.window, c.byref(point))
        dc = user.GetDC(None)
        try:
            assert gdi.BitBlt(self.memory, 0, 0, self.width, self.height, dc, point.x, point.y, 0x00CC0020)
        finally:
            user.ReleaseDC(None, dc)
        return c.string_at(self.bits, self.width * self.height * 4)

    def close(self):
        gdi.SelectObject(self.memory, self.old)
        gdi.DeleteObject(self.bitmap)
        gdi.DeleteDC(self.memory)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gui", type=Path, required=True)
    parser.add_argument("--instance", type=Path, required=True, help="Stopped instance; only metadata is copied")
    args = parser.parse_args()
    gui, source = args.gui.resolve(strict=True), args.instance.resolve(strict=True)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    probe = BUILD / "probes" / stamp / "gui-interactions"
    probe.mkdir(parents=True)
    environment = dict(os.environ, LOCALAPPDATA=str(probe / "profile"))
    # These are metadata-only UI fixtures. No service or database is started.
    fixtures = [probe / "实例 A", probe / "another folder" / "实例 B"]
    for fixture in fixtures:
        (fixture / "data").mkdir(parents=True)
        for name in ("services.ini", "server.cnf", "data/auto.cnf"):
            shutil.copy2(source / name, fixture / name)
    identities = [{p.relative_to(fixture).as_posix(): sha256(p) for p in fixture.rglob("*") if p.is_file()} for fixture in fixtures]
    record = {"os": platform.platform(), "gui_sha256": sha256(gui), "checks": {}, "scope": "Interactive rendering and metadata-only instance selection; no service operations."}
    checks = record["checks"]
    process, window = None, None
    cursor = w.POINT()
    user.GetCursorPos(c.byref(cursor))

    def hover_cards(expected):
        user.SetForegroundWindow(window)
        grid = user.GetDlgItem(window,220)
        origin = w.POINT()
        user.ClientToScreen(grid,c.byref(origin))
        results = []
        for index,row in enumerate(rows(window)):
            name=row.split("  |  ",1)[0].removesuffix("（可选）")
            # Leave the previous tool before entering the next card so that
            # visible text always belongs to the new mouse-hover event.
            blank=w.RECT();user.GetClientRect(grid,c.byref(blank))
            user.SetCursorPos(origin.x+blank.right-2,origin.y+blank.bottom-2)
            wait_for(lambda: not any(user.IsWindowVisible(tip) for tip in windows(process.pid,"tooltips_class32")),
                     "previous tooltip hidden",5)
            area=w.RECT()
            user.SendMessageW(grid,0x198,index,c.addressof(area))
            user.SetCursorPos(origin.x+(area.left+area.right)//2,origin.y+(area.top+area.bottom)//2)
            tip=wait_for(lambda: next((tip for tip in windows(process.pid,"tooltips_class32")
                                      if user.IsWindowVisible(tip) and title(tip)),None),
                         name+" real mouse hover",5)
            text=title(tip)
            assert "单击选择" not in text and name not in text and "（" not in text and "）" not in text, text
            if name in expected: assert text == expected[name], text
            results.append(text)
        # The empty area after the final card is not a service tool.
        last=w.RECT();user.SendMessageW(grid,0x198,18,c.addressof(last))
        client=w.RECT();user.GetClientRect(grid,c.byref(client))
        user.SetCursorPos(origin.x+client.right-2,origin.y+(last.top+last.bottom)//2)
        wait_for(lambda: not any(user.IsWindowVisible(tip) for tip in windows(process.pid,"tooltips_class32")),
                 "blank card area has no tooltip",5)
        return results

    def launch(initial=None):
        nonlocal process, window
        process = subprocess.Popen([str(gui), *(["--instance", str(initial)] if initial else [])], env=environment)
        window = wait_for(lambda: next(iter(windows(process.pid, "DNFNativeController")), None), "controller")
        wait_for(lambda: len(rows(window)) == 19, "19 cards")

    def close():
        nonlocal window
        user.PostMessageW(window, 0x10, 0, 0)
        assert process.wait(timeout=30) == 0
        window = None

    try:
        launch(fixtures[0])
        grid = user.GetDlgItem(window, 220)
        purposes={"game":"游戏主服务","auction":"拍卖行","point":"金币寄售"}
        record["service_tooltips"] = hover_cards(purposes)
        checks["all_19_cards_show_service_purpose_on_real_mouse_hover_and_blank_area_has_none"] = True
        rect = w.RECT()
        user.SendMessageW(grid, 0x198, 9, c.addressof(rect))
        point = ((rect.left + rect.right) // 2) | ((rect.top + rect.bottom) // 2 << 16)
        user.SendMessageW(grid, 0x198, 10, c.addressof(rect))
        other_point = ((rect.left + rect.right) // 2) | ((rect.top + rect.bottom) // 2 << 16)
        user.SendMessageW(grid, 0x201, 1, point)
        user.SendMessageW(grid, 0x202, 0, point)
        wait_for(lambda: user.SendMessageW(grid, 0x188, 0, 0) == 9, "selected community")
        before_log = title(user.GetDlgItem(window, 206))
        capture = Capture(grid)
        highlight = user.GetSysColor(13)
        color = bytes(((highlight >> 16) & 255, (highlight >> 8) & 255, highlight & 255))

        def highlight_count():
            raw = capture.pixels()
            return sum(raw[i:i+3] == color for i in range(0, len(raw), 4))

        baseline = highlight_count()
        maximum, samples = baseline, 0
        stop = threading.Event()

        def repeated_clicks():
            while not stop.is_set():
                for target in (point, point, other_point, other_point):
                    user.PostMessageW(grid, 0x201, 1, target)
                    user.PostMessageW(grid, 0x202, 0, target)
                    time.sleep(.01)

        thread = threading.Thread(target=repeated_clicks)
        thread.start()
        try:
            until = time.monotonic() + 2
            while time.monotonic() < until:
                maximum = max(maximum, highlight_count())
                samples += 1
        finally:
            stop.set()
            thread.join()
            capture.close()
        assert maximum <= baseline + 20, (baseline, maximum)
        assert title(user.GetDlgItem(window, 206)) == before_log
        record["selection_frames"] = {"samples": samples, "system_highlight_baseline": baseline, "system_highlight_maximum": maximum}
        checks["repeated_selected_card_has_no_native_blue_row_or_log_reload"] = True

        switch = user.GetDlgItem(window, 242)
        user.SendMessageW(switch, 0x201, 1, 10 | (10 << 16))
        user.SendMessageW(switch, 0x202, 0, 10 | (10 << 16))
        time.sleep(.1)
        assert user.SendMessageW(switch, 0x129, 0, 0) & 1  # WM_QUERYUISTATE / UISF_HIDEFOCUS
        screenshot(window, probe / "mouse-switch.png")
        focus(window, user.GetDlgItem(window, 243))
        key(window, 9)
        wait_for(lambda: not user.SendMessageW(user.GetDlgItem(window, 242), 0x129, 0, 0) & 1, "keyboard focus cues")
        screenshot(window, probe / "keyboard-switch.png")
        checks["mouse_hides_focus_ring_and_tab_restores_keyboard_cue"] = True

        for index in range(40):
            width = 1180 if index % 2 == 0 else 1440
            user.MoveWindow(window, 40, 40, width, 820, True)
            # WM_NULL waits for the current layout message to finish before
            # comparing positions; snapshots across different layouts mislead.
            user.SendMessageW(window, 0, 0, 0)
        screenshot(window, probe / "after-resize.png")
        checks["repeated_minimum_default_resize_remains_responsive"] = True
        assert hover_cards(purposes) == record["service_tooltips"]
        checks["service_tooltip_rectangles_follow_window_resize"] = True

        click(window, 300)
        market = wait_for(lambda: next(iter(windows(process.pid, "DnfNativeMarketMaintenance")), None), "market")
        focus(market, user.GetDlgItem(market, 301))
        key(market, 27)
        wait_for(lambda: not user.IsWindow(market), "Esc closes market")
        assert user.IsWindowEnabled(window)
        checks["market_escape_closes_and_restores_main_window"] = True

        # The service area has only the feature button. Open the real native
        # context menu and activate inspection, which now writes the main log.
        focus(window, grid)
        baseline = len(title(user.GetDlgItem(window, 206)))
        user.PostMessageW(window, 0x7B, grid, -1)
        wait_for(lambda: menu_open(window), "service context menu")
        popup=wait_for(lambda: next(iter(windows(process.pid,"#32768")),None),"service popup window",5)
        menu=user.SendMessageW(popup,0x01E1,0,0)  # MN_GETHMENU returns a system menu handle.
        assert user.GetMenuItemCount(menu)==3
        menu_labels=[]
        for index,identifier in enumerate([221,222,262]):
            assert user.GetMenuItemID(menu,index)==identifier
            label=c.create_unicode_buffer(128)
            assert user.GetMenuStringW(menu,index,label,len(label),0x400)
            menu_labels.append(label.value)
        assert menu_labels==["启动所选(&S)","停止所选(&T)","检查资源(&I)"],menu_labels
        record["service_context_menu_labels"]=menu_labels
        checks["service_context_menu_has_three_four_character_actions_without_log_entry"]=True
        key(window, ord("I"))
        wait_for(lambda: "SHA-256：" in title(user.GetDlgItem(window, 206))[baseline:], "inspection from context menu")
        assert not windows(process.pid, "#32770")
        checks["service_context_inspection_uses_main_log_without_dialog"] = True

        focus(window, user.GetDlgItem(window, 102))
        key(window, 13)
        wait_for(lambda: menu_open(window), "selector menu")
        key(window, ord("O"))
        select_folder(process, fixtures[1])
        wait_for(lambda: title(user.GetDlgItem(window, 101)) == str(fixtures[1]), "other instance selected")
        history = probe / "profile/DNF-Native/instances.ini"
        wait_for(lambda: history.exists(), "saved instance history")
        ini = configparser.ConfigParser(interpolation=None)
        ini.read(history, encoding="utf-16")
        assert ini["instances"]["selected"] == str(fixtures[1])
        assert {ini["instances"]["path0"], ini["instances"]["path1"]} == set(map(str, fixtures))
        close()
        launch()
        assert title(user.GetDlgItem(window, 101)) == str(fixtures[1])
        assert hover_cards(purposes) == record["service_tooltips"]
        checks["service_tooltips_refresh_after_remembered_instance_change"] = True
        focus(window, user.GetDlgItem(window, 102))
        key(window, 13)
        wait_for(lambda: menu_open(window), "remembered selector menu")
        key(window, ord("1"))
        wait_for(lambda: title(user.GetDlgItem(window, 101)) == str(fixtures[0]), "first remembered instance")
        close()
        launch()
        assert title(user.GetDlgItem(window, 101)) == str(fixtures[0])
        screenshot(window, probe / "restored-instance.png")
        checks["selection_only_saves_both_instances_and_restores_last_across_restarts"] = True
        close()
        for fixture, identity in zip(fixtures, identities):
            # Opening Features can create a derived DP2 script cache in .mysql.
            # Every input metadata file must retain its original contents.
            assert {name: sha256(fixture / name) for name in identity} == identity
        checks["instance_metadata_unchanged_and_history_external_to_package"] = True
    except Exception as error:
        record["error"] = type(error).__name__ + ": " + str(error)
        record["traceback"] = traceback.format_exc()
    finally:
        user.SetCursorPos(cursor.x,cursor.y)
        if window and user.IsWindow(window):
            close()
        record["passed"] = "error" not in record and all(checks.values())
        (probe / "receipt.json").write_text(json.dumps(record, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print("GUI interactions:", "PASS" if record["passed"] else "FAIL", probe / "receipt.json", flush=True)
    if not record["passed"]:
        raise SystemExit(record["error"])


if __name__ == "__main__":
    main()
