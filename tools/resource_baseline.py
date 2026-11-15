"""Read the repository's pinned resource inputs without changing them."""

import json
from pathlib import Path

from bootstrap import sha256

ROOT = Path(__file__).resolve().parents[1]
RESOURCES = ROOT / "resources"


def verify_baseline(folder=RESOURCES):
    folder = Path(folder).resolve(strict=True)
    record = json.loads((folder / "baseline.json").read_text(encoding="utf-8"))
    for name in ("native.zip", "mysql.zip", "baseline.ini"):
        metadata = record["files"][name]
        path = folder / name
        if path.stat().st_size != metadata["bytes"] or sha256(path) != metadata["sha256"]:
            raise ValueError("Resource baseline differs: " + name)
    return record
