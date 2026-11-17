"""Test production market patches through the two actual original ELFs."""
from pathlib import Path
import hashlib
import json
import platform
import subprocess
import sys
from runtime_support import unpack_runtime
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from bootstrap import BUILD
ROOT = Path(__file__).resolve().parents[1]
sha = lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest()

def main():
    work = BUILD / "auction-methods"
    work.mkdir(exist_ok=True)
    root = work / "root"
    if not root.exists():
        unpack_runtime(root, "i686", ("lib/", "usr/", "home/neople/auction/", "home/neople/point/"))
    (root / "utf8.dat").write_bytes(("2675336 2 "+"1 "*11+"1 `测试金币` `` 13002\r\n"+
        "16400 2 "+"1 "*11+"85 `测试装备85` `` 11105\r\n"+
        "16401 2 "+"1 "*11+"100 `测试装备100` `` 11105\r\n"+
        "12400 0 "+"1 "*11+"1 `残破的牛皮短裤` `Cracked Leather Pants` 11103\r\n").encode("utf-8"))
    source = (ROOT / "src/runtime/run.c").read_text(encoding="utf-8")
    for old, new in {
        "static LONG CALLBACK trap_handler(EXCEPTION_POINTERS *exception) {": "void nr_probe_install(NrRun *run);\nint nr_probe_trap(EXCEPTION_POINTERS *exception);\nstatic LONG CALLBACK trap_handler(EXCEPTION_POINTERS *exception) {",
        "    if (!nr_current()) return EXCEPTION_CONTINUE_SEARCH;": "    if (!nr_current()) return EXCEPTION_CONTINUE_SEARCH;\n    if (nr_probe_trap(exception)) return EXCEPTION_CONTINUE_EXECUTION;",
        "    InitializeCriticalSection(&run->memory_lock);": "    InitializeCriticalSection(&run->memory_lock);\n    nr_probe_install(run);",
    }.items():
        assert source.count(old) == 1
        source = source.replace(old,new)
    private_run = work / "fixture_run.c"
    private_run.write_text(source,encoding="utf-8")
    build = next(x for x in json.loads((BUILD / "build-receipt.json").read_text()) if x["arch"] == "i686")
    assert all(sha(ROOT / n) == v for n,v in build["sources"].items())
    command = [str(private_run) if Path(a) == ROOT / "src/runtime/run.c" else a for a in build["command"]]
    host = work / "auction-methods.exe"
    command[command.index("-o")+1] = str(host)
    command += ["-I",str(ROOT / "src/runtime"),str(ROOT / "tests/auction_methods.c")]
    subprocess.run(command,cwd=ROOT,check=True)
    rows=[]
    for module in ["auction","point"]:
        elf = root / f"home/neople/{module}/df_{module}_r"
        before = sha(elf)
        args=[str(host),"--root",str(root),sha(root/"lib/ld-2.17.so"),"--env",f"LD_LIBRARY_PATH=/home/neople/{module}",
              "--env","DNF_NATIVE_AUCTION_LEVEL=100","--env","DNF_NATIVE_AUCTION_REFINE=10",
              "--env","DNF_NATIVE_AUCTION_PRICE_KEYS=1",
              "--env","DNF_NATIVE_AUCTION_ENCODING=UTF8","--cwd",f"/home/neople/{module}","--run",str(elf),before,f"/home/neople/{module}/df_{module}_r","fixture"]
        result=subprocess.run(args,capture_output=True,timeout=60)
        (work/f"{module}.stdout.log").write_bytes(result.stdout)
        (work/f"{module}.stderr.log").write_bytes(result.stderr)
        ok=result.returncode==42 and b"MARKET_METHOD PASS" in result.stdout and sha(elf)==before
        rows.append({"module":module,"passed":ok,"exit_code":result.returncode,"elf_unchanged":sha(elf)==before})
        print(module,"PASS" if ok else "FAIL",result.stdout.decode(errors="replace"),flush=True)
        if not ok: print(result.stderr.decode(errors="replace"),flush=True)
    (work/"receipt.json").write_text(json.dumps({"os":platform.platform(),"host_sha256":sha(host),"cases":rows,"scope":"Original functions after actual loader/ctors"},indent=2)+"\n",encoding="utf-8")
    assert all(x["passed"] for x in rows)

if __name__ == "__main__": main()
