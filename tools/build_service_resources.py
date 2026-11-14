"""Build local service templates and an application-only seed for native first run."""

import argparse
import configparser
import datetime
import json
from pathlib import Path
import re
import shutil

from bootstrap import sha256
from prepare_service_instance import copy_service_resources

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="New resources directory beside the native GUI")
    parser.add_argument("--seed", type=Path, required=True, help="Verified application-only logical seed")
    parser.add_argument("--seed-sha256", required=True)
    args = parser.parse_args()
    seed = args.seed.resolve(strict=True)
    if sha256(seed) != args.seed_sha256:
        raise SystemExit("Application seed identity differs")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    db = {"host": "127.0.0.1", "port": 3306, "user": "dnf_game", "password": "@DNF_GAME_PASSWORD@", "database": "d_taiwan"}
    records = copy_service_resources(output, db, "@DNF_BRIDGE_PASSWORD@")
    # Keep the original product capacities. The development peer probe uses
    # smaller pools solely to reduce its repeated observation cost.
    for name, cfg in [("channel", "channel"), ("relay", "relay_200")]:
        original = (ROOT / "originals/dp123/home/neople" / name / "cfg" / (cfg + ".cfg")).read_text(encoding="utf-8")
        value = re.search(r"(?m)^max_client\s*=\s*(\d+)", original)[1]
        target = output / "i686/home/neople" / name / "cfg" / (cfg + ".cfg")
        text = re.sub(r"(?m)^max_client\s*=.*$", "max_client = " + value, target.read_text(encoding="utf-8"))
        target.write_text(text, encoding="utf-8")
    (output / "services.ini").rename(output / "services.template.ini")
    script = output / "i686/dp2/df_game_r.js"
    text = script.read_text(encoding="utf-8")
    for key, value in [("account", "dnf_frida"), ("password", "@DNF_FRIDA_PASSWORD@")]:
        text, count = re.subn(r"(var db_" + key + r"_ptr = Memory.allocUtf8String\(')[^']*('\))", lambda m: m[1] + value + m[2], text)
        if count != 1:
            raise RuntimeError("Original Frida database setting differs: " + key)
    script.write_text(text, encoding="utf-8")
    shutil.copy2(seed, output / "seed.sql")
    ini = configparser.ConfigParser(interpolation=None)
    files = sorted(path for path in output.rglob("*") if path.is_file())
    ini["resources"] = {"version": "1", "count": str(len(files)), "seed_sha256": args.seed_sha256}
    for index, path in enumerate(files):
        ini["file" + str(index)] = {"path": path.relative_to(output).as_posix(), "sha256": sha256(path)}
    with (output / "resources.ini").open("w", encoding="utf-16") as stream:
        ini.write(stream)
    receipt = {"recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
               "scope": "Untrimmed local first-run templates; not a minimized offline release or license/source closure.",
               "resource_count": len(files), "raw_bytes": sum(path.stat().st_size for path in files),
               "seed_sha256": args.seed_sha256, "manifest_sha256": sha256(output / "resources.ini"),
               "services": records,
               "sources": {name: sha256(ROOT / name) for name in ["tools/build_service_resources.py", "tools/prepare_service_instance.py", "runtime/dp2_cleanup.lua"]}}
    (output.parent / "resources-build-receipt.json").write_text(json.dumps(receipt, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")
    print("Built native first-run resources:", len(files), "files; seed", args.seed_sha256, flush=True)


if __name__ == "__main__":
    main()
