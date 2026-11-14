"""Check the original DP2/Frida bridge and a real fixed-address game hook."""

import datetime
import hashlib
import json
from pathlib import Path
import platform
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
GAME_SHA256 = "da417c5e65918f655afcddb6edfdaf2fa1e77b2f208e5aa397f021fa23b79979"
MARKERS = ["dnf-native-original-game-hook-ok",
           "dnf-native-js-lua-roundtrip-ok calls=32", "dnf-native-lua-js-roundtrip-ok calls=32"]


def main():
    game = ROOT / "originals/dp123/home/neople/game/df_game_r"
    if hashlib.sha256(game.read_bytes()).hexdigest() != GAME_SHA256:
        raise SystemExit("Bridge fixture requires the pinned original game ELF")
    command = [sys.executable, str(ROOT / "tools/probe_game.py"), "--preload", "--timeout", "90",
               "--lua-script", str(ROOT / "tests/frida_bridge.lua"),
               "--frida-script", str(ROOT / "tests/frida_bridge.js")]
    subprocess.run(command, cwd=ROOT, check=True)
    probe = json.loads((ROOT / "build/game-probe.json").read_text(encoding="utf-8"))
    instance = Path(probe["command"][2])
    stdout = (instance / "stdout.log").read_text(encoding="utf-8", errors="replace")
    stderr = (instance / "stderr.log").read_text(encoding="utf-8", errors="replace")
    hook = re.search(r"dnf-native-original-game-hook-ok original=(-?\d+) hooked=(-?\d+) restored=(-?\d+)", stdout)
    passed = (probe["exit_code"] == 42 and not probe["forced_cleanup"] and
              all(marker in stdout for marker in MARKERS) and hook and hook[1] == hook[3] and hook[1] != hook[2] and
              "unhandled exception" not in stderr)
    result = {"recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "os": platform.platform(), "command": command,
              "scope": "Own bridge scripts in original game/DP2/Lua/Frida; no database, gameplay or full plugin acceptance",
              "probe": str(instance.relative_to(ROOT)), "host_sha256": probe["host_sha256"],
              "elf_sha256": probe["elf_sha256"], "exit_code": probe["exit_code"],
              "forced_cleanup": probe["forced_cleanup"], "elapsed_seconds": probe["elapsed_seconds"],
              "markers": [marker for marker in MARKERS if marker in stdout],
              "game_hook": dict(zip(["original", "hooked", "restored"], map(int, hook.groups()))) if hook else None,
              "inputs": {k: v for k, v in probe["inputs"].items() if "cfg" not in Path(k).parts},
              "logs": {name: hashlib.sha256((instance / name).read_bytes()).hexdigest()
                       for name in ["stdout.log", "stderr.log"]},
              "syscall_gaps": probe["syscall_gaps"], "runtime_gaps": probe["runtime_gaps"],
              "passed": bool(passed)}
    (ROOT / "build/frida-checks.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print("Original DP2/Frida bridge and game hook:", "PASS" if passed else "FAIL", "exit", probe["exit_code"])
    if not passed:
        raise SystemExit("Bridge check failed; see private probe logs")


if __name__ == "__main__":
    main()
