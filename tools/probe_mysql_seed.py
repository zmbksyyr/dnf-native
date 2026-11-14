"""Restore the original application seed into a securely initialized single-EXE instance."""

import argparse
import datetime
import json
import os
from pathlib import Path
import platform
import secrets
import socket
import subprocess
import time

from bootstrap import sha256
from probe_mysql import DATABASES, DUMP_OPTIONS
from legacy_db_password import encode_password

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=Path, required=True, help="Private verified application-only logical seed")
    parser.add_argument("--seed-sha256", required=True)
    parser.add_argument("--exe", type=Path, default=ROOT / "build/mysql-standalone/mysql-standalone.exe")
    args = parser.parse_args()
    seed, exe = args.seed.resolve(strict=True), args.exe.resolve(strict=True)
    if sha256(seed) != args.seed_sha256:
        raise SystemExit("Seed SHA-256 differs")
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    instance = ROOT / "build/probes" / stamp / "mysql-secure-seed"
    instance.mkdir(parents=True)
    env = dict(os.environ)
    env["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32")
    for name in ["MYSQL_HOME", "MYSQL_PWD", "MYSQL_TCP_PORT"]:
        env.pop(name, None)
    receipt = {"recorded_utc": stamp, "os": platform.platform(), "probe": str(instance.relative_to(ROOT)),
               "scope": "Secure single-EXE initialization and real original application-seed backup/restore; no GUI or DNF client acceptance.",
               "exe_sha256": sha256(exe), "script_sha256": sha256(Path(__file__)),
               "seed_sha256": sha256(seed), "databases": DATABASES, "checks": {}, "forced_cleanup": False}
    server = None
    index = 0
    options = ["--defaults-file=client.private.cnf"]

    def command(tool, arguments):
        return [str(exe), tool, str(instance), *arguments]

    def invoke(tool, arguments, data=None, check=True):
        nonlocal index
        result = subprocess.run(command(tool, arguments), input=data, capture_output=True, env=env, timeout=180)
        index += 1
        if result.returncode:
            (instance / f"tool-{index}.private.log").write_bytes(result.stdout + result.stderr)
            if check:
                raise RuntimeError(f"Official {tool} failed; see private tool log {index}")
        return result

    def sql(query):
        return invoke("mysql", options + ["--batch", "--skip-column-names"], query.encode("ascii")).stdout

    try:
        with socket.socket() as reserved:
            reserved.bind(("127.0.0.1", 0))
            port = reserved.getsockname()[1]
        invoke("initialize", [str(port)])
        receipt["checks"]["native_secure_initialization"] = True
        with (instance / "server.private.log").open("wb") as log:
            server = subprocess.Popen(command("mysqld", ["--defaults-file=server.cnf", "--console"]),
                                      stdout=log, stderr=log, env=env)
        deadline = time.monotonic() + 45
        while True:
            if server.poll() is not None:
                raise RuntimeError("Secure seed server exited during startup")
            ready = invoke("mysql", options, b"SELECT 1;", check=False)
            if ready.returncode == 0:
                break
            if time.monotonic() >= deadline:
                raise RuntimeError("Secure seed server readiness timed out")
            time.sleep(0.1)
        invoke("mysql", options + ["--comments", "--binary-mode", "--default-character-set=latin1"], seed.read_bytes())
        receipt["checks"]["original_application_seed_imported"] = True
        table_summary = "SELECT TABLE_SCHEMA,COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA IN (" + \
                        ",".join("'" + name + "'" for name in DATABASES) + ") GROUP BY TABLE_SCHEMA ORDER BY TABLE_SCHEMA;"
        before = sql(table_summary)
        receipt["table_counts"] = {line.split("\t")[0]: int(line.split("\t")[1])
                                   for line in before.decode("ascii").splitlines()}
        if set(receipt["table_counts"]) != set(DATABASES) or not all(receipt["table_counts"].values()):
            raise RuntimeError("Original application schemas are incomplete")
        first = instance / "application-backup.sql"
        # Windows mysql removes one CR before every LF even with --binary-mode.
        # mysqldump's text stdout adds that CR; --result-file bypasses it and
        # therefore loses original stored-program CRLF on the next import.
        first.write_bytes(invoke("mysqldump", options + DUMP_OPTIONS).stdout)
        receipt["backup_sha256"] = sha256(first)
        receipt["backup_bytes"] = first.stat().st_size
        # The fresh private instance has a completed logical backup before this destructive test.
        sql(";".join("DROP DATABASE `" + name + "`" for name in DATABASES) + ";")
        invoke("mysql", options + ["--comments", "--binary-mode", "--default-character-set=latin1"], first.read_bytes())
        if sql(table_summary) != before:
            raise RuntimeError("Restored table counts differ")
        second = instance / "application-reexport.sql"
        second.write_bytes(invoke("mysqldump", options + DUMP_OPTIONS).stdout)
        receipt["reexport_sha256"] = sha256(second)
        if first.read_bytes() != second.read_bytes():
            raise RuntimeError("Application backup/reexport bytes differ")
        receipt["checks"]["all_application_databases_backup_restore_identical"] = True
        cipher = sql("SHOW SESSION STATUS LIKE 'Ssl_cipher';").decode("ascii").strip().split("\t")
        if len(cipher) != 2 or not cipher[1]:
            raise RuntimeError("Application administration transport is not TLS")
        receipt["tls_cipher"] = cipher[1]
        password = secrets.token_hex(8)
        db = {"host": "127.0.0.1", "port": port, "user": "dnf_native_game", "password": encode_password(password),
              "database": "d_taiwan"}
        sql("CREATE USER 'dnf_native_game'@'127.0.0.1' IDENTIFIED BY '" + password +
            "'; GRANT ALL ON *.* TO 'dnf_native_game'@'127.0.0.1';")
        sql("UPDATE d_taiwan.db_connect SET db_ip='127.0.0.1',db_port=" + str(port) +
            ",db_userid='dnf_native_game',db_passwd='" + db["password"] + "';")
        (instance / "game-db.json").write_text(json.dumps(db), encoding="utf-8")
        receipt["checks"]["private_game_account_prepared"] = True
    except Exception as failure:
        receipt["error"] = str(failure)
    finally:
        if server is not None:
            if server.poll() is None:
                stopped = invoke("mysqladmin", options + ["shutdown"], check=False)
                receipt["mysqladmin_shutdown_exit_code"] = stopped.returncode
                receipt["mysqladmin_shutdown_lost_ack"] = stopped.returncode == 1 and b"Lost connection to MySQL server during query" in stopped.stderr
                try:
                    server.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    receipt["forced_cleanup"] = True
                    subprocess.run(["taskkill.exe", "/PID", str(server.pid), "/T", "/F"], capture_output=True, timeout=10)
                    server.wait(timeout=10)
            receipt["server_exit_code"] = server.returncode
            receipt["server_shutdown_complete"] = server.returncode == 0 and b"Shutdown complete" in (instance / "server.private.log").read_bytes()
        receipt["seed_source_unchanged"] = sha256(seed) == args.seed_sha256
        receipt["limits"] = ["Developer Windows 10 only; not a clean Win7/10/11 offline matrix.",
                             "Original application schemas only; source MySQL system accounts are not imported.",
                             "Game account is deliberately broad for private diagnosis; no product privilege policy or GUI rollback acceptance."]
        (instance / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(receipt, ensure_ascii=True))
    if (receipt.get("error") or receipt["forced_cleanup"] or not receipt.get("server_shutdown_complete") or
            (receipt.get("mysqladmin_shutdown_exit_code") != 0 and not receipt.get("mysqladmin_shutdown_lost_ack")) or
            not receipt["seed_source_unchanged"]):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
