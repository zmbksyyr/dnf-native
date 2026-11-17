"""Verify original game initialization updates the GUI for each fresh launch."""
import argparse
import datetime
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from bootstrap import BUILD, sha256
from gui_support import (user, windows, wait_for, click, ready, ready_stopped, title, log_text,
                         hidden_startup, select_service, service_row, screenshot, responsive, service_details)
from runtime_support import runtime_diagnostics, tree_identity

MARKER = b" server has been started successfully."


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--instance", type=Path, required=True)
    parser.add_argument("--gui", type=Path, required=True)
    parser.add_argument("--pvf", type=Path, help="Read-only PVF input, copied into the private instance")
    parser.add_argument("--script-encoding", choices=("GBK", "BIG5"), help="Explicit encoding of this PVF")
    args = parser.parse_args()
    source, gui = args.instance.resolve(strict=True), args.gui.resolve(strict=True)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    probe = BUILD / "probes" / stamp / "gui-readiness"
    instance = probe / "instance"
    source_data = tree_identity(source / "data")
    shutil.copytree(source, instance, ignore=shutil.ignore_patterns(".mysql", "service-logs", ".instance.lock"))
    import configparser
    manifest = configparser.ConfigParser(interpolation=None)
    manifest.read(instance / "services.ini", encoding="utf-16")
    manifest["game"]["automatic"] = "0"
    if args.script_encoding:
        environment = [value for key, value in manifest["game"].items() if key.startswith("env")]
        environment = [value for value in environment if not value.startswith("DNF_NATIVE_SCRIPT_ENCODING=")]
        for key in list(manifest["game"]):
            if key.startswith("env"):
                del manifest["game"][key]
        environment.append("DNF_NATIVE_SCRIPT_ENCODING=" + args.script_encoding)
        for index, value in enumerate(environment):
            manifest["game"]["env" + str(index)] = value
    with (instance / "services.ini").open("w", encoding="utf-16") as stream:
        manifest.write(stream)
    root = instance / manifest["game"]["root"]
    pvf_input = args.pvf.resolve(strict=True) if args.pvf else root / "home/neople/game/Script.pvf"
    pvf_hash = sha256(pvf_input)
    if args.pvf:
        shutil.copyfile(pvf_input, root / "home/neople/game/Script.pvf")
    old_logs = [p for p in (root / "home/neople/game/log").rglob("*.init") if MARKER in p.read_bytes()]
    assert old_logs, "Use a stopped instance with a previous successful initialization log"
    record = {"recorded_utc": stamp, "os": platform.platform(), "source_instance": str(source),
              "gui_sha256": sha256(gui), "script_sha256": sha256(Path(__file__)),
              "scope": "GUI application initialization from original game stdout; no client or business acceptance",
              "pvf_sha256": pvf_hash,
              "script_encoding_override": args.script_encoding,
              "checks": {}, "launches": [], "forced_cleanup": False}
    checks = record["checks"]
    checks["old_successful_application_logs_retained"] = len(old_logs)
    resources = {p.name: sha256(p) for p in (gui.parent / "resources").iterdir() if p.is_file()}
    process = subprocess.Popen([str(gui), "--instance", str(instance)], startupinfo=hidden_startup())
    window = None
    try:
        window = wait_for(lambda: next(iter(windows(process.pid, "DNFNativeController")), None), "controller")
        select_service(window, "game")
        assert "应用已初始化" not in log_text(window)
        click(window, 223)
        ready(window, "已启动默认服务进程", 90)
        for launch in [1, 2]:
            select_service(window, "game")
            previous_outputs = set((instance / "service-logs").glob("game-*.stdout.private.log"))
            registration = {}
            for peer in ("monitor", "guild"):
                folder = instance / manifest[peer]["root"] / manifest[peer]["cwd"].lstrip("/") / "log"
                registration[peer] = (folder, {p: p.stat().st_size for p in folder.glob("GameServer*.log")})
            started = time.monotonic()
            base = len(log_text(window))
            click(window, 221)
            ready(window, "game：进程运行中", 30)
            wait_for(lambda: user.IsWindowEnabled(user.GetDlgItem(window, 222)), "game start operation complete")
            wait_for(lambda: "等待应用初始化" in log_text(window)[base:], "pending readiness recorded", 15)
            pending = log_text(window)[base:]
            assert "等待应用初始化" in pending and "应用已初始化" not in pending, pending
            assert "应用已初始化" not in service_row(window, "game")
            item = {"launch": launch, "pending_readiness": pending, "window_response_ms": responsive(window)}
            screenshot(window, probe / ("launch-" + str(launch) + "-pending.png"))
            wait_for(lambda: "应用已初始化" in log_text(window)[base:], "original game initialization in GUI", 180)
            item["application_initialized_seconds"] = round(time.monotonic() - started, 3)
            outputs = set((instance / "service-logs").glob("game-*.stdout.private.log")) - previous_outputs
            assert len(outputs) == 1
            output = outputs.pop()
            assert MARKER in output.read_bytes()
            diagnostic = output.with_name(output.name.replace(".stdout.", ".stderr.")).read_bytes()
            assert b"iconv error" not in output.read_bytes() + diagnostic, "PVF encoding conversion failed"
            item["script_encoding_selection"] = [line for line in diagnostic.decode("utf-8", "replace").splitlines()
                                                 if line.startswith("pvf script encoding source=")]
            assert len(item["script_encoding_selection"]) == 1, "Missing original script codepage selection"
            assert ("source=explicit" if args.script_encoding else "source=auto") in item["script_encoding_selection"][0]
            assert "应用已初始化" in service_row(window, "game")
            assert "应用已初始化" in log_text(window)[base:]
            details = service_details(window)
            assert "接口/协议/业务：未验证" in details and "清单未声明" in details, details
            item["fresh_stdout"] = output.name
            item["fresh_stdout_sha256_at_initialization"] = sha256(output)
            item["initialized_readiness"] = log_text(window)[base:][-600:]
            # Monitor registration ACKs follow the initialization marker. A
            # stale TCP peer can reject a restart and request normal shutdown
            # just after that marker; observe the original registration ACKs.
            # Original stdout is buffered; use each peer's fresh application
            # log, never an old registration from the copied instance.
            def registered():
                for folder, offsets in registration.values():
                    fresh = b"\n".join(p.read_bytes()[offsets.get(p, 0):] for p in folder.glob("GameServer*.log"))
                    assert b"regist failed" not in fresh, "original peer rejected game registration"
                    if b"Game server regist success. Channel:" not in fresh:
                        return False
                return True

            wait_for(registered, "monitor and guild registration", 30)
            text = output.read_bytes()
            assert b"main() exited OK" not in text, "game exited after registration"
            assert b"Channel NO 11 is already registerd" not in text
            item["original_monitor_and_guild_registration_accepted"] = True
            screenshot(window, probe / ("launch-" + str(launch) + "-initialized.png"))
            select_service(window, "channel")
            # channel only exposes the runtime channel, never application init;
            # read the real detail dialog because that level is not a log event.
            channel_details = service_details(window)
            assert "运行时通道已接入" in channel_details, channel_details
            assert "应用已初始化" not in channel_details, channel_details
            select_service(window, "game")
            assert "应用已初始化" in log_text(window)[base:]
            if launch == 1:
                click(window, 222)
                ready_stopped(window, "game", 180)
                wait_for(lambda: user.IsWindowEnabled(user.GetDlgItem(window, 221)), "game stop operation complete")
                wait_for(lambda: "宿主已退出" in log_text(window)[base:], "host exit readiness recorded", 15)
                assert b"main() exited OK" in output.read_bytes()
                checks["first_application_normal_exit"] = True
            record["launches"].append(item)
            print("GUI original game initialization", launch, "PASS", item["application_initialized_seconds"], flush=True)
        checks["per_launch_pending_then_application_initialized"] = True
        checks["old_init_and_previous_stdout_do_not_mark_new_launch_ready"] = True
        checks["channel_readiness_and_business_boundary_preserved"] = True
        click(window, 224)
        ready(window, "服务进程已退出，数据库已停止", 180)
        assert "宿主已退出" in log_text(window)
        outputs = list((instance / "service-logs").glob("game-*.stdout.private.log"))
        assert len(outputs) == 2 and all(b"main() exited OK" in p.read_bytes() for p in outputs)
        checks["both_original_game_runs_normal_exit"] = True
        user.PostMessageW(window, 0x10, 0, 0)
        process.wait(timeout=30)
    except Exception as error:
        record["error"] = type(error).__name__ + ": " + str(error)
    finally:
        if process.poll() is None:
            if window:
                user.PostMessageW(window, 0x10, 0, 0)
            try:
                process.wait(timeout=240)
            except subprocess.TimeoutExpired:
                record["forced_cleanup"] = True
                subprocess.run(["taskkill.exe", "/PID", str(process.pid), "/T", "/F"], capture_output=True)
                process.wait(timeout=10)
        record["exit_code"] = process.returncode
        record["diagnostics"] = runtime_diagnostics(instance)
        checks["source_data_unchanged"] = tree_identity(source / "data") == source_data
        checks["pvf_input_and_private_copy_unchanged"] = (
            sha256(pvf_input) == pvf_hash and sha256(root / "home/neople/game/Script.pvf") == pvf_hash)
        checks["package_resources_unchanged"] = resources == {
            p.name: sha256(p) for p in (gui.parent / "resources").iterdir() if p.is_file()}
        gaps = record["diagnostics"]["gaps"]
        record["passed"] = (not record.get("error") and not record["forced_cleanup"] and
                            process.returncode == 0 and not record["diagnostics"]["faults"] and
                            all(any("nr=" + str(n) + " " in gap for n in [291,332]) for gap in gaps) and
                            len(record["launches"]) == 2 and all(checks.values()))
        text = json.dumps(record, indent=2) + "\n"
        (probe / "receipt.json").write_text(text, encoding="utf-8")
        (BUILD / "gui-readiness-checks.json").write_text(text, encoding="utf-8")
        print("GUI game readiness:", "PASS" if record["passed"] else "FAIL", "receipt", probe / "receipt.json", flush=True)
    if not record["passed"]:
        raise SystemExit(record.get("error", "GUI readiness validation failed"))


if __name__ == "__main__":
    main()
