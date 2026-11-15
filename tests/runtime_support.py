"""Private baseline fixtures and file identity checks shared by regressions."""

import ctypes
from ctypes import wintypes as w
import hashlib
from pathlib import Path
import sys
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from bootstrap import external_output, sha256
from resource_baseline import RESOURCES, verify_baseline


def unpack_runtime(destination, arch, prefixes=("lib/", "lib64/", "usr/")):
    """Copy selected verified native ZIP members into a private external root."""
    destination = external_output(destination)
    verify_baseline()
    start = arch + "/"
    with zipfile.ZipFile(RESOURCES / "native.zip") as archive:
        for name in archive.namelist():
            if not name.startswith(start):
                continue
            relative = name[len(start):]
            if not relative.startswith(prefixes):
                continue
            target = destination / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(archive.read(name))
    for name in ("tmp", "dev/shm/sec"):
        (destination / name).mkdir(parents=True, exist_ok=True)
    return destination


def tree_identity(directory):
    digest, count, size = hashlib.sha256(), 0, 0
    for path in sorted(directory.rglob("*")):
        if not path.is_file():
            continue
        length = path.stat().st_size
        digest.update((path.relative_to(directory).as_posix() + "\0" + str(length) + "\0" + sha256(path) + "\n").encode("utf-8"))
        count += 1
        size += length
    return {"files": count, "bytes": size, "tree_sha256": digest.hexdigest()}


def request_stop(pid, signal=15):
    # Same runtime events used by cross-process guest kill(). This run mode
    # does not write a PID file, so its original stop helper cannot target it.
    if signal not in [12, 15]:
        raise ValueError("Only SIGUSR2 and SIGTERM have runtime events")
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenEventW.argtypes = [w.DWORD, w.BOOL, w.LPCWSTR]
    kernel.OpenEventW.restype = w.HANDLE
    kernel.SetEvent.argtypes = [w.HANDLE]
    kernel.CloseHandle.argtypes = [w.HANDLE]
    prefix = "Local\\dnf-native-sigusr2-" if signal == 12 else "Local\\dnf-native-terminate-"
    event = kernel.OpenEventW(2, False, prefix + str(pid))
    if not event:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        if not kernel.SetEvent(event):
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        kernel.CloseHandle(event)
