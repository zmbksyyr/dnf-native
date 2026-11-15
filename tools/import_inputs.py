"""Verify and extract input assets into ignored copies, without running installers."""

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import tarfile

from bootstrap import ORIGINALS

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, default=ROOT.parent)
    args = parser.parse_args()
    evidence = json.loads((ROOT / "docs/evidence/resource-inventory.json").read_text(encoding="utf-8"))
    for name, subdir in [("DOF.tar.gz", "dp123"), ("MySQL.tar.gz", "database-source")]:
        row = next(r for r in evidence["input_files"] if PurePosixPath(r["path"]).name == name)
        source = args.workspace / row["path"]
        if digest(source) != row["sha256"]:
            raise ValueError(f"Input archive changed: {name}")
        destination = ORIGINALS / subdir
        destination.mkdir(parents=True, exist_ok=True)
        count = 0
        with tarfile.open(source, "r|gz") as package:
            for member in package:
                path = (destination / member.name).resolve()
                if not path.is_relative_to(destination.resolve()):
                    raise ValueError("Archive path outside destination")
                if member.isdir():
                    path.mkdir(parents=True, exist_ok=True)
                    continue
                if not member.isfile():
                    raise ValueError("Unexpected archive link or special file")
                content = package.extractfile(member)
                path.parent.mkdir(parents=True, exist_ok=True)
                expected = hashlib.sha256()
                if path.exists():
                    while chunk := content.read(1024 * 1024):
                        expected.update(chunk)
                    if digest(path) != expected.hexdigest():
                        raise ValueError(f"Existing input copy changed: {path}")
                else:
                    with path.open("xb") as output:
                        while chunk := content.read(1024 * 1024):
                            output.write(chunk)
                count += 1
        print(f"Verified source and prepared {count} files: {destination}", flush=True)


if __name__ == "__main__":
    main()
