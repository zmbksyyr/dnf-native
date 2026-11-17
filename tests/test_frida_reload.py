"""Check real Gadget reloads and new RPC callbacks after two editor save styles."""

import argparse
import datetime
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from bootstrap import BUILD, sha256
from runtime_support import unpack_runtime, request_stop

GAME_SHA256 = "da417c5e65918f655afcddb6edfdaf2fa1e77b2f208e5aa397f021fa23b79979"
FRIDA_SHA256 = "8ca458e422dead8081c6e25831050dcbb006794c811bc2fe471f0cb59fdcc239"

LUA = '''local frida = require("df.frida")
local uv = require("luv")
assert(frida.load("reload-main") == 1)
print("dnf-native-reload-ready-1")
io.stdout:flush()
local timer = uv.new_timer()
local previous = 1
local count = 0
timer:start(100, 100, function()
    count = count + 1
    local result = frida.call(0, 0, "reload-probe")
    if result == previous + 1 then
        previous = result
        print("dnf-native-reload-ready-" .. result)
        io.stdout:flush()
        if result == 3 then timer:close() end
    elseif count >= 400 then
        print("dnf-native-reload-timeout last=" .. previous)
        io.stdout:flush()
        os.exit(70)
    end
end)
uv.run()
os.exit(42)
'''

JAVASCRIPT = '''var callbacks = [];
rpc.exports = {init: function () {
    var resolver = new NativeFunction(Module.getExportByName('libdp2.so', 'dp2_frida_resolver'),
                                      'pointer', ['pointer'], 'sysv');
    var main = new NativeCallback(function () { return VERSION; }, 'int', ['pointer', 'pointer'], 'sysv');
    var handler = new NativeCallback(function () { return VERSION; }, 'int',
                                    ['pointer', 'int', 'float', 'pointer'], 'sysv');
    callbacks.push(main, handler);
    Interceptor.replace(resolver(Memory.allocUtf8String('frida.main')), main);
    Interceptor.replace(resolver(Memory.allocUtf8String('frida.handler')), handler);
    Interceptor.flush();
    console.log('dnf-native-reload-script-VERSION');
}};
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", type=Path, default=BUILD / "i686/dnf-host.exe")
    args = parser.parse_args()
    host = args.host.resolve(strict=True)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    probe = BUILD / "probes" / stamp / "frida-reload"
    root = unpack_runtime(probe / "root", "i686", ("lib/", "usr/", "dp2/", "home/neople/game/"))
    for name in ["log", "pid", "history"]:
        (root / "home/neople/game" / name).mkdir(exist_ok=True)
    game = root / "home/neople/game/df_game_r"
    frida = root / "dp2/frida/frida.so"
    assert sha256(game) == GAME_SHA256 and sha256(frida) == FRIDA_SHA256
    lua, script = root / "dp2/df_game_r.lua", root / "dp2/df_game_r.js"
    lua.write_text(LUA, encoding="utf-8")
    script.write_text(JAVASCRIPT.replace("VERSION", "1"), encoding="utf-8")
    command = [str(host), "--root", str(root), sha256(root / "lib/ld-linux.so.2"),
               "--env", "HOME=/home/neople", "--env", "LD_PRELOAD=/dp2/libdp2pre.so",
               "--env", "LD_LIBRARY_PATH=/dp2/lib:/home/neople/game", "--cwd", "/home/neople/game",
               "--run", str(game), GAME_SHA256, "/home/neople/game/df_game_r", "siroco11", "nofork"]
    record = {"recorded_utc": stamp, "os": platform.platform(), "command": command,
              "script_sha256": sha256(Path(__file__)), "host_sha256": sha256(host),
              "elf_sha256": GAME_SHA256, "frida_sha256": FRIDA_SHA256,
              "scope": "Original Gadget on_change=reload: in-place/atomic saves and new RPC callbacks before game startup; the two tested save methods are recorded separately.",
              "forced_cleanup": False, "reloads": []}
    out, err = probe / "stdout.log", probe / "stderr.log"
    with out.open("wb") as stdout, err.open("wb") as stderr:
        process = subprocess.Popen(command, stdout=stdout, stderr=stderr)

    def wait_marker(version):
        marker = ("dnf-native-reload-ready-" + str(version)).encode()
        deadline = time.monotonic() + 15
        while True:
            if marker in out.read_bytes():
                return
            if process.poll() is not None:
                raise RuntimeError("Game exited before reload callback " + str(version))
            if time.monotonic() >= deadline:
                raise TimeoutError("Missing reload callback " + str(version))
            time.sleep(0.05)

    try:
        wait_marker(1)
        for version, style in [(2, "in-place"), (3, "atomic-replace")]:
            text = JAVASCRIPT.replace("VERSION", str(version))
            target = script if style == "in-place" else script.with_suffix(".js.new")
            target.write_text(text, encoding="utf-8")
            start = time.monotonic()
            if target != script:
                os.replace(target, script)
            wait_marker(version)
            record["reloads"].append({"version": version, "save_style": style,
                                      "callback_seconds": round(time.monotonic() - start, 3),
                                      "javascript_sha256": sha256(script)})
        process.wait(timeout=10)
    except Exception as error:
        record["error"] = type(error).__name__ + ": " + str(error)
    finally:
        if process.poll() is None:
            request_stop(process.pid)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                record["forced_cleanup"] = True
                process.terminate()
                process.wait(timeout=10)
        stdout = out.read_text(encoding="utf-8", errors="replace")
        stderr = err.read_text(encoding="utf-8", errors="replace")
        record["exit_code"] = process.returncode
        record["log_sha256"] = {p.name: sha256(p) for p in [out, err]}
        record["syscall_gaps"] = [line for line in stderr.splitlines() if "syscall gap" in line]
        record["runtime_gaps"] = [line for line in stderr.splitlines() if any(s in line for s in
                                   ["unhandled exception", "unsupported", "decode gap", "exhausted"])]
        record["passed"] = (not record.get("error") and not record["forced_cleanup"] and
                            process.returncode == 42 and not record["runtime_gaps"] and
                            all(any("nr=" + str(n) + " " in gap for n in [291, 332])
                                for gap in record["syscall_gaps"]) and
                            len(record["reloads"]) == 2 and all(
                                "dnf-native-reload-script-" + str(v) in stdout for v in [1, 2, 3]))
        text = json.dumps(record, indent=2) + "\n"
        (probe / "receipt.json").write_text(text, encoding="utf-8")
        (BUILD / "frida-reload-checks.json").write_text(text, encoding="utf-8")
        print("Original Frida in-place/atomic reload:", "PASS" if record["passed"] else "FAIL",
              "receipt", probe / "receipt.json", flush=True)
    if not record["passed"]:
        raise SystemExit(record.get("error", "Frida reload validation failed"))


if __name__ == "__main__":
    main()
