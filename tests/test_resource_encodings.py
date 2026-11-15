"""Verify actual simplified/traditional Chinese and Korean conversion from native.zip."""
import argparse
import datetime
import json
from pathlib import Path
import platform
import subprocess
import zipfile

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from bootstrap import sha256, BUILD

from bootstrap import TOOLCHAIN

ROOT = Path(__file__).resolve().parents[1]
BIN = TOOLCHAIN


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, required=True)
    args = parser.parse_args()
    package = args.package.resolve(strict=True)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    work = BUILD / "probes" / stamp / "resource-encodings"
    work.mkdir(parents=True)
    with zipfile.ZipFile(package / "resources/native.zip") as z:
        for entry in z.infolist():
            if entry.filename.startswith(("lib/", "usr/lib/")):
                z.extract(entry, work)
    root = work
    fixture = root / "encoding-check"
    source = ROOT / "tests/resource_encodings.c"
    command = [str(BIN / "clang.exe"), "--target=i386-linux-gnu", "-fuse-ld=lld", "-nostdlib",
               "-fno-stack-protector", "-fno-pic", "-no-pie", "-O2", "-Wall", "-Wextra", "-Werror",
               "-Wl,-e,_start", "-Wl,--image-base=0x8048000", "-Wl,--dynamic-linker=/lib/ld-2.17.so",
               str(source), "-L" + str(root / "lib"), "-l:libc.so.6", "-o", str(fixture)]
    subprocess.run(command, check=True)
    host, ld = package / "resources/dnf-host-x86.exe", root / "lib/ld-2.17.so"
    command = [str(host), "--root", str(root), sha256(ld), "--run", str(fixture), sha256(fixture), "encoding-check"]
    result = subprocess.run(command, capture_output=True, timeout=30)
    (work / "stderr.private.log").write_bytes(result.stderr)
    record = {"os": platform.platform(), "recorded_utc": stamp, "source_sha256": sha256(source),
              "native_zip_sha256": sha256(package / "resources/native.zip"), "host_sha256": sha256(host),
              "exit_code": result.returncode, "passed": result.returncode == 42,
              "conversions": "GBK, GB18030, BIG5, BIG5-HKSCS, EUC-KR, UHC to/from UTF-8; exact bytes"}
    (work / "receipt.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print("Resource encoding receipt:", work / "receipt.json", "PASS" if record["passed"] else "FAIL", flush=True)
    if not record["passed"]:
        raise SystemExit("Encoding fixture exit " + str(result.returncode))


if __name__ == "__main__":
    main()
