"""Observe the native GUI's full core startup and original game shutdown."""

import argparse
import datetime
import json
from pathlib import Path
import platform
import subprocess
import time

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from bootstrap import sha256, BUILD
from probe_core_protocols import channel, relay, stun
from test_gui import user, windows, wait_for, click, ready, responsive, screenshot, status
from test_gui_services import rows

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--instance", type=Path, required=True)
    parser.add_argument("--gui", type=Path, default=BUILD / "gui-services/DNF-Native.exe")
    parser.add_argument("--timeout", type=float, default=2100)
    parser.add_argument("--observe-seconds", type=float, default=60)
    args = parser.parse_args()
    instance, gui = args.instance.resolve(strict=True), args.gui.resolve(strict=True)
    prepared = json.loads((instance / "prepare-receipt.json").read_text(encoding="utf-8"))
    if prepared["peers_only"]:
        raise SystemExit("Prepare a default core instance without --peers-only")
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    probe = BUILD / "probes" / stamp / "gui-core"
    probe.mkdir(parents=True)
    record = {"recorded_utc": stamp, "os": platform.platform(), "scope": "GUI core startup/stop and peer protocols; no original client gameplay or security topology acceptance.",
              "gui_sha256": sha256(gui), "script_sha256": sha256(Path(__file__)),
              "prepare_receipt_sha256": sha256(instance / "prepare-receipt.json"),
              "checks": {}, "forced_cleanup": False}
    process = subprocess.Popen([str(gui), "--instance", str(instance)])
    window = None
    started = time.monotonic()
    try:
        window = wait_for(lambda: next(iter(windows(process.pid, "DNFNativeController")), None), "main window")
        click(window, 223); ready(window, "已启动默认服务进程", 90)
        record["started_rows"] = rows(window)
        assert sum("进程运行中" in row for row in rows(window)) == 16
        checks = record["checks"]
        checks["sixteen_core_processes_started_by_gui"] = True
        log_dir = instance / "i686/home/neople/game/log"
        next_update = 0
        while not any(b"server has been started successfully" in path.read_bytes() for path in log_dir.rglob("*.init")):
            elapsed = time.monotonic() - started
            if process.poll() is not None or elapsed > args.timeout:
                raise RuntimeError("GUI full core startup did not complete before timeout")
            if any("已退出" in row for row in rows(window)[:16]):
                raise RuntimeError("An original core process exited during game initialization")
            if elapsed >= next_update:
                print("GUI original game initialization:", round(elapsed, 1), "seconds; window response", responsive(window), "ms", flush=True)
                next_update = elapsed + 30
            time.sleep(1)
        checks["game_natural_startup_seconds"] = round(time.monotonic() - started, 3)
        print("GUI original game startup marker observed", flush=True)
        screenshot(window, probe / "core-running.png")
        time.sleep(args.observe_seconds)
        checks["peer_protocols_after_game_startup"] = {"channel": channel(7001), "relay": relay(True), "stun": stun()}
        checks["window_response_after_startup_ms"] = responsive(window)
        user.PostMessageW(window, 0x10, 0, 0)
        process.wait(timeout=300)
        assert process.returncode == 0
        shutdown = "\n".join(path.read_text(encoding="utf-8", errors="replace") for path in (instance / "service-logs").glob("game-*.stdout.private.log"))
        shutdown += "\n" + "\n".join(path.read_text(encoding="utf-8", errors="replace") for path in log_dir.rglob("*.log"))
        checks["game_shutdown_markers"] = [marker for marker in ["RECV SIGUSR2", "ALL USER LOGOUT COMPLETE", "GAME SERVER SHUTDOWN COMPLETE"] if marker in shutdown]
        assert len(checks["game_shutdown_markers"]) == 3
        checks["frida_dispose_and_dp2_unload"] = "dispose" in shutdown and "unload" in shutdown
        server_log = next((instance / ".mysql").glob("gui-*-server.private.log"))
        checks["database_normal_shutdown"] = b"Shutdown complete" in server_log.read_bytes()
        record["runtime_gaps"] = sorted({line.split(" pc=", 1)[0] for path in (instance / "service-logs").glob("*.stderr.private.log")
                                         for line in path.read_text(encoding="utf-8", errors="replace").splitlines()
                                         if any(marker in line for marker in ["syscall gap", "unhandled exception", "unsupported", "exhausted"])})
    except Exception as error:
        record["error"] = type(error).__name__ + ": " + str(error)
    finally:
        if process.poll() is None:
            if window:
                user.PostMessageW(window, 0x10, 0, 0)
            try:
                process.wait(timeout=300)
            except subprocess.TimeoutExpired:
                record["forced_cleanup"] = True
                subprocess.run(["taskkill.exe", "/PID", str(process.pid), "/T", "/F"], capture_output=True, timeout=15)
                process.wait(timeout=10)
        record["exit_code"] = process.returncode
        record["elapsed_seconds"] = round(time.monotonic() - started, 3)
        record["logs"] = {path.name: sha256(path) for path in sorted((instance / "service-logs").glob("*.log"))}
        (probe / "receipt.json").write_text(json.dumps(record, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")
        print("GUI core receipt:", probe / "receipt.json", flush=True)
    if record.get("error") or record["forced_cleanup"]:
        raise SystemExit(json.dumps(record.get("error", "forced cleanup"), ensure_ascii=True))


if __name__ == "__main__":
    main()
