"""Check Linux file ABI and CNG random reads through both native hosts."""

import argparse
import ctypes
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / ".local/toolchains/llvm-mingw-20261104-msvcrt-x86_64/bin"


def native_inode(path):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p,
                                  ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
    kernel.CreateFileW.restype = ctypes.c_void_p
    kernel.GetFileInformationByHandle.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    kernel.GetVolumeInformationByHandleW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_uint32,
                                                    ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                                                    ctypes.c_wchar_p, ctypes.c_uint32]
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    handle = kernel.CreateFileW(str(path), 0, 7, None, 3, 0, None)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        info = (ctypes.c_uint32 * 13)()
        filesystem = ctypes.create_unicode_buffer(32)
        if not kernel.GetFileInformationByHandle(handle, info):
            raise ctypes.WinError(ctypes.get_last_error())
        if not kernel.GetVolumeInformationByHandleW(handle, None, 0, None, None, None, filesystem, 32):
            raise ctypes.WinError(ctypes.get_last_error())
        file_id = (info[11] << 32) | info[12]
        inode = file_id & 0x0000ffffffffffff if filesystem.value == "NTFS" else file_id
        return {"filesystem": filesystem.value, "native_file_id": file_id, "expected_inode": inode}
    finally:
        kernel.CloseHandle(handle)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-only", action="store_true")
    args = parser.parse_args()
    env = dict(os.environ)
    env["PATH"] = str(BIN) + os.pathsep + env.get("PATH", "")
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    records = []
    for arch, target, base, size, inode_offset in [("i686", "i386-linux-gnu", "0x8048000", 96, 88),
                                                   ("x86_64", "x86_64-linux-gnu", "0x400000", 144, 8)]:
        fixture = ROOT / "build/fixtures" / arch / "files"
        fixture.parent.mkdir(parents=True, exist_ok=True)
        build = [str(BIN / "clang.exe"), "--target=" + target, "-fuse-ld=lld", "-nostdlib", "-static",
                 "-ffreestanding", "-fno-stack-protector", "-fno-pic", "-O2", "-Wall", "-Wextra", "-Werror",
                 "-Wl,--image-base=" + base, "-Wl,-e,_start", str(ROOT / "tests/files.c"), "-o", str(fixture)]
        subprocess.run(build, env=env, check=True)
        peer_fixture = fixture.with_name("files-peer")
        peer_build = [*build[:-1], str(peer_fixture), "-DFILE_PEER"]
        subprocess.run(peer_build, env=env, check=True)
        if args.build_only:
            print(arch, "files fixture built")
            continue
        instance = ROOT / "build/probes" / stamp / ("files-" + arch)
        instance.mkdir(parents=True)
        (instance / "sample").write_bytes(b"abcdef")
        os.link(instance / "sample", instance / "sample-link")
        (instance / "directory/beta").mkdir(parents=True)
        (instance / "directory/alpha").write_bytes(b"directory entry")
        identity = native_inode(instance / "sample")
        sha = hashlib.sha256(fixture.read_bytes()).hexdigest()
        host = ROOT / "build" / arch / "dnf-host.exe"
        command = [str(host), "--root", str(instance), "0" * 64, "--run", str(fixture), sha, "/proc-test"]
        peer_sha = hashlib.sha256(peer_fixture.read_bytes()).hexdigest()
        peer_command = [str(host), "--root", str(instance), "0" * 64, "--run", str(peer_fixture), peer_sha, "/proc-test"]
        with subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as process:
            initial = process.stdout.read(size + 13)
            peer = subprocess.run(peer_command, capture_output=True, timeout=20)
            tail, stderr = process.communicate(b"g", timeout=20)
        stdout = initial + tail
        inode = int.from_bytes(stdout[inode_offset:inode_offset + 8], "little")
        content = (instance / "sample").read_bytes()
        shared = (instance / "shared").read_bytes()
        passed = (process.returncode == 42 and peer.returncode == 42 and peer.stdout == b"peer-ok\n" and
                  stdout[size:] == b"shared-ready\nfiles-ok\n" and inode == identity["expected_inode"] and
                  content == b"abcd\0\0\0\0\0" and shared[:3] == b"shp" and len(shared) == 280 and
                  not (instance / "dev/urandom").exists())
        records.append({"arch": arch, "build": build, "command": command, "fixture_sha256": sha,
                        "host_sha256": hashlib.sha256(host.read_bytes()).hexdigest(), **identity,
                        "guest_inode": inode, "exit_code": process.returncode,
                        "stderr": stderr.decode("utf-8", "replace"),
                        "peer_build": peer_build, "peer_command": peer_command, "peer_fixture_sha256": peer_sha,
                        "peer_exit_code": peer.returncode, "peer_stderr": peer.stderr.decode("utf-8", "replace"),
                        "passed": passed})
        print(arch, "files, directory pagination and live shared mappings", "PASS" if passed else "FAIL", flush=True)
    if args.build_only:
        return
    result = {"recorded_utc": stamp, "os": platform.platform(),
              "scope": "Own Linux fixtures: file identity, CNG reads, directory pagination/EOF/rewind, live shared aliases and cross-process visibility. Not service acceptance.",
              "results": records}
    (ROOT / "build/files-checks.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if not all(r["passed"] for r in records):
        raise SystemExit("File checks failed")


if __name__ == "__main__":
    main()
