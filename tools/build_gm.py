"""Build the independent GM tool from the sibling source checkout for bundling."""

import argparse
import json
import os
from pathlib import Path
import subprocess

from bootstrap import BUILD, external_output, sha256

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT.parent / "dnf-gm")
    parser.add_argument("--output", type=Path, default=BUILD / "gm")
    args = parser.parse_args()
    source = args.source.resolve(strict=True)
    output = external_output(args.output)
    output.mkdir(parents=True, exist_ok=True)
    temporary = BUILD / "tmp"
    temporary.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, DNF_GM_BUILD=str(output), TMP=str(temporary), TEMP=str(temporary),
               ZIG_LOCAL_CACHE_DIR=str(BUILD / ".zig-local-cache"),
               ZIG_GLOBAL_CACHE_DIR=str(BUILD / ".zig-global-cache"))
    command = ["cmd.exe", "/d", "/c", str(source / "tools/build.bat")]
    subprocess.run(command, cwd=source, env=env, check=True)
    binary = output / "DNF_GM.exe"
    names = subprocess.check_output(["git", "ls-files", "--cached", "--others", "--exclude-standard"],
                                    cwd=source, text=True).splitlines()
    sources = {name: sha256(source / name) for name in names
               if (source / name).is_file() and (name.startswith("src/") or name == "tools/build.bat")}
    receipt = {"source": str(source), "command": command, "sources": sources,
               "output_sha256": sha256(binary), "output_bytes": binary.stat().st_size,
               "source_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip(),
               "source_dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=source, text=True).strip()),
               "scope": "Independent Windows x64 GM; connection and runtime validation recorded separately."}
    (output / "build-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print("GM:", binary, binary.stat().st_size, "bytes", flush=True)


if __name__ == "__main__":
    main()
