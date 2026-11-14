"""Exercise the single EXE in a fresh Unicode instance with real official MySQL tools."""

import argparse
import datetime
import json
import os
from pathlib import Path
import platform
import shutil
import socket
import subprocess
import time

from bootstrap import sha256

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=ROOT / "build/mysql-standalone/mysql-standalone.exe")
    args = parser.parse_args()
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    probe = ROOT / "build/probes" / stamp / "mysql-sfx"
    isolated = probe / "单文件 (独立目录)"
    isolated.mkdir(parents=True)
    exe = isolated / "mysql-standalone.exe"
    shutil.copyfile(args.exe, exe)
    instance = probe / "中文 (数据库实例)"
    env = dict(os.environ)
    env["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32")
    for name in ["MYSQL_HOME", "MYSQL_PWD", "MYSQL_TCP_PORT"]:
        env.pop(name, None)
    receipt = {"recorded_utc": stamp, "os": platform.platform(), "probe": str(probe.relative_to(ROOT)),
               "exe_sha256": sha256(exe), "exe_bytes": exe.stat().st_size,
               "scope": "Secure native initialization, TLS query, backup and restore from only the standalone EXE; developer Windows 10 only.",
               "checks": {}, "forced_cleanup": False}
    checks = receipt["checks"]
    server = None
    options = []
    log_index = 0

    def invoke(tool, arguments, data=None, check=True):
        nonlocal log_index
        result = subprocess.run([str(exe), tool, str(instance), *arguments], input=data,
                                capture_output=True, env=env, cwd=isolated, timeout=180 if tool == "initialize" else 60)
        log_index += 1
        if result.returncode:
            (probe / (f"tool-{log_index}.private.log")).write_bytes(result.stdout + result.stderr)
            if check:
                raise RuntimeError(f"Official {tool} failed; see private tool log {log_index}")
        return result

    try:
        preparations = [subprocess.Popen([str(exe), "prepare", str(instance)], stdout=subprocess.PIPE,
                                         stderr=subprocess.PIPE, env=env, cwd=isolated) for _ in range(4)]
        outputs = [process.communicate(timeout=60) for process in preparations]
        if any(process.returncode for process in preparations) or len({output[0] for output in outputs}) != 1:
            raise RuntimeError("Concurrent preparation did not produce one verified payload")
        payload = Path(outputs[0][0].decode("utf-8").strip())
        checks["concurrent_prepare"] = len(preparations)
        versions = {}
        for tool in ["mysqld", "mysql", "mysqladmin", "mysqldump"]:
            output = invoke(tool, ["--no-defaults", "--version"]).stdout.decode("utf-8", errors="replace")
            if "5.7.44" not in output:
                raise RuntimeError("Official tool version differs")
            versions[tool] = "5.7.44"
        receipt["versions"] = versions
        with socket.socket() as reserved:
            reserved.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            reserved.bind(("127.0.0.1", 0))
            port = reserved.getsockname()[1]
            initialized = invoke("initialize", [str(port)])
        config = instance / "client.private.cnf"
        password = next(line.split("=", 1)[1] for line in config.read_text(encoding="ascii").splitlines()
                        if line.startswith("password="))
        if len(password) != 48 or password.encode("ascii") in initialized.stdout + initialized.stderr:
            raise RuntimeError("Generated credential is missing or was exposed")
        if (instance / "bootstrap.private.cnf").exists():
            raise RuntimeError("Temporary bootstrap credential was retained after success")
        if b"Shutdown complete" not in (instance / "mysql-bootstrap.private.log").read_bytes():
            raise RuntimeError("Initialization bootstrap did not shut down completely")
        checks["secure_initialize_without_tcp"] = True
        checks["initialize_fresh_private_data"] = True
        acl_env = dict(env)
        acl_env["DNF_MYSQL_INSTANCE"] = str(instance)
        acl_script = """$root = $env:DNF_MYSQL_INSTANCE
$sid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$results = foreach ($name in @('data','server.cnf','client.private.cnf','mysql-initialize.private.log','mysql-bootstrap.private.log','mysql-shutdown.private.log')) {
    $acl = Get-Acl -LiteralPath (Join-Path $root $name)
    $rules = @($acl.GetAccessRules($true,$true,[System.Security.Principal.SecurityIdentifier]))
    [pscustomobject]@{name=$name; protected=$acl.AreAccessRulesProtected;
        current_user_only=($rules.Count -gt 0 -and @($rules | Where-Object { $_.IdentityReference.Value -ne $sid -or $_.AccessControlType -ne 'Allow' }).Count -eq 0)}
}
ConvertTo-Json -InputObject @($results) -Compress
"""
        acls = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", acl_script],
                              capture_output=True, env=acl_env, timeout=20, check=True)
        receipt["private_permissions"] = json.loads(acls.stdout.decode("ascii"))
        if not all(item["protected"] and item["current_user_only"] for item in receipt["private_permissions"]):
            raise RuntimeError("Sensitive instance files are not restricted to the current Windows user")
        checks["private_windows_permissions"] = True
        options = ["--defaults-file=" + config.name]
        pid_file = instance / "mysql.pid"
        with (probe / "server.private.log").open("wb") as output:
            server = subprocess.Popen([str(exe), "mysqld", str(instance), "--defaults-file=server.cnf", "--console",
                                       "--pid-file=" + str(pid_file)],
                                      stdout=output, stderr=output, env=env, cwd=isolated)
        deadline = time.monotonic() + 45
        while True:
            if server.poll() is not None:
                raise RuntimeError("MySQL exited during startup; see private server log")
            ready = invoke("mysql", options, b"SELECT VERSION();", check=False)
            if ready.returncode == 0:
                break
            if time.monotonic() >= deadline:
                raise RuntimeError("MySQL SQL readiness timeout")
            time.sleep(0.1)
        passwordless = invoke("mysql", ["--no-defaults", "--protocol=TCP", "--host=127.0.0.1", "--port=" + str(port),
                                        "--user=root", "--ssl-mode=REQUIRED"], b"SELECT 1;", check=False)
        if passwordless.returncode == 0 or b"1045" not in passwordless.stderr:
            raise RuntimeError("Passwordless root was not denied by authentication")
        checks["passwordless_root_refused"] = True

        def sql(query):
            return invoke("mysql", options + ["--batch", "--skip-column-names"], query.encode("utf-8")).stdout

        sql("CREATE DATABASE native_probe CHARACTER SET utf8mb4;"
            "CREATE TABLE native_probe.sample(id INT PRIMARY KEY, body VARBINARY(256)) ENGINE=InnoDB;"
            "INSERT INTO native_probe.sample VALUES(1,0x000102FF),(2,0xE4B8ADE69687);")
        expected = b"1\t000102FF\n2\tE4B8ADE69687\n"
        query = "SELECT id,HEX(body) FROM native_probe.sample ORDER BY id;"
        if sql(query).replace(b"\r\n", b"\n") != expected:
            raise RuntimeError("Database row bytes differ")
        receipt["database_identity"] = sql("SELECT VERSION(),@@server_uuid;").decode("ascii").strip()
        tls = sql("SHOW SESSION STATUS LIKE 'Ssl_cipher';").decode("ascii").strip().split("\t")
        if len(tls) != 2 or not tls[1]:
            raise RuntimeError("TLS cipher is absent")
        receipt["tls_cipher"] = tls[1]
        checks["query_write_read_over_tls"] = True
        dump = instance / "数据库备份.sql"
        invoke("mysqldump", options + ["--databases", "native_probe", "--routines", "--events", "--triggers",
                                       "--hex-blob", "--result-file=" + str(dump)])
        receipt["logical_backup_sha256"] = sha256(dump)
        sql("DROP DATABASE native_probe;")
        invoke("mysql", options, dump.read_bytes())
        if sql(query).replace(b"\r\n", b"\n") != expected:
            raise RuntimeError("Restored database bytes differ")
        checks["logical_backup_restore_rows"] = 2
        invoke("mysqladmin", options + ["ping"])
        child_pid = int(pid_file.read_text(encoding="ascii").strip())
        module_env = dict(env)
        module_env["DNF_MYSQL_CHILD_PID"] = str(child_pid)
        script = """[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding
$result = (Get-Process -Id $env:DNF_MYSQL_CHILD_PID).Modules |
    Where-Object { $_.ModuleName -in @('msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll','ucrtbase.dll') } |
    Select-Object ModuleName,FileName
ConvertTo-Json -InputObject @($result) -Compress
"""
        modules = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", script],
                                 capture_output=True, env=module_env, timeout=20, check=True)
        loaded = json.loads(modules.stdout.decode("utf-8-sig"))
        receipt["runtime_modules"] = []
        vc_names = {"msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"}
        seen = set()
        for item in loaded:
            name = item["ModuleName"].lower()
            path = Path(item["FileName"])
            local = path.parent == payload / "bin"
            receipt["runtime_modules"].append({"name": name, "app_local": local, "sha256": sha256(path)})
            if name in vc_names:
                if not local:
                    raise RuntimeError("VC runtime loaded outside the standalone payload")
                seen.add(name)
        if seen != vc_names:
            raise RuntimeError("Cannot verify all three loaded VC runtime modules")
        checks["loaded_vc_runtime_app_local"] = True
        stopped = invoke("mysqladmin", options + ["shutdown"], check=False)
        receipt["mysqladmin_shutdown_exit_code"] = stopped.returncode
        server.wait(timeout=30)
        receipt["server_exit_code"] = server.returncode
        checks["server_shutdown_complete"] = server.returncode == 0 and b"Shutdown complete" in (probe / "server.private.log").read_bytes()
        if not checks["server_shutdown_complete"]:
            raise RuntimeError("Official MySQL did not shut down normally")
        receipt["mysqladmin_shutdown_lost_ack"] = stopped.returncode == 1 and b"Lost connection to MySQL server during query" in stopped.stderr
        if stopped.returncode and not receipt["mysqladmin_shutdown_lost_ack"]:
            raise RuntimeError("mysqladmin shutdown reported an unexpected error; server outcome recorded separately")
        identity = {str(p.relative_to(instance / "data")): sha256(p)
                    for p in (instance / "data").rglob("*") if p.is_file()}
        repeat = invoke("initialize", [str(port)], check=False)
        if repeat.returncode == 0 or b"fresh instance" not in repeat.stderr:
            raise RuntimeError("Secure initialization accepted an existing instance")
        repeat = invoke("mysqld", ["--no-defaults", "--initialize-insecure", "--console"], check=False)
        after = {str(p.relative_to(instance / "data")): sha256(p)
                 for p in (instance / "data").rglob("*") if p.is_file()}
        if repeat.returncode == 0 or identity != after:
            raise RuntimeError("Reinitialization did not preserve existing data")
        checks["existing_data_reinitialization_refused"] = True
        readme = payload / "README"
        content = readme.read_bytes()
        try:
            readme.write_bytes(b"!" + content[1:])
            rejected = subprocess.run([str(exe), "prepare", str(instance)], capture_output=True, env=env, timeout=30)
            if rejected.returncode == 0 or b"Payload hash differs" not in rejected.stderr:
                raise RuntimeError("Modified cache was accepted")
            checks["modified_cache_refused"] = True
        finally:
            readme.write_bytes(content)
    except Exception as failure:
        receipt["error"] = str(failure)
    finally:
        if server and server.poll() is None:
            try:
                invoke("mysqladmin", options + ["shutdown"], check=False)
                server.wait(timeout=20)
            except Exception:
                receipt["forced_cleanup"] = True
                subprocess.run(["taskkill.exe", "/PID", str(server.pid), "/T", "/F"], capture_output=True, timeout=10)
                server.wait(timeout=10)
        receipt["limits"] = ["Tests use Windows 10 10.0.19044 on the developer host, not a clean Win7/10/11 matrix.",
                             "Windows 10 loads its system UCRT; app-local UCRT execution on Win7 remains unverified.",
                             "Native initialization generates a local credential; GUI password configuration and cross-machine migration remain untested.",
                             "No GUI backup/rollback workflow or final complete offline package acceptance."]
        (probe / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(receipt, ensure_ascii=True))
    if receipt.get("error") or receipt["forced_cleanup"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
