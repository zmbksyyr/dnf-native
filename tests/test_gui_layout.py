"""Check the real service grid, window bounds, DPI layout and log filtering."""
import argparse
import ctypes as c
from ctypes import wintypes as w
import datetime
import json
import os
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
user.GetDC.argtypes, user.GetDC.restype = [w.HWND], w.HDC
user.ReleaseDC.argtypes, user.ReleaseDC.restype = [w.HWND, w.HDC], c.c_int
gdi = c.WinDLL("gdi32", use_last_error=True)
gdi.SelectObject.argtypes, gdi.SelectObject.restype = [w.HDC, w.HGDIOBJ], w.HGDIOBJ
gdi.DeleteObject.argtypes = [w.HGDIOBJ]
gdi.CreateFontW.restype = w.HGDIOBJ
gdi.CreateFontW.argtypes = [c.c_int] * 5 + [w.DWORD] * 8 + [w.LPCWSTR]
gdi.GetTextExtentPoint32W.argtypes = [w.HDC, w.LPCWSTR, c.c_int, c.POINTER(w.SIZE)]


# A switch paints a 38px track, keeps 4px to the right edge and leaves 10px
# between the label and the track; anything narrower clips its label.
SWITCH_CHROME = 52


def text_width(text, dpi):
    # GDI handles do not cross processes, so the label is measured with the same
    # face and pixel height the controller creates for the current DPI.
    dc = user.GetDC(None)
    font = gdi.CreateFontW(-round(12 * dpi / 96), 0, 0, 0, 400, False, False, False,
                           1, 0, 0, 5, 0, "Microsoft YaHei UI")
    previous = gdi.SelectObject(dc, font)
    extent = w.SIZE()
    gdi.GetTextExtentPoint32W(dc, text, len(text), c.byref(extent))
    gdi.SelectObject(dc, previous)
    user.ReleaseDC(None, dc)
    gdi.DeleteObject(font)
    return extent.cx


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
    process = subprocess.Popen([str(gui), "--instance", str(source)],
                               env=dict(os.environ, LOCALAPPDATA=str(probe / "profile")))
    window = None
    try:
        window = wait_for(lambda: next(iter(windows(process.pid, "DNFNativeController")), None), "controller")
        wait_for(lambda: len(rows(window)) == 19, "19 service cards")
        wait_for(lambda: "CPU" in title(window) and "内存" in title(window), "title bar process metrics", 5)
        checks["title_bar_shows_live_cpu_and_memory"] = True
        grid = user.GetDlgItem(window, 220)
        scales = [(96, 1440, 940), (96, 1180, 760), (120, 1475, 950)]
        for dpi, width, height in scales:
            suggested = w.RECT(12, 12, 12 + width, 12 + height)
            user.SendMessageW(window, 0x02E0, dpi | dpi << 16, c.addressof(suggested))
            user.MoveWindow(window, 12, 12, width, height, True)
            client = w.RECT()
            user.GetClientRect(window, c.byref(client))
            visible = {}
            for identifier in range(100, 301):
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
                wait_for(lambda: title(user.GetDlgItem(window, 285)) == selected, "details match card", 5)
                tiles.append([area.left, area.top, area.right, area.bottom])
            assert tiles[0][1] == tiles[6][1] < tiles[7][1] == tiles[13][1] < tiles[14][1] == tiles[18][1]
            for identifier in [102, 223, 224, 220, 202, 111, 113, 114, 206, 283, 284, 285, 286, 287]:
                assert identifier in visible
            for identifier in [230,231,232,233,234,235,281,221,222,225]:
                assert not user.GetDlgItem(window,identifier), (identifier,"removed main-window control")
            assert visible[101][2] + round(44*dpi/96) < visible[250][0], "directory and IP groups touch"
            assert visible[102][3] < visible[220][1] < visible[202][1] < visible[206][1]
            for identifiers in [[102, 260, 101, 250, 251, 262, 116, 280, 224, 223],
                                [207, 213, 300],
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
        wait_for(lambda: title(user.GetDlgItem(window, 206)) == "".join(line for line in full.splitlines(keepends=True) if "mysql" in line.lower()), "case-insensitive MySQL match", 5)
        needle = "GUI-NO-SUCH-LOG-LINE-" + stamp
        set_text(edit, needle)
        wait_for(lambda: not title(user.GetDlgItem(window, 206)), "filter removes nonmatching lines", 5)
        set_text(edit, "")
        wait_for(lambda: title(user.GetDlgItem(window, 206)) == full, "clear filter restores raw text", 5)
        set_text(edit, "gui-no-such-log-line-" + stamp)
        assert not title(user.GetDlgItem(window, 206))
        set_text(edit, "")
        checks["log_filter_and_clear_use_actual_console_text"] = True
        click(window, 300)
        market = wait_for(lambda: next(iter(windows(process.pid, "DnfNativeMarketMaintenance")), None), "market maintenance window", 5)
        market_client = w.RECT()
        user.GetClientRect(market, c.byref(market_client))
        market_controls = [301, 302, 303, 304, 308, 309, 310, 311, 312, 313, 314, 315, 316, 317, 320, 321, 324, 325, 326, 230, 231, 232, 233, 234, 235]
        for identifier in market_controls:
            child = user.GetDlgItem(market, identifier)
            assert child and user.IsWindowVisible(child), (identifier, "market control missing")
            area = rect(child, market)
            assert 0 <= area.left < area.right <= market_client.right
            assert 0 <= area.top < area.bottom <= market_client.bottom, (identifier, area.top, area.bottom, market_client.bottom)
        for row, header in (([310, 311, 312, 313], 320), ([314, 315, 316, 317], 321)):
            areas=[rect(user.GetDlgItem(market, identifier), market) for identifier in row]
            assert len({area.top for area in areas}) == 1
            assert all(a.right < b.left for a,b in zip(areas,areas[1:]))
            assert rect(user.GetDlgItem(market, header), market).bottom <= areas[0].top
        for top,bottom in zip([310,311,312,313],[314,315,316,317]):
            a,b=rect(user.GetDlgItem(market,top),market),rect(user.GetDlgItem(market,bottom),market)
            assert a.left==b.left and a.right==b.right and a.bottom<b.top
        for identifier in [318,319,323,307]:
            assert not user.GetDlgItem(market,identifier)
        for identifier in [230,231,301,302,303,304]:
            area=rect(user.GetDlgItem(market,identifier),market)
            needed=text_width(title(user.GetDlgItem(market,identifier)),120)+round(SWITCH_CHROME*120/96)
            assert area.right-area.left>=needed, (identifier,"clipped switch text")
        for identifiers in [[235,233,234,230,231,232],[301,302],[303,304]]:
            for left,right in zip(identifiers,identifiers[1:]):
                assert rect(user.GetDlgItem(market,left),market).right < rect(user.GetDlgItem(market,right),market).left
        assert not user.GetDlgItem(market,307)
        summary=rect(user.GetDlgItem(market,308),market)
        state=rect(user.GetDlgItem(market,309),market)
        assert summary.bottom<=state.top and state.bottom-summary.top<=round(40*120/96)
        assert all(not user.IsWindowEnabled(user.GetDlgItem(market,identifier)) for identifier in range(310,318))
        assert title(market)=="功能"
        assert not user.GetDlgItem(market,306) and not user.GetDlgItem(window,204)
        assert not user.GetWindowLongW(user.GetDlgItem(window,206),-16) & 0x00300000
        screenshot(market,probe/"market-window.png")
        user.PostMessageW(market,0x10,0,0)
        wait_for(lambda:not user.IsWindow(market),"market title-bar close",5)
        checks["feature_settings_and_auction_two_rows_four_columns"] = True
        checks["both_market_scope_is_fixed_and_logs_have_no_scrollbars"] = True
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
