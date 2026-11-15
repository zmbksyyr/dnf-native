"""Recover original data in private MySQL 5.7.44 copies and verify a logical roundtrip."""

from contextlib import contextmanager
import datetime
import hashlib
import json
from pathlib import Path
import platform
import secrets
import shutil
import subprocess
import time

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from bootstrap import LOCAL, BUILD, ORIGINALS

ROOT = Path(__file__).resolve().parents[1]
MYSQL = LOCAL / "mysql-5.7.44"
SOURCE = ORIGINALS / "database-source/var/lib/mysql"
DATABASES = ["d_channel", "d_guild", "d_taiwan", "d_taiwan_secu", "d_technical_report",
             "taiwan_billing", "taiwan_cain", "taiwan_cain_2nd", "taiwan_cain_auction_cera",
             "taiwan_cain_auction_gold", "taiwan_cain_log", "taiwan_cain_web", "taiwan_game_event",
             "taiwan_login", "taiwan_login_play", "taiwan_mng_manager", "taiwan_prod", "taiwan_se_event"]
DUMP_OPTIONS = ["--default-character-set=latin1", "--lock-all-tables", "--routines", "--events",
                "--triggers", "--hex-blob", "--skip-comments", "--set-gtid-purged=OFF", "--databases", *DATABASES]
TOOLS = {"mysqld.exe": "432693b766a4bc23a769f19993fa2bcb86f983db7e9f40d32a6380ead6f62d63",
         "mysql.exe": "3e5828f7eac9f63d1c31c9e243906d6b1ca55fe9abe69ffde6765c41712031e6",
         "mysqladmin.exe": "0db97929bdfa84efd49e75c072095d16231f44b1657e2157b8439ae7b4fdb98d",
         "mysqldump.exe": "67f5aa71bf46210da9e2cc74e17b73cadae930f8150ffe167f01783fa5178d09",
         "mysql_upgrade.exe": "c60ea935007cb202cde58ba01aec5d491398876de3de70104d5b5f69374ee121",
         "mysqlcheck.exe": "283a89997a2a30d8ee79918203c67e9db7990853c8a3748c3b8f0936105a9395"}


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def tree_identity(directory):
    digest, count, size = hashlib.sha256(), 0, 0
    for path in sorted(directory.rglob("*")):
        if not path.is_file():
            continue
        length = path.stat().st_size
        digest.update((path.relative_to(directory).as_posix() + "\0" + str(length) + "\0" + sha256(path) + "\n").encode("utf-8"))
        count += 1
        size += length
    return {"files": count, "bytes": size, "tree_sha256": digest.hexdigest()}


def tool(name, options):
    return [str(MYSQL / "bin" / name), *options]


def command_run(records, directory, name, command, stdin=None, output=None, timeout=180):
    stdout_path = output or directory / (name + ".stdout.log")
    stderr_path = directory / (name + ".stderr.log")
    record = {"step": name, "command": command, "exit_code": None, "timed_out": False}
    records.append(record)
    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        try:
            process = subprocess.run(command, stdin=stdin, stdout=stdout, stderr=stderr, timeout=timeout)
            record["exit_code"] = process.returncode
        except subprocess.TimeoutExpired:
            record["timed_out"] = True
            raise RuntimeError(name + " timed out; private diagnostics retained") from None
        finally:
            stdout.flush()
            stderr.flush()
            record["stdout"] = {"bytes": stdout_path.stat().st_size, "sha256": sha256(stdout_path)}
            record["stderr"] = {"bytes": stderr_path.stat().st_size, "sha256": sha256(stderr_path)}
            record["stderr_error_lines"] = sum(b"ERROR" in line.upper() for line in stderr_path.read_bytes().splitlines())
    if record["exit_code"]:
        raise RuntimeError(name + " failed; private diagnostics retained")


def admin_query(session, sql):
    marker = "dnf_probe_" + secrets.token_hex(12)
    session["process"].stdin.write((sql + "\nSELECT '" + marker + "';\n").encode("ascii"))
    session["process"].stdin.flush()
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        if marker.encode("ascii") + b"\n" in session["stdout"].read_bytes().replace(b"\r\n", b"\n"):
            return
        if session["process"].poll() is not None:
            raise RuntimeError("Persistent administration query failed; private diagnostics retained")
        time.sleep(0.05)
    raise RuntimeError("Persistent administration query timed out; private diagnostics retained")


