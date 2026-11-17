"""Real game method checks for all selectable caps and growth request parsing."""
import datetime
import argparse
import json
from pathlib import Path
import platform
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from bootstrap import BUILD, sha256
from runtime_support import unpack_runtime

ROOT = Path(__file__).resolve().parents[1]
LEVELS = [50,55,60,65,70,75,80,85,86,90,95,99,100]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--work", type=Path, help="Reuse an isolated fixture under ../x")
    parser.add_argument("--level", type=int, choices=LEVELS)
    args = parser.parse_args()
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    work = (args.work or BUILD / "probes" / stamp / "game-rules").resolve()
    work.relative_to((ROOT.parent / "x").resolve())
    if not (work / "home/neople/game/df_game_r").exists():
        unpack_runtime(work,"i686",("lib/","usr/","home/neople/game/","dp2/"))
    for name in ("log","pid","history"):
        (work / "home/neople/game" / name).mkdir(exist_ok=True)
    (work / "dp2/df_game_r.js").write_bytes((ROOT / "tests/game_rules.js").read_bytes())
    elf, ld, host = work / "home/neople/game/df_game_r", work / "lib/ld-2.17.so", BUILD / "i686/dnf-host.exe"
    original = sha256(elf)
    record = {"os":platform.platform(),"host_sha256":sha256(host),"elf_sha256":original,
              "scope":"Original ELF methods and request parsing", "checks":[]}
    for level in ([args.level] if args.level else LEVELS):
        (work / "dp2/df_game_r.lua").write_text('local f=require("df.frida")\nassert(f.load("' + str(level) + '",function() return 0 end)==42)\nos.exit(42)\n')
        command = [str(host),"--root",str(work),sha256(ld),"--env","HOME=/home/neople","--env","LD_PRELOAD=/dp2/libdp2pre.so",
                   "--env","LD_LIBRARY_PATH=/dp2/lib:/home/neople/game","--env","DNF_NATIVE_MAX_LEVEL="+str(level),
                   "--cwd","/home/neople/game","--run",str(elf),original,"/home/neople/game/df_game_r","siroco11","nofork"]
        with (work / (str(level)+".stdout.log")).open("wb") as out, (work / (str(level)+".stderr.log")).open("wb") as err:
            result = subprocess.run(command,stdout=out,stderr=err,timeout=60)
        result.stdout = (work / (str(level)+".stdout.log")).read_bytes()
        result.stderr = (work / (str(level)+".stderr.log")).read_bytes()
        assert result.returncode == 42 and ("game-rules-pass level="+str(level)).encode() in result.stdout, (level,result.returncode,result.stdout[-1200:],result.stderr[-1000:])
        assert result.stderr.count(b"game level sha256=") == 15 and b"unhandled exception" not in result.stderr
        record["checks"].append({"level":level,"exit_code":42,"passed":True})
        print("Game rules:",level,"PASS",flush=True)
    bad = command.copy(); bad[bad.index("DNF_NATIVE_MAX_LEVEL="+str(level))] = "DNF_NATIVE_MAX_LEVEL=101"
    result = subprocess.run(bad,capture_output=True,timeout=10)
    assert result.returncode == 1 and b"unsupported maximum level" in result.stderr
    record["unsupported_level_rejected"] = True
    record["original_elf_unchanged"] = sha256(elf) == original
    record["passed"] = record["original_elf_unchanged"]
    (work / "receipt.json").write_text(json.dumps(record,indent=2)+"\n")
    print(work / "receipt.json",flush=True)


if __name__ == "__main__":
    main()
