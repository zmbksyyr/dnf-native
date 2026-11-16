"""Build both hosts, the database helper, GUI and a new instance package."""

import argparse
from pathlib import Path
import subprocess
import sys

from bootstrap import external_output, BUILD

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=BUILD / "package")
    args = parser.parse_args()
    args.output = external_output(args.output)
    for script, arguments in [
        ("build.py", []), ("build_mysql.py", []), ("build_gui.py", []), ("build_login.py", []),
        ("build_instance_package.py", ["--output", str(args.output)]),
    ]:
        subprocess.run([sys.executable, "-B", str(ROOT / "tools" / script), *arguments], cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
