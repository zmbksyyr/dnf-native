"""Exercise the independent launcher's actual key-export GUI and launch contract.

Optional registration uses only a cold copy of an explicitly supplied private
instance, on a separate database port. No actual game client is used here.
"""
import argparse
import base64
import ctypes as c
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import secrets
import shutil
import socket
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from bootstrap import ROOT, BUILD, TOOLCHAIN
from gui_support import user, windows, wait_for, click, title, select_folder, screenshot, hidden_startup


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def public_numbers(pem):
    data = base64.b64decode(b"".join(pem.splitlines()[1:-1]))

    def tlv(data, offset=0):
        tag, size = data[offset:offset + 2]
        start = offset + 2
        if size & 128:
            count = size & 127
            size = int.from_bytes(data[start:start + count], "big")
            start += count
        return tag, data[start:start + size], start + size

    _, outer, _ = tlv(data)
    _, _, offset = tlv(outer)
    _, bits, _ = tlv(outer, offset)
    assert bits[0] == 0
    _, sequence, _ = tlv(bits[1:])
    _, modulus, offset = tlv(sequence)
    _, exponent, _ = tlv(sequence, offset)
    return int.from_bytes(modulus, "big"), int.from_bytes(exponent, "big")


def external_test_key():
    # Disposable test key, never a production key generator. Use e=3 to catch
    # exporters that incorrectly assume the default public exponent 65537.
    def prime():
        while True:
            n = secrets.randbits(512) | (1 << 511) | 1
            if n % 3 != 2 or any(n % p == 0 for p in (5, 7, 11, 13, 17, 19, 23, 29, 31)): continue
            d, s = n - 1, 0
            while d % 2 == 0: d //= 2; s += 1
            for _ in range(24):
                x = pow(2 + secrets.randbelow(n - 3), d, n)
                if x in (1, n - 1): continue
                for _ in range(s - 1):
                    x = x * x % n
                    if x == n - 1: break
                else: break
            else: return n

    def tlv(tag, value):
        size = len(value)
        encoded = size.to_bytes((size.bit_length() + 7) // 8, "big")
        return bytes([tag]) + (bytes([size]) if size < 128 else bytes([128 + len(encoded)]) + encoded) + value

    p, q = prime(), prime()
    assert p != q
    phi = math.lcm(p - 1, q - 1)
    n, e, d = p * q, 3, pow(3, -1, phi)
    integers = (0, n, e, d, p, q, d % (p - 1), d % (q - 1), pow(q, -1, p))
    body = b"".join(tlv(2, v.to_bytes((v.bit_length() + 8) // 8, "big")) for v in integers)
    b64 = base64.b64encode(tlv(48, body))
    pem = b"-----BEGIN RSA PRIVATE KEY-----\n" + b"\n".join(b64[i:i + 64] for i in range(0, len(b64), 64)) + b"\n-----END RSA PRIVATE KEY-----\n"
    return pem, (n, e)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--launcher", type=Path, default=BUILD / "login/DNFLogin.exe")
    parser.add_argument("--instance-source", type=Path, help="Explicitly selected, stopped private test instance")
    parser.add_argument("--port", type=int, default=13306)
    args = parser.parse_args()
    work = BUILD / "probes" / datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ") / "login"
    work.mkdir(parents=True)
    client = work / "客户端 空格"
    client.mkdir()
    shutil.copy2(args.launcher, client / "DNFLogin.exe")
    checks = {}

    def open_launcher():
        # Deliberately different cwd; no server configuration is available.
        process = subprocess.Popen([str(client / "DNFLogin.exe")], cwd=work, startupinfo=hidden_startup())
        window = wait_for(lambda: next(iter(windows(process.pid, "DNFLoginWndClass")), None), "launcher", 15)
        return process, window

    def close_launcher(process, window):
        user.PostMessageW(window, 0x10, 0, 0)
        process.wait(timeout=15)
        assert process.returncode == 0

    def export(process, window, target):
        click(window, 1010)
        select_folder(process, target)
        wait_for(lambda: "导出成功" in title(user.GetDlgItem(window, 1007)), "export status", 15)
        dialog = wait_for(lambda: next((h for h in windows(process.pid, "#32770") if title(h) == "密钥对已导出"), None), "export message")
        button = wait_for(lambda: next((i for i in (1, 2) if user.GetDlgItem(dialog, i)), None), "export OK button", 10)
        click(dialog, button)
        wait_for(lambda: not user.IsWindow(dialog), "export message close")

    process, window = open_launcher()
    try:
        for identifier, expected in {1001: "127.0.0.1", 1002: "3306", 1003: "game",
                                      1004: "uu5!^%jg", 1005: "test", 1006: "123456"}.items():
            assert title(user.GetDlgItem(window, identifier)) == expected
        assert user.SendMessageW(user.GetDlgItem(window, 1004), 0xD2, 0, 0) == 0
        assert user.SendMessageW(user.GetDlgItem(window, 1006), 0xD2, 0, 0) == 0
        checks["requested_defaults_prefilled_and_visible"] = True
        exported = work / "export-default"; exported.mkdir()
        export(process, window, exported)
        for name in ("private_key.pem", "publickey.pem"):
            assert (exported / name).read_bytes() == (ROOT / "resources/login" / name).read_bytes()
        checks["default_gui_export_exact_pair"] = True
        before = {p.name: digest(p) for p in exported.iterdir()}
        click(window, 1010); select_folder(process, exported)
        wait_for(lambda: "已有密钥" in title(user.GetDlgItem(window, 1007)), "no overwrite")
        assert before == {p.name: digest(p) for p in exported.iterdir()}
        checks["existing_keys_not_overwritten"] = True
        screenshot(window, work / "launcher.png")
    finally:
        close_launcher(process, window)
    external, expected_numbers = external_test_key()
    (client / "private_key.pem").write_bytes(external)
    process, window = open_launcher()
    try:
        exported = work / "export-external"; exported.mkdir()
        export(process, window, exported)
        assert (exported / "private_key.pem").read_bytes() == external
        assert public_numbers((exported / "publickey.pem").read_bytes()) == expected_numbers
        checks["active_external_key_export"] = True
        checks["external_public_exponent_three_preserved"] = True
    finally:
        close_launcher(process, window)
    (client / "private_key.pem").write_bytes(b"invalid key")
    process = subprocess.Popen([str(client / "DNFLogin.exe")], cwd=work, startupinfo=hidden_startup())
    dialog = wait_for(lambda: next(iter(windows(process.pid, "#32770")), None), "invalid key error", 15)
    button = wait_for(lambda: next((i for i in (1, 2) if user.GetDlgItem(dialog, i)), None), "error OK button", 10)
    assert not windows(process.pid, "DNFLoginWndClass")
    click(dialog, button); process.wait(timeout=15); assert process.returncode == 1
    checks["invalid_external_key_rejected"] = True
    (client / "private_key.pem").unlink()

    if args.instance_source:
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", args.port))
        source = args.instance_source.resolve(strict=True)
        instance = work / "database-copy"; instance.mkdir()
        for name in ("mysql", "data"):
            shutil.copytree(source / name, instance / name)
        for name in ("server.cnf", "client.private.cnf", "login.private.cnf"):
            shutil.copy2(source / name, instance / name)
        server = subprocess.Popen([str(instance / "mysql/bin/mysqld.exe"),
            "--defaults-file=" + str(instance / "server.cnf"), "--port=" + str(args.port),
            "--bind-address=127.0.0.1", "--basedir=" + str(instance / "mysql"),
            "--datadir=" + str(instance / "data"), "--log-error=" + str(instance / "error.log"),
            "--pid-file=" + str(instance / "mysql.pid")], cwd=instance, startupinfo=hidden_startup())

        def query(sql, private="client.private.cnf", check=True):
            result = subprocess.run([str(instance / "mysql/bin/mysql.exe"),
                "--defaults-file=" + str(instance / private), "--port=" + str(args.port),
                "--batch", "--skip-column-names", "--execute=" + sql], cwd=instance,
                capture_output=True, timeout=15)
            if check: assert result.returncode == 0, "Private-copy SQL failed (details retained locally)"
            return result

        process = window = None
        try:
            wait_for(lambda: server.poll() is None and query("SELECT 1", check=False).returncode == 0, "copied MySQL ready", 60)
            # This fixture only captures args/env/cwd; it does not simulate game behavior.
            subprocess.run([str(TOOLCHAIN / "x86_64-w64-mingw32-clang.exe"), "-O2", "-static",
                "-municode", "-mwindows", str(ROOT / "tests/login_client.c"), "-o", str(client / "dnf.exe")], check=True)
            import configparser
            cnf = configparser.ConfigParser(interpolation=None)
            cnf.read(instance / "login.private.cnf", encoding="utf-8-sig")
            account = "login" + secrets.token_hex(4)
            process, window = open_launcher()
            for identifier, value in {1001: "127.0.0.1", 1002: str(args.port), 1003: "game",
                1004: cnf["client"]["password"].strip('"'), 1005: account, 1006: secrets.token_hex(8)}.items():
                text = c.create_unicode_buffer(value)
                user.SendMessageW(user.GetDlgItem(window, identifier), 0xC, 0, c.addressof(text))
            def connection_failure(expected_title):
                dialog = wait_for(lambda: next((h for h in windows(process.pid, "#32770") if title(h) == expected_title), None), expected_title, 15)
                button = wait_for(lambda: next((i for i in (1, 2) if user.GetDlgItem(dialog, i)), None), "failure close button", 10)
                click(dialog, button)
                wait_for(lambda: not user.IsWindow(dialog), "failure dialog closed")

            wrong_password = c.create_unicode_buffer("wrong-" + secrets.token_hex(12))
            user.SendMessageW(user.GetDlgItem(window, 1004), 0xC, 0, c.addressof(wrong_password))
            for button in (1008, 1009):
                click(window, button)
                connection_failure("数据库认证失败")
                assert "账号和密码" in title(user.GetDlgItem(window, 1007))
            checks["wrong_database_password_identified_for_register_and_login"] = True
            password = c.create_unicode_buffer(cnf["client"]["password"].strip('"'))
            user.SendMessageW(user.GetDlgItem(window, 1004), 0xC, 0, c.addressof(password))
            click(window, 1008)
            wait_for(lambda: "账号创建成功" in title(user.GetDlgItem(window, 1007)), "four-table registration", 20)
            uid = query("SELECT UID FROM d_taiwan.accounts WHERE accountname='" + account + "'").stdout.decode().strip()
            assert uid.isdigit()
            counts = query("SELECT " + ",".join("(SELECT COUNT(*) FROM " + table + " WHERE " + column + "=" + uid + ")" for table, column in [
                ("d_taiwan.accounts", "UID"), ("d_taiwan.member_info", "m_id"),
                ("d_taiwan.member_white_account", "m_id"), ("taiwan_login.member_login", "m_id")])).stdout.decode().strip()
            assert counts == "1\t1\t1\t1"
            checks["game_registration_real_four_table_rows"] = True
            click(window, 1009)
            capture = client / "client-launch.txt"
            wait_for(capture.exists, "client launch capture", 20)
            token, cwd, address = capture.read_bytes().decode("utf-16-le").splitlines()
            assert Path(cwd) == client
            assert address == os.environ.get("GAME_SERVER_IP", "127.0.0.1")
            modulus, exponent = public_numbers((ROOT / "resources/login/publickey.pem").read_bytes())
            signature = int.from_bytes(base64.b64decode(token), "big")
            recovered = pow(signature, exponent, modulus).to_bytes((modulus.bit_length() + 7) // 8, "big")
            prefix, payload = recovered[2:].split(b"\0", 1)
            assert recovered[:2] == b"\0\1" and set(prefix) == {255}
            assert int.from_bytes(payload[:4], "big") == int(uid)
            checks["real_login_token_verifies_with_server_default_key"] = True
            checks["unicode_client_path_and_working_directory"] = True
            checks["game_address_passed_to_child"] = True
        finally:
            if process: close_launcher(process, window)
            subprocess.run([str(instance / "mysql/bin/mysqladmin.exe"),
                "--defaults-file=" + str(instance / "client.private.cnf"), "--port=" + str(args.port), "shutdown"],
                cwd=instance, capture_output=True, timeout=30, check=True)
            server.wait(timeout=30)
            assert server.returncode == 0
    record = {"checks": checks, "passed": all(checks.values()), "launcher_sha256": digest(args.launcher),
        "os": sys.getwindowsversion()[:], "scope": "Actual independent launcher GUI, key export, optional copied database registration and launch contract; no game-client business claim."}
    (work / "receipt.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print("Launcher checks passed:", work / "receipt.json")


if __name__ == "__main__":
    main()