@contextmanager
def running_server(records, directory, pipe, phase, recover=False, client_options=None):
    command = tool("mysqld.exe", ["--no-defaults", "--basedir=" + str(MYSQL),
                                 "--datadir=" + str(directory / "data"), "--lower-case-table-names=1",
                                 "--character-set-server=latin1", "--skip-networking", "--enable-named-pipe",
                                 "--socket=" + pipe, "--console", *(["--skip-grant-tables"] if recover else [])])
    options = client_options or ["--no-defaults", "--protocol=PIPE", "--socket=" + pipe, "--user=root"]
    record = {"instance": str(directory), "phase": phase, "command": command, "ready": False, "forced_cleanup": False,
              "shutdown": "not_requested", "exit_code": None}
    records.append(record)
    session = None
    with (directory / (phase + "-server.log")).open("wb") as log, \
            (directory / (phase + "-admin.stdout.log")).open("wb") as admin_out, \
            (directory / (phase + "-admin.stderr.log")).open("wb") as admin_err:
        process = subprocess.Popen(command, stdout=log, stderr=log)
        record["pid"] = process.pid
        try:
            deadline = time.monotonic() + 45
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError("Private MySQL server exited during startup")
                probe = subprocess.run(tool("mysql.exe", [*options, "--execute=SELECT 1;"]),
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
                if probe.returncode == 0:
                    record["ready"] = True
                    break
                time.sleep(0.1)
            if not record["ready"]:
                raise RuntimeError("Private MySQL server did not become ready")
            admin = subprocess.Popen(tool("mysql.exe", [*options, "--batch", "--skip-column-names", "--unbuffered"]),
                                     stdin=subprocess.PIPE, stdout=admin_out, stderr=admin_err)
            session = {"process": admin, "stdout": directory / (phase + "-admin.stdout.log")}
            admin_query(session, "SELECT 1;")
            yield session, options
        finally:
            if process.poll() is None:
                if session and session["process"].poll() is None:
                    try:
                        session["process"].stdin.write(b"SHUTDOWN;\n")
                        session["process"].stdin.close()
                        record["shutdown"] = "persistent_admin_sql"
                    except OSError:
                        record["shutdown"] = "persistent_admin_write_failed"
                else:
                    try:
                        stop = subprocess.run(tool("mysqladmin.exe", [*options, "shutdown"]),
                                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
                        record["shutdown"] = "mysqladmin" if stop.returncode == 0 else "mysqladmin_failed"
                    except (OSError, subprocess.TimeoutExpired):
                        record["shutdown"] = "mysqladmin_failed"
                try:
                    process.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    record["forced_cleanup"] = True
                    process.terminate()
                    process.wait(timeout=10)
            record["exit_code"] = process.returncode
            if session:
                admin = session["process"]
                if admin.stdin and not admin.stdin.closed:
                    try:
                        admin.stdin.close()
                    except OSError:
                        pass
                try:
                    admin.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    admin.terminate()
                    admin.wait(timeout=5)
                    record["admin_forced_cleanup"] = True
                record["admin_exit_code"] = admin.returncode


def main():
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    directory = BUILD / "probes" / stamp
    recovery, restored = directory / "mysql-recovery", directory / "mysql-restored"
    recovery.mkdir(parents=True)
    restored.mkdir()
    receipt = {"recorded_utc": stamp, "os": platform.platform(), "source": str(SOURCE),
               "script_sha256": sha256(Path(__file__)),
               "database_count": len(DATABASES), "databases": DATABASES, "dump_options": DUMP_OPTIONS,
               "commands": [], "servers": [], "passed": False,
               "scope": "Private physical-copy recovery and logical application-data roundtrip on this developer machine only.",
               "limitations": ["Original system schemas and accounts are not imported into the fresh instance.",
                               "No GUI backup/restore, standalone package, CRT closure, clean-system or cross-OS acceptance."]}
    failure = None
    try:
        receipt["tools"] = {name: sha256(MYSQL / "bin" / name) for name in TOOLS}
        if receipt["tools"] != TOOLS:
            raise RuntimeError("MySQL tools differ from the pinned official 5.7.44 binaries")
        receipt["source_before"] = tree_identity(SOURCE)
        shutil.copytree(SOURCE, recovery / "data")
        receipt["recovery_copy"] = tree_identity(recovery / "data")
        if receipt["source_before"] != receipt["recovery_copy"]:
            raise RuntimeError("Physical source copy differs before recovery")
        print("Physical source copied and verified:", receipt["source_before"]["files"], "files", flush=True)
        with running_server(receipt["servers"], recovery, "DNFNativeRecovery" + stamp, "upgrade", recover=True) as (admin, options):
            command_run(receipt["commands"], recovery, "upgrade", tool("mysql_upgrade.exe", [*options, "--force"]))
            # mysql_upgrade reloads privilege tables. This connection was opened
            # before that reload and retains its established administrative rights.
            user, password = "dnf_probe_" + secrets.token_hex(6), secrets.token_hex(16)
            admin_query(admin, "CREATE USER '" + user + "'@'localhost' IDENTIFIED BY '" + password + "';\n"
                        "GRANT ALL PRIVILEGES ON *.* TO '" + user + "'@'localhost' WITH GRANT OPTION;")
            private = recovery / "client.private.ini"
            private.write_text("[client]\nprotocol=PIPE\nsocket=DNFNativeRecovery" + stamp + "\nuser=" + user +
                               "\npassword=" + password + "\n", encoding="ascii")
            client_options = ["--defaults-file=" + str(private)]
        # The upgraded performance_schema layout is activated on restart.
        with running_server(receipt["servers"], recovery, "DNFNativeRecovery" + stamp, "export",
                            client_options=client_options) as (_, options):
            command_run(receipt["commands"], recovery, "check-upgrade",
                        tool("mysqlcheck.exe", [*options, "--check-upgrade", "--all-databases"]))
            command_run(receipt["commands"], recovery, "export", tool("mysqldump.exe", [*options, *DUMP_OPTIONS]),
                        output=recovery / "application.sql")
        print("Original copy upgraded, application databases exported, recovery server stopped", flush=True)
        command_run(receipt["commands"], restored, "initialize", tool("mysqld.exe", ["--no-defaults",
                    "--basedir=" + str(MYSQL), "--datadir=" + str(restored / "data"), "--lower-case-table-names=1",
                    "--character-set-server=latin1", "--initialize-insecure", "--console"]))
        with running_server(receipt["servers"], restored, "DNFNativeRestored" + stamp, "restore") as (_, options):
            with (recovery / "application.sql").open("rb") as sql:
                command_run(receipt["commands"], restored, "import",
                            tool("mysql.exe", [*options, "--comments", "--default-character-set=latin1"]), stdin=sql)
            command_run(receipt["commands"], restored, "reexport", tool("mysqldump.exe", [*options, *DUMP_OPTIONS]),
                        output=restored / "application.sql")
        receipt["seed"] = {"bytes": (recovery / "application.sql").stat().st_size,
                           "sha256": sha256(recovery / "application.sql")}
        receipt["reexport"] = {"bytes": (restored / "application.sql").stat().st_size,
                               "sha256": sha256(restored / "application.sql")}
        receipt["identical"] = (recovery / "application.sql").read_bytes() == (restored / "application.sql").read_bytes()
        receipt["source_after"] = tree_identity(SOURCE)
        receipt["source_unchanged"] = receipt["source_before"] == receipt["source_after"]
        receipt["passed"] = receipt["identical"] and receipt["source_unchanged"] and all(
            server["exit_code"] == 0 and not server["forced_cleanup"] and not server.get("admin_forced_cleanup", False)
            for server in receipt["servers"])
        if not receipt["passed"]:
            raise RuntimeError("Roundtrip, original source identity or normal shutdown check failed")
    except Exception as error:
        failure = str(error)
        receipt["failure"] = failure
    finally:
        receipt["private_logs"] = {str(path.relative_to(directory)): {"bytes": path.stat().st_size, "sha256": sha256(path)}
                                   for path in directory.rglob("*.log")}
        text = json.dumps(receipt, indent=2) + "\n"
        (directory / "mysql-probe.json").write_text(text, encoding="utf-8")
        (BUILD / "mysql-probe.json").write_text(text, encoding="utf-8")
    print("MySQL logical roundtrip:", "PASS" if receipt["passed"] else "FAIL", flush=True)
    print("Private instances:", directory, flush=True)
    if failure:
        raise SystemExit(failure)


if __name__ == "__main__":
    main()
