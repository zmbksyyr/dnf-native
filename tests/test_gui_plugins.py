"""Verify native plugin switches using original ELF, real Lua/Frida and game startup."""

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
import socket
import struct
import subprocess
import time
import traceback

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from bootstrap import sha256, BUILD
from runtime_support import tree_identity
from probe_core_protocols import channel, relay, stun
from gui_support import user, kernel, windows, wait_for, click, ready, ready_stopped, screenshot
from gui_support import rows, select_service, service_row

from gui_support import guest_root

ROOT = Path(__file__).resolve().parents[1]


def loaded_plugins(pid, instance):
    """Observe real Linux ELF headers in the live 32-bit host's memory."""
    expected = {}
    for name, path in [("dp2", "dp2/libdp2.so"), ("frida", "dp2/frida/frida.so")]:
        data = (guest_root(instance) / path).read_bytes()
        phoff = struct.unpack_from("<I", data, 28)[0]
        stride, count = struct.unpack_from("<HH", data, 42)
        expected[name] = data[:phoff + stride * count]
    class Memory(c.Structure):
        _fields_ = [("base", c.c_void_p), ("allocation", c.c_void_p), ("allocation_protect", w.DWORD),
                    ("region_size", c.c_size_t), ("state", w.DWORD), ("protect", w.DWORD), ("type", w.DWORD)]
    kernel.OpenProcess.argtypes, kernel.OpenProcess.restype = [w.DWORD, w.BOOL, w.DWORD], w.HANDLE
    kernel.VirtualQueryEx.argtypes, kernel.VirtualQueryEx.restype = [w.HANDLE, c.c_void_p, c.POINTER(Memory), c.c_size_t], c.c_size_t
    kernel.ReadProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)]
    kernel.CloseHandle.argtypes = [w.HANDLE]
    process = kernel.OpenProcess(0x410, False, pid)
    if not process:
        raise c.WinError(c.get_last_error())
    found = {name: False for name in expected}
    try:
        address = 0
        info = Memory()
        while address < 0x100000000:
            if not kernel.VirtualQueryEx(process, address, c.byref(info), c.sizeof(info)):
                raise c.WinError(c.get_last_error())
            if info.state == 0x1000 and not info.protect & 0x101:
                size = min(info.region_size, max(map(len, expected.values())))
                data = c.create_string_buffer(size)
                count = c.c_size_t()
                if kernel.ReadProcessMemory(process, address, data, size, c.byref(count)):
                    for name, header in expected.items():
                        if data.raw[:len(header)] == header:
                            found[name] = True
            address = (info.base or 0) + info.region_size
        return found
    finally:
        kernel.CloseHandle(process)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-instance", type=Path, required=True, help="Stopped private instance; never used in place")
    parser.add_argument("--gui", type=Path, default=BUILD / "package/DNF-Native.exe")
    parser.add_argument("--hosts", type=Path, help="Use hosts from ARCH/dnf-host.exe in this build directory, in the new private instance only")
    parser.add_argument("--nosegneg", action="store_true", help="Select the packaged CentOS glibc variant for the game performance comparison")
    parser.add_argument("--timeout", type=float, default=2100)
    args = parser.parse_args()
    gui, source = args.gui.resolve(strict=True), args.source_instance.resolve(strict=True)
    with socket.socket() as available:
        available.bind(("127.0.0.1", 3306))
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    probe = BUILD / "probes" / stamp / "gui-plugins"
    instance = probe / "插件 实例(1)"
    source_before = tree_identity(source / "data")
    shutil.copytree(source, instance, ignore=shutil.ignore_patterns(".mysql", "service-logs"))
    # A copied init log may already contain the ready marker. Keep it outside
    # this run's log directory so startup is observed from fresh output.
    game_logs = guest_root(instance) / "home/neople/game/log"
    if game_logs.exists():
        game_logs.rename(probe / "prior-game-logs")
    (game_logs / "siroco11").mkdir(parents=True)
    if args.hosts:
        program = probe / "program"
        shutil.copytree(gui.parent, program)
        gui = program / gui.name
        resource_ini = program / "resources/resources.ini"
        resources = configparser.ConfigParser(interpolation=None)
        resources.read(resource_ini, encoding="utf-16")
        for arch, label in [("i686", "x86"), ("x86_64", "x64")]:
            name = "dnf-host-" + label + ".exe"
            target = program / "resources" / name
            shutil.copy2(args.hosts.resolve(strict=True) / arch / "dnf-host.exe", target)
            resources["package"][name] = sha256(target)
        with resource_ini.open("w", encoding="utf-16") as stream:
            resources.write(stream)
    assert tree_identity(instance / "data") == source_before
    manifest = instance / "services.ini"
    ini = configparser.ConfigParser(interpolation=None)
    ini.read(manifest, encoding="utf-16")
    ini["game"]["automatic"] = "0"
    if args.nosegneg:
        for key, value in ini["game"].items():
            if key.startswith("env") and value.startswith("LD_LIBRARY_PATH=") and "/lib/i686/nosegneg" not in value:
                ini["game"][key] = value.replace("LD_LIBRARY_PATH=", "LD_LIBRARY_PATH=/lib/i686/nosegneg:", 1)
    with manifest.open("w", encoding="utf-16") as stream:
        ini.write(stream)
    lua, js = guest_root(instance) / "dp2/df_game_r.lua", guest_root(instance) / "dp2/df_game_r.js"
    lua_before, js_before = lua.read_bytes(), js.read_bytes()
    module = guest_root(instance) / "dp2/lua/df/frida.lua"
    module_before = module.read_bytes()
    env = dict(os.environ)
    env["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32")
    record = {"recorded_utc": stamp, "os": platform.platform(), "gui_sha256": sha256(gui),
              "host_sha256": {arch: sha256(gui.parent / "resources" / ("dnf-host-" + label + ".exe"))
                              for arch, label in [("i686", "x86"), ("x86_64", "x64")]},
              "nosegneg": any(value.startswith("LD_LIBRARY_PATH=") and "/lib/i686/nosegneg" in value
                              for value in ini["game"].values()),
              "script_sha256": sha256(Path(__file__)), "checks": {}, "forced_cleanup": False,
              "scope": "Native GUI plugin modes, original Lua/Frida hooks and RPC, live ELF mapping checks, natural game startup with DP2 disabled. No client gameplay or clean multi-OS acceptance."}
    checks = record["checks"]
    process = None
    window = None

    def launch():
        nonlocal process, window
        process = subprocess.Popen([str(gui), "--instance", str(instance)], env=env)
        window = wait_for(lambda: next(iter(windows(process.pid, "DNFNativeController")), None), "native plugin window")

    def checked(identifier):
        return user.SendMessageW(user.GetDlgItem(window, identifier), 0xF0, 0, 0) == 1

    def save(dp2, frida):
        if checked(230) != dp2:
            click(window, 230); wait_for(lambda: checked(230) == dp2, "DP2 checkbox")
        if dp2 and checked(231) != frida:
            click(window, 231); wait_for(lambda: checked(231) == frida, "Frida checkbox")
        click(window, 232); ready(window, "插件设置已保存")
        ini.read(manifest, encoding="utf-16")
        assert ini["plugins"]["mode"] == ("off" if not dp2 else "frida" if frida else "lua")

    def fixture(frida):
        select_service(window, "game")
        previous = set((instance / "service-logs").glob("game-*.stdout.private.log"))
        click(window, 221)
        ready(window, "game：进程运行中", 30)
        log = wait_for(lambda: next(iter(set((instance / "service-logs").glob("game-*.stdout.private.log")) - previous), None), "new game log")
        marker = b"dnf-native-plugin-probe-hold"
        wait_for(lambda: marker in log.read_bytes(), "actual Lua/Frida fixture", 90)
        assert all(not user.IsWindowEnabled(user.GetDlgItem(window, key)) for key in [230, 231, 232])
        pid = int(service_row(window, "game").rsplit("PID ", 1)[1])
        observed = loaded_plugins(pid, instance)
        assert observed == {"dp2": True, "frida": frida}, observed
        wait_for(lambda: "已退出，代码 42" in service_row(window, "game"), "intentional fixture exit", 30)
        wait_for(lambda: user.IsWindowEnabled(user.GetDlgItem(window, 232)), "plugin editing after game exit")
        output = log.read_bytes()
        if frida:
            assert all(marker in output for marker in [b"dnf-native-original-game-hook-ok", b"dnf-native-js-lua-roundtrip-ok calls=32", b"dnf-native-lua-js-roundtrip-ok calls=32"])
        else:
            assert b"dnf-native-plugin-disabled-load-return=-1000" in output
        assert sum("进程运行中" in row for row in rows(window)) == 15
        return observed

    try:
        launch()
        assert checked(230) and checked(231)
        save(True, False)
        checks["original_module_backup_matches_before_save"] = (Path(str(module) + ".before-native-switch")).read_bytes() == module_before
        first_module = module.read_bytes()
        save(True, False)
        checks["repeated_save_preserves_module_bytes"] = module.read_bytes() == first_module
        user.PostMessageW(window, 0x10, 0, 0); process.wait(timeout=30)
        launch()
        checks["saved_lua_only_mode_survives_gui_restart"] = checked(230) and not checked(231)
        click(window, 223); ready(window, "已启动默认服务进程", 90)
        time.sleep(25)
        assert sum("进程运行中" in row for row in rows(window)) == 15
        hold = "\nprint('dnf-native-plugin-probe-hold')\nlocal uv=require('luv'); local t=uv.new_timer(); t:start(5000,0,function() t:close() end); uv.run(); os.exit(42)\n"
        bridge = (ROOT / "tests/frida_bridge.lua").read_text(encoding="utf-8").replace("os.exit(42)", hold)
        (guest_root(instance) / "dp2/plugin-bridge.lua").write_text(bridge, encoding="utf-8")
        lua.write_text("local frida=require('df.frida')\nif os.getenv('DNF_NATIVE_FRIDA')=='0' then\nassert(frida.load()==-1000)\nprint('dnf-native-plugin-disabled-load-return=-1000')\n" + hold + "\nelse dofile('/dp2/plugin-bridge.lua') end\n", encoding="ascii")
        js.write_bytes((ROOT / "tests/frida_bridge.js").read_bytes())
        checks["lua_running_without_frida_mapped"] = fixture(False)
        save(True, True)
        checks["frida_reenabled_real_hook_and_bidirectional_rpc"] = fixture(True)
        save(True, False)
        checks["frida_disabled_again_without_stopping_peers"] = fixture(False)
        save(False, False)
        assert not checked(231) and not user.IsWindowEnabled(user.GetDlgItem(window, 231))
        checks["dp2_off_also_disables_frida"] = True
        lua.write_bytes(lua_before); js.write_bytes(js_before)
        select_service(window, "game"); click(window, 221); ready(window, "game：进程运行中", 30)
        started = time.monotonic()
        log_dir = guest_root(instance) / "home/neople/game/log"
        wait_for(lambda: any(b"All DB Open Success" in path.read_bytes() for path in log_dir.rglob("*.init")), "original game database initialization", 90)
        pid = int(service_row(window, "game").rsplit("PID ", 1)[1])
        checks["dp2_off_live_modules"] = loaded_plugins(pid, instance)
        assert checks["dp2_off_live_modules"] == {"dp2": False, "frida": False}
        next_update = 0
        while not any(b"server has been started successfully" in path.read_bytes() for path in log_dir.rglob("*.init")):
            elapsed = time.monotonic() - started
            if elapsed > args.timeout or process.poll() is not None or any("已退出" in row for row in rows(window) if "（可选）" not in row):
                raise RuntimeError("Original game with DP2 disabled did not complete natural startup")
            if elapsed >= next_update:
                print("DP2 disabled: original game initialization", round(elapsed, 1), "seconds", flush=True)
                next_update = elapsed + 30
            time.sleep(1)
        checks["dp2_off_natural_game_startup_seconds"] = round(time.monotonic() - started, 3)
        checks["protocols_with_dp2_disabled"] = {"channel": channel(7001), "relay": relay(True), "stun": stun()}
        screenshot(window, probe / "plugins-off-running.png")
        click(window, 222); ready_stopped(window, "game", 210)
        checks["dp2_off_game_stop_code_zero"] = "代码 0" in service_row(window, "game")
        save(True, True)
        checks["plugins_editable_again_after_natural_game_stop"] = True
        user.PostMessageW(window, 0x10, 0, 0); process.wait(timeout=180)
        assert process.returncode == 0
        checks["close_normally_stops_remaining_peers_and_database"] = True
        checks["source_cold_database_unchanged"] = tree_identity(source / "data") == source_before
        assert not any(value is False for value in checks.values())
    except Exception as error:
        record["error"] = type(error).__name__ + ": " + str(error)
        record["traceback"] = traceback.format_exc()
        if window and process is not None and process.poll() is None:
            record["failure_rows"] = rows(window)
            screenshot(window, probe / "failure.png")
    finally:
        if process is not None and process.poll() is None:
            if window:
                user.PostMessageW(window, 0x10, 0, 0)
            try:
                process.wait(timeout=300)
            except subprocess.TimeoutExpired:
                record["forced_cleanup"] = True
                subprocess.run(["taskkill.exe", "/PID", str(process.pid), "/T", "/F"], capture_output=True, timeout=15)
                process.wait(timeout=10)
        lua.write_bytes(lua_before); js.write_bytes(js_before)
        record["exit_code"] = process.returncode if process else None
        record["instance"] = instance.relative_to(BUILD).as_posix()
        record["logs"] = {p.name: sha256(p) for p in (instance / "service-logs").glob("*.log")}
        (probe / "receipt.json").write_text(json.dumps(record, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")
        print("GUI plugin receipt:", probe / "receipt.json", flush=True)
    if "error" in record or record["forced_cleanup"]:
        raise SystemExit(record.get("error", "forced cleanup"))


if __name__ == "__main__":
    main()
