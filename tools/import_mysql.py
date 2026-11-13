"""Import pinned official MySQL tools/data files; version checks never start a server."""

import datetime
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import subprocess
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = "5.7.44"
NAME = "mysql-5.7.44-winx64"
URL = "https://cdn.mysql.com/archives/mysql-5.7/" + NAME + ".zip"
SHA256 = "aed661fe8120254a1dc30f5a4d5de346681922f4847cf025e2d4084eca78e70e"
TOOLS = ["mysqld.exe", "mysql.exe", "mysqladmin.exe", "mysqldump.exe", "mysql_upgrade.exe", "mysqlcheck.exe"]


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def signatures(destination):
    env = dict(os.environ)
    env["DNF_MYSQL_IMPORT_ROOT"] = str(destination)
    # Paths arrive through the environment; no path text is interpreted as PowerShell code.
    names = ",".join("'" + name + "'" for name in TOOLS)
    script = """$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding
$records = foreach ($name in NAMES) {
    $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $env:DNF_MYSQL_IMPORT_ROOT ('bin/' + $name))
    [pscustomobject]@{
        name = $name
        status = $signature.Status.ToString()
        signer_subject = $signature.SignerCertificate.Subject
        signer_thumbprint = $signature.SignerCertificate.Thumbprint
    }
}
ConvertTo-Json -InputObject @($records) -Depth 3 -Compress
""".replace("NAMES", "@(" + names + ")")
    process = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", script],
                             capture_output=True, env=env, timeout=60, check=True)
    return json.loads(process.stdout.decode("utf-8-sig"))


def main():
    archive = ROOT / ".local/downloads" / (NAME + ".zip")
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        partial = archive.with_suffix(".zip.part")
        print("Downloading", URL, flush=True)
        request = urllib.request.Request(URL, headers={"User-Agent": "dnf-native-mysql-import"})
        with urllib.request.urlopen(request, timeout=45) as source, partial.open("wb") as output:
            while chunk := source.read(1024 * 1024):
                output.write(chunk)
        if sha256(partial) != SHA256:
            raise ValueError("Official MySQL download SHA-256 mismatch; partial file retained")
        partial.replace(archive)
    if sha256(archive) != SHA256:
        raise ValueError("Cached MySQL archive SHA-256 mismatch; existing file retained")

    destination = ROOT / ".local/mysql-5.7.44"
    destination.mkdir(parents=True, exist_ok=True)
    selected = {"bin/" + name for name in TOOLS} | {"LICENSE", "README"}
    payload = {}
    added = 0
    with zipfile.ZipFile(archive) as package:
        for info in package.infolist():
            if info.is_dir():
                continue
            relative = info.filename.removeprefix(NAME + "/")
            if relative not in selected and not relative.startswith("share/"):
                continue
            path = (destination / PurePosixPath(relative)).resolve()
            if not path.is_relative_to(destination.resolve()):
                raise ValueError("MySQL archive entry outside destination")
            content = package.read(info)
            digest = hashlib.sha256(content).hexdigest()
            if path.exists():
                if not path.is_file() or sha256(path) != digest:
                    raise ValueError("Existing MySQL payload differs from official package: " + relative)
            else:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(content)
                added += 1
            payload[relative] = {"bytes": len(content), "sha256": digest}
    if not selected.issubset(payload) or not any(name.startswith("share/") for name in payload):
        raise ValueError("Official MySQL package is missing required tools or share files")

    versions = []
    for name in TOOLS:
        command = [str(destination / "bin" / name), "--no-defaults", "--version"]
        process = subprocess.run(command, capture_output=True, timeout=15)
        output = (process.stdout + process.stderr).decode("utf-8", "replace").strip()
        versions.append({"name": name, "command": command, "exit_code": process.returncode, "output": output,
                         "passed": process.returncode == 0 and VERSION in output and "x86_64" in output})
    authenticode = signatures(destination)
    for record in authenticode:
        record["verified_oracle_signature"] = (record["status"] == "Valid" and
                                                 "Oracle America, Inc." in (record["signer_subject"] or ""))
    passed = all(item["passed"] for item in versions) and all(item["verified_oracle_signature"] for item in authenticode)
    receipt = {"recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
               "os": platform.platform(), "name": NAME, "url": URL,
               "source": "Official Oracle MySQL archive ZIP; pinned SHA-256 and Windows Authenticode checks",
               "archive_bytes": archive.stat().st_size, "archive_sha256": SHA256,
               "destination": str(destination), "payload": payload,
               "tools": versions, "authenticode": authenticode, "passed": passed,
               "scope": "Official file import and --no-defaults --version only; no server started.",
               "limitations": ["Version checks use this developer machine's available Windows runtimes; local CRT dependency closure is not verified.",
                               "Clean Windows 7/10/11, offline operation and standalone packaging are not verified.",
                               "mysql_upgrade and mysqlcheck are development recovery tools; their inclusion in the final payload is not decided."]}
    (ROOT / ".local/mysql-import.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(f"MySQL {VERSION}: {len(payload)} official files verified, {added} missing files added", flush=True)
    print("Version and Oracle Authenticode checks:", "PASS" if passed else "FAIL", flush=True)
    if not passed:
        raise SystemExit("MySQL import checks failed; see .local/mysql-import.json")


if __name__ == "__main__":
    main()
