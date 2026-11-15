"""Observe original game initialization in a fresh private root; no readiness claim."""

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import time

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from test_channel import request_stop

from bootstrap import LOCAL, BUILD, ORIGINALS

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--host", type=Path, default=BUILD / "i686/dnf-host.exe")
    parser.add_argument("--trace", action="store_true")
    parser.add_argument("--preload", action="store_true", help="Use the original DP2 preload chain")
    parser.add_argument("--env", action="append", default=[], help="Add a guest NAME=VALUE environment setting")
    parser.add_argument("--lua-script", type=Path, help="Use a test Lua script in the private DP2 copy")
    parser.add_argument("--frida-script", type=Path, help="Use a test JS script in the private DP2 copy")
    parser.add_argument("--frida-teardown", choices=["minimal", "full"], help="Override Gadget cleanup in the private config")
    parser.add_argument("--dp2-cleanup", action="store_true",
                        help="Close luv handles before plugin destruction and use Gadget full teardown")
    parser.add_argument("--stop-after-started", type=float, metavar="SECONDS",
                        help="Observe this long after the original startup marker, then send SIGUSR2")
    parser.add_argument("--db-config", type=Path, help="Private JSON connection settings for an isolated test database")
    args = parser.parse_args()
    if (args.lua_script or args.frida_script or args.frida_teardown or args.dp2_cleanup) and not args.preload:
        parser.error("Test scripts require --preload")
    if args.dp2_cleanup and args.frida_teardown == "minimal":
        parser.error("--dp2-cleanup requires Gadget full teardown")
    if args.stop_after_started is not None and args.stop_after_started < 0:
        parser.error("--stop-after-started must be nonnegative")
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    instance = BUILD / "probes" / stamp / "game"
    instance.mkdir(parents=True)
    for name in ["lib", "usr"]:
        shutil.copytree(LOCAL / "sysroot/i686" / name, instance / name)
    shutil.copytree(ORIGINALS / "dp123/usr/local/share/GeoIP", instance / "usr/local/share/GeoIP")
    (instance / "tmp").mkdir()
    # POSIX shm_open names are resolved below /dev/shm. Keep the isolated
    # root's standard shared-memory directory present for the original TSS
    # startup path.
    (instance / "dev/shm/sec").mkdir(parents=True)
    for name in ["libnxencryption.so", "libGeoIP.so.1"]:
        shutil.copy2(ORIGINALS / "dp123/lib" / name, instance / "lib" / name)
    original = ORIGINALS / "dp123/home/neople/game"
    guest_dir = instance / "home/neople/game"
    guest_dir.mkdir(parents=True)
    for source in original.iterdir():
        if source.is_file():
            shutil.copy2(source, guest_dir / source.name)
        elif source.name not in ["log", "pid", "history"]:
            shutil.copytree(source, guest_dir / source.name)
    for name in ["log", "pid", "history"]:
        (guest_dir / name).mkdir()
    if args.db_config:
        db = json.loads(args.db_config.read_text(encoding="utf-8"))
        config = guest_dir / "cfg/siroco11.cfg"
        text = config.read_text(encoding="utf-8")
        for setting, key in [("ip", "host"), ("port", "port"), ("acc", "user"), ("pwd", "password"), ("name", "database")]:
            text, count = re.subn(r"(?m)^master_db_" + setting + r"[^\S\r\n]*=.*$", lambda match: "master_db_" + setting + " = " + str(db[key]), text)
            if count != 1:
                raise ValueError("Expected one original master database setting")
        config.write_text(text, encoding="utf-8")
    elf, ld = guest_dir / "df_game_r", instance / "lib/ld-2.17.so"
    # GLib resolves the home directory while Frida is loading on another thread.
    # Supply the guest user's normal HOME instead of triggering NSS dlopen there.
    command = [str(args.host.resolve()), "--root", str(instance), hashlib.sha256(ld.read_bytes()).hexdigest(),
               "--env", "HOME=/home/neople",
               "--cwd", "/home/neople/game", "--run", str(elf), hashlib.sha256(elf.read_bytes()).hexdigest(),
               "/home/neople/game/df_game_r", "siroco11", "nofork"]
    if args.preload:
        shutil.copytree(ORIGINALS / "dp123/dp2", instance / "dp2")
        for source, name in [(args.lua_script, "df_game_r.lua"), (args.frida_script, "df_game_r.js")]:
            if source:
                shutil.copy2(source, instance / "dp2" / name)
        if args.dp2_cleanup:
            with (instance / "dp2/df_game_r.lua").open("ab") as script:
                script.write(b"\n" + (ROOT / "src/runtime/dp2_cleanup.lua").read_bytes())
        if args.frida_teardown or args.dp2_cleanup:
            config = instance / "dp2/frida/frida.config"
            settings = json.loads(config.read_text(encoding="utf-8"))
            settings["teardown"] = "full" if args.dp2_cleanup else args.frida_teardown
            config.write_text(json.dumps(settings, indent=2) + "\n", encoding="utf-8")
        command[4:4] = ["--env", "LD_PRELOAD=/dp2/libdp2pre.so", "--env", "LD_LIBRARY_PATH=/dp2/lib:/home/neople/game"]
    command[4:4] = [part for value in args.env for part in ["--env", value]]
    env = dict(os.environ)
    if args.trace:
        env["DNF_NATIVE_TRACE"] = "1"
    forced = False
    startup_seen = stop_sent = None
    started = time.monotonic()
    with (instance / "stdout.log").open("wb") as stdout, (instance / "stderr.log").open("wb") as stderr:
        process = subprocess.Popen(command, stdout=stdout, stderr=stderr, env=env)
        try:
            if args.stop_after_started is None:
                process.wait(timeout=args.timeout)
            else:
                while process.poll() is None:
                    now = time.monotonic()
                    remaining = started + args.timeout - now
                    if remaining <= 0:
                        raise subprocess.TimeoutExpired(command, args.timeout)
                    if startup_seen is None and any(b"server has been started successfully" in path.read_bytes()
                                                    for path in (guest_dir / "log").rglob("*.init")):
                        startup_seen = now
                        print("Original game startup marker observed", flush=True)
                    if startup_seen is not None and stop_sent is None and now - startup_seen >= args.stop_after_started:
                        request_stop(process.pid, 12)
                        stop_sent = time.monotonic()
                        print("Requested original game SIGUSR2 shutdown", flush=True)
                    try:
                        process.wait(timeout=min(1, remaining))
                    except subprocess.TimeoutExpired:
                        pass
        except subprocess.TimeoutExpired:
            forced = True
            process.terminate()
            process.wait(timeout=5)
    stderr = (instance / "stderr.log").read_text(encoding="utf-8", errors="replace")
    initialization = "\n".join(p.read_text(encoding="utf-8", errors="replace") for p in (guest_dir / "log").rglob("*.init"))
    shutdown = (instance / "stdout.log").read_text(encoding="utf-8", errors="replace") + "\n" + "\n".join(
        p.read_text(encoding="utf-8", errors="replace") for p in (guest_dir / "log").rglob("*.log"))
    markers = ["All DB Open Success", "Init Query Table Success", "Init GlobalData Success",
               "Init HistoryLog Success", "InitRDARScriptStringManager Success", "Init DataManager Success"]
    inputs = [elf, guest_dir / "Script.pvf", guest_dir / "captcha_font/SeoulHangangL.ttf",
              instance / "usr/local/share/GeoIP/GeoIP.dat", guest_dir / "cfg/siroco11.cfg"]
    if args.preload:
        inputs.extend([instance / "dp2/libdp2pre.so", instance / "dp2/libdp2.so",
                       instance / "dp2/lib/liblua53.so", instance / "dp2/lib/libdp2game.so",
                       instance / "dp2/frida/frida.so",
                       instance / "dp2/frida/frida.config",
                       instance / "dp2/df_game_r.lua", instance / "dp2/df_game_r.js"])
    result = {"recorded_utc": stamp, "os": platform.platform(), "command": command,
              "host_sha256": hashlib.sha256(Path(command[0]).read_bytes()).hexdigest(),
              "elf_sha256": hashlib.sha256(elf.read_bytes()).hexdigest(), "exit_code": process.returncode,
              "scope": "Original game startup observation only; no service readiness, database or plugin acceptance.",
              "forced_cleanup": forced,
              "dp2_cleanup": args.dp2_cleanup,
              "startup_seen_after_seconds": round(startup_seen - started, 3) if startup_seen is not None else None,
              "stop_signal": 12 if stop_sent is not None else None,
              "observation_after_start_seconds": round(stop_sent - startup_seen, 3) if stop_sent is not None else None,
              "shutdown_markers": [marker for marker in ["RECV SIGUSR2", "ALL USER LOGOUT COMPLETE", "GAME SERVER SHUTDOWN COMPLETE"]
                                   if marker in shutdown],
              "elapsed_seconds": round(time.monotonic() - started, 3),
              "initialization_markers": [marker for marker in markers if marker in initialization],
              "inputs": {str(p.relative_to(instance)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs},
              "syscall_gaps": sorted({line.split(" pc=", 1)[0] for line in stderr.splitlines() if "syscall gap" in line}),
              "runtime_gaps": sorted({line.split(" pc=", 1)[0] for line in stderr.splitlines()
                                      if any(marker in line for marker in ["unsupported", "snapshot failed", "decode gap", "table exhausted", "unimplemented guest"])}),
              "logs": {str(p.relative_to(instance)): {"bytes": p.stat().st_size, "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
                       for p in instance.rglob("*") if p.is_file() and (p.suffix == ".log" or "log" in p.parts)}}
    receipt = json.dumps(result, indent=2) + "\n"
    (instance / "receipt.json").write_text(receipt, encoding="utf-8")
    (BUILD / "game-probe.json").write_text(receipt, encoding="utf-8")
    print("Original game:", "exit", process.returncode, "forced cleanup", forced, "gaps", result["syscall_gaps"])
    print("Private logs:", instance)


if __name__ == "__main__":
    main()
