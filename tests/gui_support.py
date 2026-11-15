"""Win32 control and screenshot helpers for current GUI regressions."""

import ctypes as c
from ctypes import wintypes as w
import struct
import subprocess
import time
import zlib

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


def list_rows(window):
    box = user.GetDlgItem(window, 220)
    result = []
    for index in range(user.SendMessageW(box, 0x18B, 0, 0)):  # LB_GETCOUNT
        text = c.create_unicode_buffer(user.SendMessageW(box, 0x18A, index, 0) + 1)
        user.SendMessageW(box, 0x189, index, c.addressof(text))
        result.append(text.value)
    return result


def rows(window):
    # Group headers have no service identity and must never count as a service.
    return [text for text in list_rows(window) if not text.startswith(("▾ ", "▸ "))]

def select(window, index):
    box = user.GetDlgItem(window, 220)
    visible = -1
    target = None
    for row in range(user.SendMessageW(box, 0x18B, 0, 0)):
        text = c.create_unicode_buffer(user.SendMessageW(box, 0x18A, row, 0) + 1)
        user.SendMessageW(box, 0x189, row, c.addressof(text))
        if text.value.startswith(("▾ ", "▸ ")):
            continue
        visible += 1
        if visible == index:
            target = row
            break
    if target is None:
        raise RuntimeError("Service row index is not visible: " + str(index))
    user.SendMessageW(box, 0x186, target, 0)
    user.SendMessageW(window, 0x111, 220 | (1 << 16), box)


def service_row(window, name):
    return next(row for row in rows(window) if row.split("  |  ", 1)[0].removesuffix("（可选）") == name)


def select_service(window, name):
    index = next(i for i, row in enumerate(rows(window))
                 if row.split("  |  ", 1)[0].removesuffix("（可选）") == name)
    select(window, index)


def log_sources(window):
    combo = user.GetDlgItem(window, 240)
    result = []
    for index in range(user.SendMessageW(combo, 0x146, 0, 0)):
        text = c.create_unicode_buffer(user.SendMessageW(combo, 0x149, index, 0) + 1)
        user.SendMessageW(combo, 0x148, index, c.addressof(text))
        result.append(text.value)
    return result


def choose_log_source(window, label):
    index = wait_for(lambda: next((i + 1 for i, text in enumerate(log_sources(window)) if label in text), None), label) - 1
    combo = user.GetDlgItem(window, 240)
    user.SendMessageW(combo, 0x14E, index, 0)
    user.SendMessageW(window, 0x111, 240 | (1 << 16), combo)
