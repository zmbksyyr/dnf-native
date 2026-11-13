"""Fetch the pinned upstream compiler into .local; never use reference builds."""

import hashlib
import json
from pathlib import Path
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = "20261104"
NAME = f"llvm-mingw-{VERSION}-msvcrt-x86_64"
URL = f"https://github.com/mstorsjo/llvm-mingw/releases/download/{VERSION}/{NAME}.zip"
SHA256 = "6f9c316699759f088de26f5ca0e1df35c1d4e8ee3600f31229821c5717fd0d2b"


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    cache = ROOT / ".local/downloads"
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / (NAME + ".zip")
    if not archive.exists() or sha256(archive) != SHA256:
        partial = archive.with_suffix(".part")
        print(f"Downloading {URL}", flush=True)
        request = urllib.request.Request(URL, headers={"User-Agent": "dnf-native-bootstrap"})
        with urllib.request.urlopen(request, timeout=45) as source, partial.open("wb") as output:
            while chunk := source.read(1024 * 1024):
                output.write(chunk)
        if sha256(partial) != SHA256:
            raise ValueError("Upstream compiler archive SHA-256 mismatch")
        partial.replace(archive)
    destination = ROOT / ".local/toolchains"
    compiler = destination / NAME / "bin/clang.exe"
    if not compiler.exists():
        destination.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(archive) as package:
            for info in package.infolist():
                path = (destination / info.filename).resolve()
                if not path.is_relative_to(destination.resolve()):
                    raise ValueError("Unsafe compiler archive path")
            package.extractall(destination)
    receipt = {"name": NAME, "url": URL, "archive_sha256": SHA256,
               "source": "Official mstorsjo/llvm-mingw release asset; GitHub published SHA-256",
               "role": "Development compiler only; not a product runtime dependency"}
    (ROOT / ".local/compiler.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(compiler)


if __name__ == "__main__":
    main()
