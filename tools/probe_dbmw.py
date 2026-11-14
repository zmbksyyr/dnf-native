"""Observe three original DBMW instances on an independent private database copy."""
import argparse
import datetime
import json
from pathlib import Path
import platform
import re
import shutil
import socket
import subprocess
import time

from probe_mysql import tree_identity
from test_bridge import sha
from test_channel import request_stop

ROOT = Path(__file__).resolve().parents[1]
INSTANCES = [("dbmw_guild", "dbmw_gld_siroco"), ("dbmw_mnt", "dbmw_mnt_siroco"),
             ("dbmw_stat", "dbmw_stat_siroco")]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database-source", type=Path, required=True,
                        help="Stopped private restored instance containing data/ and game-db.json")
    args = parser.parse_args()
    source = args.database_source.resolve()
    for port in [3307, 20403, 20203, 20303]:
        with socket.socket() as check:
            check.bind(("127.0.0.1", port))
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    instance = ROOT / "build/probes" / stamp / "dbmw"
    instance.mkdir(parents=True)
    before = tree_identity(source / "data")
    shutil.copytree(source / "data", instance / "mysql/data")
    if before != tree_identity(instance / "mysql/data"):
        raise RuntimeError("Private database copy differs from its source")
    mysqlbin = ROOT / ".local/mysql-5.7.44/bin"
    pipe = "DNFDbmw" + stamp
    options = ["--no-defaults", "--protocol=PIPE", "--socket=" + pipe, "--user=root"]
    receipt = {"recorded_utc": stamp, "os": platform.platform(), "probe": str(instance.relative_to(ROOT)),
               "scope": "Three original DBMW instances: database/thread initialization and SIGTERM cleanup only. No protocol or database business acceptance.",
               "database_source": before, "results": []}
    processes, database = [], None

    def sql(query):
        result = subprocess.run([str(mysqlbin / "mysql.exe"), *options, "--batch", "--skip-column-names"],
                                input=query.encode("ascii"), capture_output=True, timeout=10)
        if result.returncode:
            (instance / "sql-error.private.log").write_bytes(result.stderr)
            raise RuntimeError("Private SQL failed; see the private log")
        return result.stdout

    try:
        command = [str(mysqlbin / "mysqld.exe"), "--no-defaults", "--basedir=" + str(mysqlbin.parent),
                   "--datadir=" + str(instance / "mysql/data"), "--lower-case-table-names=1",
                   "--character-set-server=latin1", "--bind-address=127.0.0.1", "--port=3307",
                   "--enable-named-pipe", "--socket=" + pipe, "--console"]
        with (instance / "mysql/server.private.log").open("wb") as log:
            database = subprocess.Popen(command, stdout=log, stderr=log)
        # A real query gates service startup; the TCP listener alone is insufficient.
        for attempt in range(100):
            check = subprocess.run([str(mysqlbin / "mysql.exe"), *options, "--execute=SELECT 1"],
                                   capture_output=True, timeout=5)
            if check.returncode == 0:
                break
            if database.poll() is not None:
                raise RuntimeError("Private MySQL exited during startup")
            time.sleep(0.1)
        else:
            raise RuntimeError("Private MySQL startup timeout")
        sql("UPDATE d_taiwan.db_connect SET db_ip='127.0.0.1', db_port=3307;")
        db = json.loads((source / "game-db.json").read_text(encoding="utf-8"))
        db["port"] = 3307
        for name, configuration in INSTANCES:
            private = instance / name
            for lib in ["lib", "usr"]:
                shutil.copytree(ROOT / ".local/sysroot/i686" / lib, private / lib)
            guest = private / "home/neople" / name
            shutil.copytree(ROOT / "originals/dp123/home/neople" / name, guest)
            for directory in ["log", "pid"]:
                (guest / directory).mkdir(exist_ok=True)
            config = guest / "cfg" / (configuration + ".cfg")
            text = config.read_text(encoding="utf-8")
            for setting, key in [("ip", "host"), ("port", "port"), ("acc", "user"),
                                 ("pwd", "password"), ("name", "database")]:
                text, count = re.subn(r"(?m)^master_db_" + setting + r"[^\S\r\n]*=.*$",
                                     lambda match: "master_db_" + setting + " = " + str(db[key]), text)
                if count != 1:
                    raise RuntimeError("Original DBMW configuration field not found: " + setting)
            config.write_text(text, encoding="utf-8")
            host, ld, elf = ROOT / "build/i686/dnf-host.exe", private / "lib/ld-2.17.so", guest / "df_dbmw_r"
            command = [str(host), "--root", str(private), sha(ld), "--env", "HOME=/home/neople", "--cwd",
                       "/home/neople/" + name, "--run", str(elf), sha(elf), "/home/neople/" + name + "/df_dbmw_r",
                       configuration, "nofork"]
            record = {"name": name, "host_sha256": sha(host), "elf_sha256": sha(elf), "config_sha256": sha(config)}
            receipt["results"].append(record)
            with (private / "stdout.private.log").open("wb") as out, (private / "stderr.log").open("wb") as err:
                process = subprocess.Popen(command, stdout=out, stderr=err)
            processes.append((process, private, record))
            try:
                process.wait(timeout=5)
                raise RuntimeError(name + " exited before the shutdown request")
            except subprocess.TimeoutExpired:
                pass
    except Exception as failure:
        receipt["error"] = str(failure)
    finally:
        for process, private, record in processes:
            record["forced_cleanup"] = False
            if process.poll() is None:
                request_stop(process.pid)
                try:
                    process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    record["forced_cleanup"] = True
                    process.terminate()
                    process.wait(timeout=5)
            record["exit_code"] = process.returncode
            stderr = (private / "stderr.log").read_text(encoding="utf-8", errors="replace")
            stdout = (private / "stdout.private.log").read_text(encoding="utf-8", errors="replace")
            record["gaps"] = [line for line in stderr.splitlines()
                              if any(marker in line for marker in ["syscall gap", "unhandled exception", "unsupported", "exhausted"])]
            record["markers"] = [marker for marker in ["DBMW_ALL_DB Open Success", "Application Network Thread Begin() Success!",
                                                        "Application Load() Success!", "Recv SIGTERM signal", "Thread Free Success!", "Application End!"]
                                 if marker in stdout]
            record["logs"] = {name: sha(private / name) for name in ["stderr.log", "stdout.private.log"]}
            # Original main at 0x80637e0 returns 1 even after successful Clear().
            record["passed"] = (record["exit_code"] == 1 and not record["forced_cleanup"] and
                                not record["gaps"] and len(record["markers"]) == 6)
            print(record["name"], "PASS" if record["passed"] else "FAIL", "exit", record["exit_code"], flush=True)
        if database:
            receipt["mysql_forced_cleanup"] = False
            if database.poll() is None:
                stopped = subprocess.run([str(mysqlbin / "mysqladmin.exe"), *options, "shutdown"], capture_output=True, timeout=15)
                receipt["mysql_shutdown_exit_code"] = stopped.returncode
                (instance / "mysql/shutdown.private.log").write_bytes(stopped.stdout + stopped.stderr)
                try:
                    database.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    receipt["mysql_forced_cleanup"] = True
                    database.terminate()
                    database.wait(timeout=5)
            receipt["mysql_exit_code"] = database.returncode
        receipt["database_source_unchanged"] = before == tree_identity(source / "data")
        (instance / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        print("DBMW receipt:", instance / "receipt.json", flush=True)
    if (receipt.get("error") or len(receipt["results"]) != 3 or not all(row["passed"] for row in receipt["results"]) or
            receipt.get("mysql_exit_code") != 0 or receipt.get("mysql_forced_cleanup") or not receipt["database_source_unchanged"]):
        raise SystemExit(receipt.get("error", "DBMW observation failed"))


if __name__ == "__main__":
    main()
