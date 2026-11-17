"""Check entry preservation and the real DPX minimum-level API in a private ELF."""
import datetime
import json
from pathlib import Path
import platform
import subprocess
import sys

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from bootstrap import BUILD, TOOLCHAIN, sha256
from game_fixture import main as game_fixture
import game_fixture as fixture

ROOT=Path(__file__).resolve().parents[1]
PROBE=r'''
local operand = _DPX.reloc(0x08213f29)
local function current() return _DP.mem.read(operand,0) & 255 end
assert(current()==30)
for _,level in ipairs({70,10,30,0,127}) do
    _DPX.set_auction_min_level(level)
    assert(current()==level)
end
for _,value in ipairs({-1,128,10.5,"10",true,{}}) do
    assert(not pcall(_DPX.set_auction_min_level,value))
    assert(current()==127)
end
local uv=require("luv")
local timer=uv.new_timer()
timer:start(20,0,function()
    _DPX.set_auction_min_level(10)
    assert(current()==10)
    timer:close()
end)
uv.run()
assert(current()==10)
print("GAME_COMPAT_API_PASS requested operands, invalid inputs unchanged, delayed callback")
os.exit(42)
'''

def main():
    stamp=datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    work=BUILD/"probes"/stamp/"game-compat"
    work.mkdir(parents=True)
    exe=work/"installer.exe"
    command=[str(TOOLCHAIN/"x86_64-w64-mingw32-clang++.exe"),"-std=c++17","-Wall","-Wextra","-Werror",
             "-O2","-static","-municode",str(ROOT/"tests/game_compat.cpp"),"-lshell32","-ladvapi32","-o",str(exe)]
    subprocess.run(command,check=True)
    installed=subprocess.run([str(exe),str(work/"entry")],capture_output=True,check=True)
    (work/"installer.log").write_bytes(installed.stdout+installed.stderr)
    lua=work/"minimum-level.lua"
    lua.write_bytes((work/"entry/production-compat.lua").read_bytes()+PROBE.encode())
    # Reuse the repository fixture: original game/DP2, independent ZIP copy,
    # Database access is supplied by the deterministic fixture callbacks.
    fixture.BUILD=work
    sys.argv=["game_fixture.py","--host",str(BUILD/"i686/dnf-host.exe"),"--lua-script",str(lua),"--timeout","30"]
    game_fixture()
    evidence=json.loads((work/"game-probe.json").read_text())
    stdout=next(work.glob("probes/*/game-fixture/stdout.log")).read_bytes()
    assert b"GAME_COMPAT_API_PASS" in stdout
    record={"os":platform.platform(),"header_sha256":sha256(ROOT/"src/controller/game_compat.h"),
            "installer_passed":b"GAME_COMPAT_INSTALL PASS" in installed.stdout,
            "api_passed":evidence["exit_code"]==42 and not evidence["forced_cleanup"],
            "elf_sha256":evidence["elf_sha256"],"host_sha256":evidence["host_sha256"],
            "scope":"Production installer file preservation and real DPX operand API, including delayed callback"}
    (work/"receipt.json").write_text(json.dumps(record,indent=2)+"\n")
    print("Game compatibility: PASS",work/"receipt.json",flush=True)

if __name__=="__main__": main()
