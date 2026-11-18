"""Exercise the standalone Robot module and original markets on a cold copy."""
import argparse
import configparser
import datetime
import json
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import time
import traceback
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from bootstrap import BUILD, sha256
from runtime_support import tree_identity
from gui_support import (user, windows, wait_for, click, ready, ready_stopped,
                         rows, log_text, hidden_startup, screenshot,
                         select_service, service_row, service_details, title, open_features, close_features)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gui", type=Path, required=True)
    parser.add_argument("--instance", type=Path, required=True)
    args = parser.parse_args()
    gui, source = args.gui.resolve(strict=True), args.instance.resolve(strict=True)
    for port in (3306, 8111, 8112, 30803, 30603):
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", port))
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S")
    work = BUILD / "probes" / stamp / "gui-robot"
    instance = work / "instance"
    work.mkdir(parents=True)
    before = tree_identity(source / "data")
    print("Copying private instance:", instance, flush=True)
    # Exercise first initialization even when the source has previously run Robot.
    def ignore(directory, names):
        excluded = {".mysql", "service-logs", ".instance.lock"}
        if Path(directory) == source:
            excluded.add("robot")
        return excluded.intersection(names)

    shutil.copytree(source, instance, ignore=ignore)
    manifest = instance / "services.ini"
    ini = configparser.ConfigParser(interpolation=None)
    ini.read(manifest, encoding="utf-16")
    for key, name in ini["services"].items():
        if key.startswith("service") and name in ini:
            ini[name]["automatic"] = "0" if name in ("zergsvr", "secagent", "gunnersvr") else "1"
    ini["robot"] = {"enabled": "0"}
    ini["plugins"] = {"mode": "off"}
    with manifest.open("w", encoding="utf-16") as stream:
        ini.write(stream)
    game = instance / ini["game"]["root"] / "home/neople/game"
    private = game / "privatekey.pem"
    # Deliberately exercise preparation of only the missing game private key.
    if private.exists():
        private.rename(private.with_suffix(".fixture-backup"))
    record = {"gui_sha256": sha256(gui), "checks": {},
              "scope": "Real Win32 GUI, standalone Windows Robot, private MySQL and original market ELF services."}
    checks = record["checks"]
    process = window = feature = None
    tool = gui.parent / "resources/mysql-control.exe"

    def launch():
        nonlocal process, window
        process = subprocess.Popen([str(gui), "--instance", str(instance)], startupinfo=hidden_startup(),
                                   env=dict(__import__("os").environ, LOCALAPPDATA=str(work / "profile")))
        window = wait_for(lambda: next(iter(windows(process.pid, "DNFNativeController")), None), "controller")
        wait_for(lambda: len(rows(window)) in (19, 20), "service cards")

    def command(identifier, expected, timeout=120):
        base = len(log_text(window))
        click(window, identifier)
        ready(window, expected, timeout, since=base)

    def toggle(enabled):
        nonlocal feature
        feature = open_features(window)
        base = len(log_text(window))
        click(feature, 236)
        ready(window, "Robot 已启用" if enabled else "Robot 已停用", since=base)
        wait_for(lambda: user.IsWindowEnabled(feature), "Robot setting saved")
        saved = configparser.ConfigParser(interpolation=None)
        saved.read(manifest, encoding="utf-16")
        assert saved.getboolean("robot", "enabled") == enabled
        assert len(rows(window)) == (20 if enabled else 19)
        for identifier in (301, 302, 304, *range(310, 318)):
            assert bool(user.IsWindowEnabled(user.GetDlgItem(feature, identifier))) == (not enabled), identifier
        assert not user.IsWindowEnabled(user.GetDlgItem(feature, 234)), "Robot is independently saved"
        screenshot(feature, work / ("features-robot-on.png" if enabled else "features-robot-off.png"))
        close_features(feature)
        feature = None

    def api(name, payload=None, timeout=240):
        packet = "<tw><c>" + name + "</c><json>" + json.dumps(payload or {}) + "</json></tw>"
        with socket.create_connection(("127.0.0.1", 8111), timeout=timeout) as sock:
            sock.settimeout(timeout)
            sock.sendall(packet.encode())
            raw = b""
            while b"</tw>" not in raw:
                data = sock.recv(65536)
                assert data, "Robot closed response"
                raw += data
        response = json.loads(raw.decode().split("<result>", 1)[1].split("</result>", 1)[0])
        assert response.get("ok"), (name, response)
        (work / (name + ".private.json")).write_text(json.dumps(response, ensure_ascii=False, indent=2), encoding="utf-8")
        return response.get("result", response)

    def sql(text):
        result = subprocess.run([str(tool), "mysql", str(instance), "--defaults-file=client.private.cnf",
                                 "--batch", "--skip-column-names"], input=text.encode(), capture_output=True, timeout=60)
        assert result.returncode == 0, result.stderr.decode(errors="replace")
        return result.stdout.decode().strip()

    def wait_marker(name, old=None):
        path = instance / "robot/state/pids" / (name + ".pid")
        return wait_for(lambda: path.exists() and path.read_text() != old and path.read_text(), name + " PID marker", 60)

    try:
        launch()
        command(111, "数据库 SQL 检查通过", 90)
        # A running peer must retain its handle when the optional module is added.
        select_service(window, "bridge")
        command(221, "bridge：进程运行中", 30)
        bridge_before = service_row(window, "bridge")
        toggle(True)
        assert service_row(window, "bridge") == bridge_before
        assert private.exists()
        assert not (instance / "robot/keys/privatekey.pem").exists(), "GUI wrote Robot-owned keys"
        checks["toggle_immediately_saved_retains_peer_handles_and_only_prepares_game_keys"] = True
        select_service(window, "robot")
        command(221, "robot：进程运行中", 180)
        details = service_details(window)
        assert "Windows" in details and "robot.exe" in details and "x86_64" in details, details
        checks["robot_resource_details_in_main_log"] = True
        keys = api("keypairStatus")
        assert keys["loaded"] and keys["game_valid"] and keys["config_valid"], keys
        assert sha256(private) == sha256(instance / "robot/keys/privatekey.pem")
        # Keep this lifecycle test independent of the fresh default auto target.
        api("autoStop")
        checks["real_robot_initializes_database_pvf_and_synchronizes_game_keys"] = True
        profile = api("hostProfileStatus")
        assert profile["market_owner"] == "robot"
        assert {"game", "auction", "point", "monitor", "relay"} <= set(profile["services"])
        api("databaseStatus")
        created=api("createRobots", {"count": 1})
        assert len(created["robots"]) == 1, created
        checks["robot_creates_account_character_and_its_own_schema"] = True
        with urllib.request.urlopen("http://127.0.0.1:8112/", timeout=15) as page:
            assert b"html" in page.read().lower()
        checks["robot_api_and_web_respond"] = True
        command(223, "已启动默认服务进程", 180)
        # Isolate market replay to this copy. This test performs no user DB writes.
        for db in ("taiwan_cain_auction_gold", "taiwan_cain_auction_cera"):
            for table in ("auction_main", "auction_system_main", "auction_average_price", "auction_roi_average_price"):
                sql(f"DELETE FROM {db}.{table};")
        api("marketSyncItemInfo")
        api("marketEnsureServices")
        first = {name: wait_marker(name) for name in ("auction", "point")}
        wait_for(lambda: all("进程运行中" in service_row(window, name) for name in first), "GUI adopts Robot children", 30)
        patch = api("marketPatchAuctionMemory")
        assert len(patch["entries"]) == 6 and all(entry["ok"] for entry in patch["entries"]), patch
        checks["robot_starts_original_markets_gui_adopts_handles_memory_patches_verified"] = True
        api("marketSyncItemInfo")
        second = {name: wait_marker(name, first[name]) for name in first}
        assert all(second[name] != first[name] for name in first)
        wait_for(lambda: all("进程运行中" in service_row(window, name) for name in first), "GUI adopts restarted children", 30)
        checks["robot_rebuilds_indexes_and_restarts_both_markets"] = True
        wait_for(lambda: title(user.GetDlgItem(window, 213)).startswith("17 / 20"), "live module count includes Robot children", 15)
        checks["live_count_includes_robot_restarted_markets_excludes_web_worker"] = True
        wait_for(lambda: "应用已初始化" in service_row(window, "game"), "original game initialization", 240)
        uid = created["robots"][0]["uid"]
        online = api("robotsOnline", {"uids": [uid]})
        assert online["confirmed"] == 1, online
        status = api("robotsStatus", {"uids": [uid]})
        assert status["robots"][0]["online"], status
        def actor_idle():
            current = api("robotsStatus", {"uids": [uid]})
            item = current["robots"][0]
            return item["online"] and item.get("actor_state") in ("idle", "attached", "running")
        wait_for(actor_idle, "Robot actor becomes movable", 60)
        moved = api("robotsMove", {"uids": [uid]})
        assert moved["confirmed"] == 1, moved
        api("robotsLogout", {"uids": [uid]})
        checks["robot_real_game_login_move_and_logout"] = True
        select_service(window, "robot")
        command(222, "robot：已停止", 60)
        command(221, "robot：进程运行中", 180)
        assert api("keypairStatus")["loaded"]
        checks["robot_normal_stop_restart"] = True
        screenshot(window, work / "controller-robot.png")
        command(224, "服务进程已退出，数据库已停止", 120)
        user.PostMessageW(window, 0x10, 0, 0)
        process.wait(timeout=30)
        process = None
        launch()
        assert len(rows(window)) == 20
        command(111, "数据库 SQL 检查通过", 90)
        toggle(False)
        checks["reopen_preserves_module_and_disabling_restores_native_controls"] = True
        command(224, "服务进程已退出，数据库已停止", 90)
        user.PostMessageW(window, 0x10, 0, 0)
        process.wait(timeout=30)
        process = None
        record["passed"] = True
    except Exception:
        record["passed"] = False
        record["error"] = traceback.format_exc()
    finally:
        if process:
            if feature and user.IsWindow(feature):
                user.PostMessageW(feature, 0x10, 0, 0)
            if window:
                user.PostMessageW(window, 0x10, 0, 0)
            try:
                process.wait(timeout=60)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(timeout=10)
                # This is our isolated copy, authenticated by its private
                # maintenance config; never leave its database after failure.
                subprocess.run([str(tool), "mysql", str(instance), "--defaults-file=client.private.cnf",
                                "--execute=SHUTDOWN;"], capture_output=True, timeout=20)
        checks["source_database_unchanged"] = tree_identity(source / "data") == before
        record["passed"] = record.get("passed", False) and checks["source_database_unchanged"]
        (work / "receipt.json").write_text(json.dumps(record, ensure_ascii=False, indent=2), encoding="utf-8")
        print("GUI Robot:", "PASS" if record["passed"] else "FAIL", work / "receipt.json", flush=True)
        if not record["passed"]:
            print(record.get("error", "source changed"), flush=True)
    if not record["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
