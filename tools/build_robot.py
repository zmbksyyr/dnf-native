"""Build the optional Windows Robot module from the sibling source checkout."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

from bootstrap import BUILD, external_output

ROOT = Path(__file__).resolve().parents[1]


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT.parent / "dnf-robot")
    parser.add_argument("--output", type=Path, default=BUILD / "robot")
    args = parser.parse_args()
    source = args.source.resolve(strict=True)
    output = external_output(args.output)
    output.mkdir(parents=True, exist_ok=True)
    binary = output / "robot.exe"
    env = dict(os.environ)
    env["CGO_ENABLED"] = "0"
    env["GOOS"] = "windows"
    env["GOARCH"] = "amd64"
    env["GOMODCACHE"] = str((BUILD / ".go-mod").resolve())
    env["GOCACHE"] = str((BUILD / ".go-cache").resolve())
    command = ["go", "build", "-trimpath", "-ldflags=-s -w", "-o", str(binary), "./cmd/robot"]
    subprocess.run(command, cwd=source, env=env, check=True)
    receipt = {
        "source": str(source), "command": command, "goos": "windows", "goarch": "amd64",
        "cgo": False, "output_sha256": sha256(binary), "output_bytes": binary.stat().st_size,
        "scope": "Native Windows Robot module; Linux market memory and shell operations remain explicit unsupported paths."
    }
    (output / "build-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print("Robot:", binary, binary.stat().st_size, "bytes", flush=True)


if __name__ == "__main__":
    main()
