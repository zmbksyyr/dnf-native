"""Import pinned Microsoft app-local runtimes; extract packages without installing them."""

import datetime
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import urllib.request
import zipfile

from bootstrap import sha256

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [
    ("7zr-26.06.exe", "https://github.com/ip7z/7zip/releases/download/26.06/7zr.exe",
     "e80db2dad715a6c5adabe292d31ed54e11f4e9b99ca349827b50573b175891bc"),
    ("7z2606-x64.exe", "https://github.com/ip7z/7zip/releases/download/26.06/7z2606-x64.exe",
     "9b697f65ecc2d9b0152f5ee96d72dde522a7827690349d9307935dacf40f42ec"),
    ("vc_redist-vs2019.x64.exe",
     "https://download.visualstudio.microsoft.com/download/pr/c5b955f2-cdce-405d-9f80-47276eb42576/6AFAE68A783F11292149175844AED0E2CE3F247BC0250F6CB18C931295B3F399/VC_redist.x64.exe",
     "6afae68a783f11292149175844aed0e2ce3f247bc0250f6cb18c931295b3f399"),
    ("microsoft.windows.sdk.cpp.10.0.19041.5.nupkg",
     "https://api.nuget.org/v3-flatcontainer/microsoft.windows.sdk.cpp/10.0.19041.5/microsoft.windows.sdk.cpp.10.0.19041.5.nupkg",
     "35074cafacc99a47e87c25641f98e0952576487fe29607103f83d61540ad0523"),
]


def download(name, url, digest):
    target = ROOT / ".local/downloads" / name
    target.parent.mkdir(parents=True, exist_ok=True)
    if not target.exists():
        partial = target.with_suffix(".part")
        with urllib.request.urlopen(url, timeout=45) as source, partial.open("wb") as output:
            while chunk := source.read(1024 * 1024):
                output.write(chunk)
        if sha256(partial) != digest:
            raise ValueError("Download SHA-256 differs: " + name)
        partial.replace(target)
    if sha256(target) != digest:
        raise ValueError("Cached SHA-256 differs: " + name)
    return target


def extract(program, archive, destination):
    result = subprocess.run([str(program), "x", str(archive), "-o" + str(destination), "-y"],
                            capture_output=True, timeout=60)
    if result.returncode:
        raise RuntimeError("Package extraction failed: " + str(archive))


def main():
    paths = [download(*source) for source in SOURCES]
    seven = ROOT / ".local/toolchains/7zip-26.06/7z.exe"
    extract(paths[0], paths[1], seven.parent)
    work = ROOT / ".local/sources/vc-redist-2019"
    extract(seven, paths[2], work)
    # These offsets belong to the hash-pinned 14.29.30157.0 Burn bundle.
    bundle = paths[2].read_bytes()
    offset = 684808
    size = struct.unpack_from("<I", bundle, offset + 8)[0]
    if bundle[offset:offset + 4] != b"MSCF" or size != 24638032:
        raise ValueError("Pinned VC runtime container differs")
    container = work / "container-1.cab"
    container.write_bytes(bundle[offset:offset + size])
    extract(seven, container, work / "packages")
    extract(seven, work / "packages/a12", work / "x64")

    destination = ROOT / ".local/mysql-runtime-x64"
    destination.mkdir(parents=True, exist_ok=True)
    origins = {}
    for name in ["msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"]:
        shutil.copyfile(work / "x64" / name, destination / name)
        origins[name] = "VC++ 2019 14.29.30157.0 x64 minimum runtime CAB"
    with zipfile.ZipFile(paths[3]) as package:
        prefix = "c/Redist/10.0.19041.0/ucrt/DLLs/x64/"
        for info in package.infolist():
            if info.filename.startswith(prefix) and info.filename.endswith(".dll"):
                name = Path(info.filename).name
                (destination / name).write_bytes(package.read(info))
                origins[name] = "Microsoft.Windows.SDK.CPP 10.0.19041.5: " + info.filename
    shutil.copyfile(work / "u4", destination / "VC-runtime-license.rtf")
    (destination / "UCRT-source.txt").write_text(
        "Microsoft Universal CRT, Windows SDK 10.0.19041.5\n"
        "Source: " + SOURCES[3][1] + "\nLicense: https://aka.ms/WinSDKLicenseURL\n", encoding="utf-8")

    env = dict(os.environ)
    env["DNF_RUNTIME_DLL_ROOT"] = str(destination)
    script = """[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding
$result = foreach ($file in Get-ChildItem -LiteralPath $env:DNF_RUNTIME_DLL_ROOT -Filter '*.dll') {
    $signature = Get-AuthenticodeSignature -LiteralPath $file.FullName
    [pscustomobject]@{name=$file.Name; status=$signature.Status.ToString();
        signer=$signature.SignerCertificate.Subject; version=$file.VersionInfo.FileVersion}
}
ConvertTo-Json -InputObject @($result) -Compress
"""
    result = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", script],
                            capture_output=True, env=env, timeout=60, check=True)
    signatures = json.loads(result.stdout.decode("utf-8-sig"))
    passed = len(origins) == 45 and {item["name"] for item in signatures} == set(origins) and all(item["status"] == "Valid" and
                                      "Microsoft" in (item["signer"] or "") for item in signatures)
    receipt = {"recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
               "sources": [{"name": name, "url": url, "sha256": digest} for name, url, digest in SOURCES],
               "development_only": [SOURCES[0][0], SOURCES[1][0]],
               "destination": str(destination), "authenticode": signatures, "passed": passed,
               "files": {p.name: {"bytes": p.stat().st_size, "sha256": sha256(p),
                                  "source": origins.get(p.name, "Microsoft license/source notice")}
                         for p in sorted(destination.iterdir()) if p.is_file()},
               "limits": ["Authenticode and pinned source verification only; clean Windows 7/11 execution not tested.",
                          "Windows 10 selects its system UCRT even when app-local UCRT files are present."]}
    (ROOT / ".local/mysql-runtime-import.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print("Microsoft app-local runtime:", len(origins), "DLLs; signatures", "PASS" if passed else "FAIL")
    if not passed:
        raise SystemExit("See .local/mysql-runtime-import.json")


if __name__ == "__main__":
    main()
