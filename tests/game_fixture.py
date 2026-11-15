"""Run an explicit Lua/Frida fixture in a private copy of the pinned game."""

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
from runtime_support import unpack_runtime


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout", type=float, default=90)
    parser.add_argument("--host", type=Path, default=BUILD / "i686/dnf-host.exe")
    parser.add_argument("--lua-script", type=Path, required=True)
    parser.add_argument("--frida-script", type=Path)
    parser.add_argument("--env", action="append", default=[])
    args = parser.parse_args()
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    instance = BUILD / "probes" / stamp / "game-fixture"
    unpack_runtime(instance, "i686", ("lib/", "usr/", "home/neople/game/", "dp2/"))
    guest = instance / "home/neople/game"
    for name in ("log", "pid", "history"):
        (guest / name).mkdir(exist_ok=True)
    inputs = [guest / "df_game_r", instance / "dp2/libdp2pre.so",
              instance / "dp2/libdp2.so", instance / "dp2/frida/frida.so"]
    for source, name in [(args.lua_script, "df_game_r.lua"), (args.frida_script, "df_game_r.js")]:
        if source:
            target = instance / "dp2" / name
            target.write_bytes(source.read_bytes())
            inputs.append(target)
    elf, ld = guest / "df_game_r", instance / "lib/ld-2.17.so"
    command = [str(args.host.resolve(strict=True)), "--root", str(instance), sha256(ld),
               "--env", "HOME=/home/neople", "--env", "LD_PRELOAD=/dp2/libdp2pre.so",
               "--env", "LD_LIBRARY_PATH=/dp2/lib:/home/neople/game",
               *[part for value in args.env for part in ("--env", value)],
               "--cwd", "/home/neople/game", "--run", str(elf), sha256(elf),
               "/home/neople/game/df_game_r", "siroco11", "nofork"]
    started = time.monotonic()
    forced = False
    with (instance / "stdout.log").open("wb") as out, (instance / "stderr.log").open("wb") as err:
        process = subprocess.Popen(command, stdout=out, stderr=err, env=dict(os.environ))
        try:
            process.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            forced = True
            process.terminate()
            process.wait(timeout=10)
    stderr = (instance / "stderr.log").read_text(encoding="utf-8", errors="replace")
    receipt = {
        "recorded_utc": stamp, "os": platform.platform(), "command": command,
        "scope": "Own explicit script in the pinned game/DP2; expected script exit 42, not natural startup or business acceptance.",
        "host_sha256": sha256(Path(command[0])), "elf_sha256": sha256(elf),
        "exit_code": process.returncode, "forced_cleanup": forced,
        "elapsed_seconds": round(time.monotonic() - started, 3),
        "inputs": {p.relative_to(instance).as_posix(): sha256(p) for p in inputs},
        "syscall_gaps": sorted({line for line in stderr.splitlines() if "syscall gap" in line}),
        "runtime_gaps": sorted({line for line in stderr.splitlines() if any(
            word in line for word in ("unhandled exception", "unsupported", "decode gap", "exhausted"))}),
    }
    text = json.dumps(receipt, indent=2) + "\n"
    (instance / "receipt.json").write_text(text, encoding="utf-8")
    (BUILD / "game-probe.json").write_text(text, encoding="utf-8")
    print("Game fixture:", process.returncode, "forced cleanup", forced,
          "syscall diagnostics", len(receipt["syscall_gaps"]), "logs:", instance, flush=True)
    if forced or process.returncode != 42 or receipt["runtime_gaps"]:
        raise SystemExit("Game fixture failed; see private logs")


if __name__ == "__main__":
    main()
