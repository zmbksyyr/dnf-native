"""Exercise native offline logical restore and recovery on a private original-data copy."""

import argparse
import ctypes
import datetime
import json
import os
from pathlib import Path
import platform
import secrets
import shutil
import subprocess
import time

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from bootstrap import sha256, BUILD
from runtime_support import tree_identity

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--database-source", type=Path, required=True, help="Stopped private secure test instance")
    args = parser.parse_args()
    exe, source = args.exe.resolve(strict=True), args.database_source.resolve(strict=True)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    probe = BUILD / "probes" / stamp / "mysql-maintenance"
    instance = probe / "中文实例 (native)"
    instance.mkdir(parents=True)
    before = tree_identity(source / "data")
    shutil.copytree(source / "data", instance / "data")
    shutil.copytree(source / "mysql", instance / "mysql")
    for name in ["client.private.cnf", "server.cnf"]:
        shutil.copy2(source / name, instance / name)
    env = dict(os.environ)
    env["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32")
    for name in ["MYSQL_HOME", "MYSQL_PWD", "MYSQL_TCP_PORT"]:
        env.pop(name, None)
    receipt = {"recorded_utc": stamp, "os": platform.platform(), "probe": str(probe.relative_to(BUILD)),
               "scope": "Native stopped-instance backup, shadow logical restore, failure preservation and journal recovery.",
               "exe_sha256": sha256(exe), "script_sha256": sha256(Path(__file__)),
               "sources": {name: sha256(ROOT / name) for name in
                           ["src/database/mysql_control.cpp", "src/database/mysql_operations.h", "tools/build_mysql.py"]},
               "checks": {}, "commands": [], "forced_cleanup": False}
    checks = receipt["checks"]
    index = 0
    server = None
    options = []

    def invoke(operation, arguments=(), data=None, check=True, target=instance):
        nonlocal index
        result = subprocess.run([str(exe), operation, str(target), *map(str, arguments)], input=data,
                                capture_output=True, env=env, timeout=600)
        index += 1
        (probe / f"command-{index}.private.log").write_bytes(result.stdout + result.stderr)
        receipt["commands"].append({"operation": operation, "exit_code": result.returncode,
                                    "log": f"command-{index}.private.log"})
        if check and result.returncode:
            raise RuntimeError(f"Native {operation} failed; see private command {index}")
        return result

    def running_sql(statements, expected_output=None):
        nonlocal server, options
        pipe = "DNFMaintenanceTest" + secrets.token_hex(8)
        options = ["--defaults-file=client.private.cnf", "--protocol=PIPE", "--socket=" + pipe,
                   "--ssl-mode=DISABLED", "--comments", "--binary-mode", "--default-character-set=latin1"]
        pid_file = instance / "probe.pid"
        with (probe / f"server-{index}.private.log").open("wb") as log:
            server = subprocess.Popen([str(exe), "mysqld", str(instance), "--defaults-file=server.cnf",
                                       "--skip-networking", "--enable-named-pipe", "--socket=" + pipe,
                                       "--pid-file=" + str(pid_file), "--console"],
                                      stdout=log, stderr=log, env=env)
        deadline = time.monotonic() + 45
        while invoke("mysql", options, b"SELECT 1;", check=False).returncode:
            if server.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError("Private mutation server did not become ready")
            time.sleep(0.1)
        child_pid = int(pid_file.read_text(encoding="ascii").strip())
        busy = invoke("backup", [probe / "must-not-backup-running"], check=False)
        if busy.returncode == 0:
            raise RuntimeError("Backup accepted a running instance")
        checks["running_instance_refused"] = True
        result = invoke("mysql", options, statements)
        stopped = invoke("mysql", options, b"SHUTDOWN;\n", check=False)
        server.wait(timeout=45)
        if server.returncode or (stopped.returncode and b"ERROR 2013 (HY000)" not in stopped.stderr):
            raise RuntimeError("Private mutation server shutdown failed")
        server = None
        if expected_output is not None and expected_output not in result.stdout.replace(b"\r\n", b"\n"):
            raise RuntimeError("Stored text bytes differ; see the private SQL result")
        return child_pid

    # Verify stored bytes directly: comparing two dumps can hide the same lossy
    # text conversion in both exports. Include every latin1 byte and UTF-8 text
    # beyond latin1, including a supplementary character.
    legacy_hex = bytes(range(256)).hex().upper().encode("ascii")
    unicode_hex = "中文角色😀".encode("utf-8").hex().upper().encode("ascii")
    text_query = b"SELECT CONCAT(HEX(legacy_text),':',HEX(unicode_text)) FROM native_restore_probe.text_row;\n"
    expected_text = legacy_hex + b":" + unicode_hex + b"\n"
    try:
        running_sql(b"CREATE DATABASE native_restore_probe;\n"
                    b"CREATE TABLE native_restore_probe.innodb_row(id INT PRIMARY KEY,body VARBINARY(32)) ENGINE=InnoDB;\n"
                    b"CREATE TABLE native_restore_probe.myisam_row(id INT PRIMARY KEY,body VARBINARY(32)) ENGINE=MyISAM;\n"
                    b"INSERT INTO native_restore_probe.innodb_row VALUES(1,0x000D0AFF);\n"
                    b"INSERT INTO native_restore_probe.myisam_row VALUES(2,0xE4B8ADE69687);\n"
                    b"DELIMITER $$\r\nCREATE PROCEDURE native_restore_probe.native_crlf()\r\nBEGIN\r\nSELECT 1;\r\nEND$$\r\nDELIMITER ;\r\n"
                    b"CREATE EVENT native_restore_probe.native_event ON SCHEDULE EVERY 1 DAY DISABLE DO SELECT 1;\n"
                    b"CREATE TRIGGER native_restore_probe.native_trigger BEFORE INSERT ON native_restore_probe.myisam_row FOR EACH ROW SET NEW.body=0xAA;\n"
                    b"CREATE TABLE native_restore_probe.text_row(id INT PRIMARY KEY,legacy_text VARCHAR(256) CHARACTER SET latin1,unicode_text VARCHAR(128) CHARACTER SET utf8mb4) ENGINE=InnoDB;\n"
                    b"INSERT INTO native_restore_probe.text_row VALUES(1,0x" + legacy_hex + b",0x" + unicode_hex + b");\n")
        running_sql(text_query, expected_text)
        first = probe / "完整备份 (first)"
        invoke("backup", [first])
        first_hash = sha256(first / "logical.sql")
        checks["native_full_logical_backup"] = True
        receipt["backup_bytes"] = (first / "logical.sql").stat().st_size
        receipt["backup_sha256"] = first_hash
        running_sql(b"UPDATE native_restore_probe.innodb_row SET body=0xAA;\n"
                    b"DELETE FROM native_restore_probe.myisam_row;\n"
                    b"CREATE DATABASE added_after_backup; CREATE TABLE added_after_backup.extra(id INT);\n"
                    b"CREATE USER 'native_after_backup'@'localhost' ACCOUNT LOCK;\n")
        mutated = probe / "mutated-backup"
        invoke("backup", [mutated])
        mutated_hash = sha256(mutated / "logical.sql")
        if first_hash == mutated_hash:
            raise RuntimeError("Test did not change the live data")
        corrupt = probe / "corrupt-backup"
        shutil.copytree(first, corrupt)
        with (corrupt / "logical.sql").open("ab") as stream:
            stream.write(b"\nINVALID RESTORE SQL;\n")
        physical_before = tree_identity(instance / "data")
        failed = invoke("restore", [corrupt], check=False)
        if failed.returncode == 0 or b"Backup hash" not in failed.stderr or tree_identity(instance / "data") != physical_before:
            raise RuntimeError("Corrupt backup was not refused before touching the active data")
        checks["corrupt_backup_refused_without_data_writes"] = True
        manifest = (corrupt / "manifest.txt").read_text(encoding="ascii").splitlines()
        manifest[-1] = sha256(corrupt / "logical.sql")
        (corrupt / "manifest.txt").write_text("\n".join(manifest) + "\n", encoding="ascii", newline="\n")
        failed = invoke("restore", [corrupt], check=False)
        if failed.returncode == 0 or b"Shadow logical restore failed" not in failed.stderr:
            raise RuntimeError("Invalid SQL did not fail in the shadow database")
        if not any(b"ERROR 1064" in path.read_bytes() and b"INVALID RESTORE SQL" in path.read_bytes()
                   for path in (instance / ".mysql").glob("restore-*/import/client-*.private.log")):
            raise RuntimeError("Restore failed before reaching the deliberately invalid SQL")
        preserved = probe / "after-failed-import"
        invoke("backup", [preserved])
        if sha256(preserved / "logical.sql") != mutated_hash:
            raise RuntimeError("Failed logical restore changed the active database")
        checks["failed_sql_import_preserved_active_database"] = True
        invoke("restore", [first])
        restored = probe / "after-native-restore"
        invoke("backup", [restored])
        if sha256(restored / "logical.sql") != first_hash:
            raise RuntimeError("Restored database backup differs from the selected backup")
        running_sql(text_query, expected_text)
        checks["latin1_and_unicode_stored_text_bytes_preserved"] = True
        checks["native_shadow_restore_byte_exact"] = True
        checks["innodb_myisam_binary_rows_and_extra_schema_removal"] = True
        checks["accounts_routines_events_and_triggers_restored"] = True
        snapshots = list((instance / ".mysql").glob("restore-*/previous-data"))
        if len(snapshots) != 1 or not (snapshots[0].parent / "before.sql").is_file():
            raise RuntimeError("Successful restore did not retain the cold snapshot and logical prebackup")
        receipt["cold_snapshot"] = str(snapshots[0].relative_to(probe))
        checks["cold_snapshot_and_logical_prebackup_retained"] = True

        # Denying directory rename is a real Windows sharing failure. It lets
        # the old datadir move to its snapshot, then rejects candidate activation
        # and exercises the controller's own automatic rollback catch path.
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p,
                                      ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p]
        kernel.CreateFileW.restype = ctypes.c_void_p
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        existing = set((instance / ".mysql").glob("restore-*"))
        with (probe / "activation-failure.private.log").open("wb") as log:
            server = subprocess.Popen([str(exe), "restore", str(instance), str(first)],
                                      stdout=log, stderr=log, env=env)
        locked = None
        deadline = time.monotonic() + 120
        while not locked:
            if server.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError("Could not reach the activation sharing-failure window")
            for operation in set((instance / ".mysql").glob("restore-*")) - existing:
                candidate = operation / "shadow/data"
                handle = kernel.CreateFileW(str(candidate), 0x80000000, 3, None, 3, 0x02000000, None)
                if handle != ctypes.c_void_p(-1).value:
                    locked = handle
                    break
            if not locked:
                time.sleep(0.02)
        try:
            server.wait(timeout=300)
        finally:
            kernel.CloseHandle(locked)
        activation_log = (probe / "activation-failure.private.log").read_bytes()
        if (server.returncode == 0 or b"Activate verified logical restore" not in activation_log or
                b"Previous cold database restored" not in activation_log or (instance / "restore.pending").exists()):
            raise RuntimeError("Failed candidate activation did not automatically roll back")
        server = None
        rollback_backup = probe / "after-automatic-rollback"
        invoke("backup", [rollback_backup])
        if sha256(rollback_backup / "logical.sql") != first_hash:
            raise RuntimeError("Automatic rollback did not preserve the original database")
        checks["automatic_rollback_after_original_datadir_rename"] = True

        # Build the three actual rename-window states from stopped real data.
        # This exercises the normal recovery entry point without a production
        # failure-injection switch or pretending that process termination ran.
        recovery_states = []
        for phase in ["before_rename", "between_renames", "after_activation"]:
            target = probe / ("recovery-" + phase)
            target.mkdir()
            shutil.copytree(instance / "data", target / "data")
            for name in ["server.cnf", "client.private.cnf"]:
                shutil.copy2(instance / name, target / name)
            previous_hash = tree_identity(target / "data")
            leaf = "restore-" + secrets.token_hex(24)
            work = target / ".mysql" / leaf
            work.mkdir(parents=True)
            if phase != "before_rename":
                (target / "data").rename(work / "previous-data")
            if phase == "after_activation":
                shutil.copytree(snapshots[0], target / "data")
            (target / "restore.pending").write_text(leaf, encoding="ascii")
            blocked = invoke("mysqld", ["--version"], check=False, target=target)
            if blocked.returncode == 0 or b"requires recover" not in blocked.stderr:
                raise RuntimeError("Pending restore did not block server start")
            invoke("recover", target=target)
            if (target / "restore.pending").exists() or tree_identity(target / "data") != previous_hash:
                raise RuntimeError("Recovery did not restore the previous exact cold data")
            recovery_states.append(phase)
        checks["rename_window_recovery"] = recovery_states

        # Abruptly close this wrapper and verify its owned official server also
        # exits. Retain an actual process handle so PID reuse cannot affect it.
        pipe = "DNFJobTest" + secrets.token_hex(8)
        pid_file = instance / "job-test.pid"
        with (probe / "job-server.private.log").open("wb") as log:
            server = subprocess.Popen([str(exe), "mysqld", str(instance), "--defaults-file=server.cnf", "--skip-networking",
                                       "--enable-named-pipe", "--socket=" + pipe, "--console", "--pid-file=" + str(pid_file)],
                                      stdout=log, stderr=log, env=env)
        deadline = time.monotonic() + 45
        while not pid_file.exists():
            if server.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError("Job lifecycle test server did not create its PID file")
            time.sleep(0.05)
        kernel.OpenProcess.restype = ctypes.c_void_p
        kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        owned = kernel.OpenProcess(0x100000, False, int(pid_file.read_text(encoding="ascii").strip()))
        if not owned:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            server.kill()
            server.wait(timeout=10)
            if kernel.WaitForSingleObject(owned, 10000) != 0:
                raise RuntimeError("Official server survived abrupt wrapper termination")
        finally:
            kernel.CloseHandle(owned)
        server = None
        checks["owned_server_exits_when_wrapper_is_terminated"] = True
    except Exception as error:
        receipt["error"] = str(error)
    finally:
        if server and server.poll() is None:
            receipt["forced_cleanup"] = True
            server.kill()
            server.wait(timeout=10)
        receipt["source_data_unchanged"] = before == tree_identity(source / "data")
        receipt["limits"] = ["Developer Windows 10 only; no clean ordinary-user Win7/10/11 acceptance.",
                             "Native maintenance requires a stopped instance and unchanged backup-time client/server configuration.",
                             "Rename-window recovery uses constructed on-disk states; no power-loss or disk-full injection.",
                             "Abrupt wrapper termination is real and deliberately kills its owned MySQL child; subsequent crash recovery is not a normal shutdown claim.",
                             "DNF shutdown coordination, GUI wiring, credential migration and cross-instance restore remain pending."]
        (probe / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(receipt, ensure_ascii=True))
    if receipt.get("error") or receipt["forced_cleanup"] or not receipt["source_data_unchanged"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
