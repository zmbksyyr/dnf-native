"""Check the original DP2/Frida bridge and a real fixed-address game hook."""

import argparse
import datetime
import hashlib
import json
from pathlib import Path
import platform
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from bootstrap import BUILD

ROOT = Path(__file__).resolve().parents[1]
GAME_SHA256 = "da417c5e65918f655afcddb6edfdaf2fa1e77b2f208e5aa397f021fa23b79979"
MARKERS = ["dnf-native-original-game-hook-ok",
           "dnf-native-js-lua-roundtrip-ok calls=32", "dnf-native-lua-js-roundtrip-ok calls=32"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--item-reset", action="store_true", help="Also check native item reset and original cache construction")
    args = parser.parse_args()
    script = ROOT / "tests/frida_bridge.js"
    markers = list(MARKERS)
    sources = ["tests/test_frida.py", "tests/frida_bridge.js", "tests/frida_bridge.lua"]
    if args.item_reset:
        sources += ["src/runtime/frida_item_reset.js", "tests/frida_item_reset.js"]
        text = script.read_text(encoding="utf-8").replace("checkGameHook();", "checkGameHook(); checkItemReset();")
        script = BUILD / "frida-item-reset.generated.js"
        script.parent.mkdir(exist_ok=True)
        script.write_text((ROOT / sources[-2]).read_text(encoding="utf-8") + "\n" +
                          (ROOT / sources[-1]).read_text(encoding="utf-8") + "\n" + text, encoding="utf-8")
        markers += ["dnf-native-cache1000-constructed milliseconds=", "dnf-native-reset61-boundaries-and-restore-ok"]
    command = [sys.executable, "-B", str(ROOT / "tests/game_fixture.py"), "--timeout", "90",
               "--lua-script", str(ROOT / "tests/frida_bridge.lua"),
               "--frida-script", str(script)]
    subprocess.run(command, cwd=ROOT, check=True)
    probe = json.loads((BUILD / "game-probe.json").read_text(encoding="utf-8"))
    instance = Path(probe["command"][2])
    stdout = (instance / "stdout.log").read_text(encoding="utf-8", errors="replace")
    stderr = (instance / "stderr.log").read_text(encoding="utf-8", errors="replace")
    hook = re.search(r"dnf-native-original-game-hook-ok original=(-?\d+) hooked=(-?\d+) restored=(-?\d+)", stdout)
    passed = (probe["elf_sha256"] == GAME_SHA256 and probe["exit_code"] == 42 and not probe["forced_cleanup"] and
              all(marker in stdout for marker in markers) and hook and hook[1] == hook[3] and hook[1] != hook[2] and
              "unhandled exception" not in stderr)
    result = {"recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "os": platform.platform(), "command": command,
              "scope": "Own bridge scripts in original game/DP2/Lua/Frida",
              "probe": str(instance.relative_to(BUILD)), "host_sha256": probe["host_sha256"],
              "elf_sha256": probe["elf_sha256"], "exit_code": probe["exit_code"],
              "forced_cleanup": probe["forced_cleanup"], "elapsed_seconds": probe["elapsed_seconds"],
              "markers": [marker for marker in markers if marker in stdout],
              "sources": {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in sources},
              "item_reset": args.item_reset,
              "cache1000_milliseconds": int(re.search(r"dnf-native-cache1000-constructed milliseconds=(\d+)", stdout)[1])
                  if args.item_reset and "dnf-native-cache1000-constructed milliseconds=" in stdout else None,
              "game_hook": dict(zip(["original", "hooked", "restored"], map(int, hook.groups()))) if hook else None,
              "inputs": {k: v for k, v in probe["inputs"].items() if "cfg" not in Path(k).parts},
              "logs": {name: hashlib.sha256((instance / name).read_bytes()).hexdigest()
                       for name in ["stdout.log", "stderr.log"]},
              "syscall_gaps": probe["syscall_gaps"], "runtime_gaps": probe["runtime_gaps"],
              "passed": bool(passed)}
    (BUILD / "frida-checks.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print("Original DP2/Frida bridge and game hook:", "PASS" if passed else "FAIL", "exit", probe["exit_code"])
    if not passed:
        raise SystemExit("Bridge check failed; see private probe logs")


if __name__ == "__main__":
    main()
