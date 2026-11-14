"""Run a real libuv timer loop in original game/DP2/Lua/luv before resource loading."""

import argparse
import datetime
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", type=Path, default=ROOT / "build/i686/dnf-host.exe")
    args = parser.parse_args()
    command = [sys.executable, str(ROOT / "tools/probe_game.py"), "--preload", "--timeout", "60",
               "--host", str(args.host.resolve()), "--lua-script", str(ROOT / "tests/luv_timer.lua")]
    subprocess.run(command, cwd=ROOT, check=True)
    probe = json.loads((ROOT / "build/game-probe.json").read_text(encoding="utf-8"))
    instance = Path(probe["command"][2])
    stdout = (instance / "stdout.log").read_text(encoding="utf-8", errors="replace")
    stderr = (instance / "stderr.log").read_text(encoding="utf-8", errors="replace")
    markers = ["dnf-native-luv-timer-fired", "dnf-native-luv-loop-returned"]
    passed = (probe["exit_code"] == 42 and not probe["forced_cleanup"] and
              all(marker in stdout for marker in markers) and "unhandled exception" not in stderr)
    result = {key: probe[key] for key in ["os", "host_sha256", "elf_sha256", "exit_code", "forced_cleanup",
                                        "elapsed_seconds", "inputs", "syscall_gaps", "runtime_gaps"]}
    result.update({"recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                   "scope": "Own Lua timer in original game/DP2/luv; no Frida, natural game startup or business acceptance",
                   "probe": str(instance.relative_to(ROOT)), "command": command,
                   "markers": [marker for marker in markers if marker in stdout], "passed": passed,
                   "log_sha256": {name: hashlib.sha256((instance / name).read_bytes()).hexdigest()
                                  for name in ["stdout.log", "stderr.log"]}})
    (ROOT / "build/luv-checks.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print("Original DP2/luv timer:", "PASS" if passed else "FAIL", "exit", probe["exit_code"])
    if not passed:
        raise SystemExit("See build/luv-checks.json and private probe logs")


if __name__ == "__main__":
    main()
