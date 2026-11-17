"""Check zero-fill semantics and resident-page reduction on both real hosts."""
import datetime
import json
from pathlib import Path
import platform
import queue
import subprocess
import sys
import threading

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from bootstrap import BUILD,TOOLCHAIN,sha256
from runtime_support import unpack_runtime,process_memory

ROOT=Path(__file__).resolve().parents[1]


def main():
    stamp=datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    probe=BUILD/"probes"/stamp/"zero-pages"
    records=[]
    for arch,triple,base,lib in [("i686","i386-linux-gnu","0x8048000","lib"),
                                ("x86_64","x86_64-linux-gnu","0x400000","lib64")]:
        directory=unpack_runtime(probe/arch,arch)
        program,loader=directory/"zero-pages",directory/lib/"ld-2.17.so"
        command=[str(TOOLCHAIN/"clang.exe"),"--target="+triple,"-fuse-ld=lld","-nostdlib",
                 "-fno-stack-protector","-fno-pic","-no-pie","-fno-builtin","-O2","-Wall","-Wextra","-Werror",
                 "-Wl,--image-base="+base,"-Wl,-e,_start","-Wl,--dynamic-linker=/"+lib+"/ld-2.17.so",
                 str(ROOT/"tests/zero_pages.c"),"-L"+str(directory/lib),"-l:libc.so.6","-o",str(program)]
        subprocess.run(command,check=True)
        host=BUILD/arch/"dnf-host.exe"
        with (directory/"stderr.log").open("wb") as err:
            child=subprocess.Popen([str(host),"--root",str(directory),sha256(loader),"--run",str(program),sha256(program),"zero-pages"],
                                   stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err)
            lines=queue.Queue()
            def read_lines():
                for line in child.stdout: lines.put(line)
            reader=threading.Thread(target=read_lines,daemon=True)
            reader.start()
            record={"arch":arch,"host_sha256":sha256(host),"fixture_sha256":sha256(program),"forced_cleanup":False}
            try:
                assert lines.get(timeout=20)==b"dirty\n"
                record["dirty"]=process_memory(child.pid)
                child.stdin.write(b"a");child.stdin.flush()
                assert lines.get(timeout=20)==b"zero\n"
                record["zero"]=process_memory(child.pid)
                saved=record["dirty"]["working_bytes"]-record["zero"]["working_bytes"]
                assert saved>40*1024*1024,record
                record["resident_bytes_saved"]=saved
                child.stdin.write(b"b");child.stdin.flush()
                assert lines.get(timeout=20)==b"zero-pages-ok\n"
                assert child.wait(timeout=10)==42
            except Exception as error:
                record["error"]=type(error).__name__+": "+str(error)
            finally:
                if child.poll() is None:
                    record["forced_cleanup"]=True;child.terminate();child.wait(timeout=10)
                child.stdin.close();reader.join(timeout=2);child.stdout.close()
        text=(directory/"stderr.log").read_text(encoding="utf-8",errors="replace")
        record["exit_code"]=child.returncode
        record["gaps"]=[line for line in text.splitlines() if any(word in line for word in
                        ["syscall gap","unhandled exception","zero-page memset failed","unsupported"])]
        record["hook_recorded"]="zero-page memset sha256=" in text
        shim=directory/"zero-interpose.so"
        subprocess.run([str(TOOLCHAIN/"clang.exe"),"--target="+triple,"-fuse-ld=lld","-nostdlib","-shared",
                        "-fPIC","-fno-stack-protector","-fno-builtin","-O2","-DNR_INTERPOSE",
                        str(ROOT/"tests/zero_pages.c"),"-o",str(shim)],check=True)
        interposed=subprocess.run([str(host),"--root",str(directory),sha256(loader),
                                   "--env","LD_PRELOAD=/zero-interpose.so","--env","ZERO_INTERPOSE=1",
                                   "--run",str(program),sha256(program),"zero-pages"],capture_output=True,timeout=20)
        record["preload_preserved"]=interposed.returncode==42 and interposed.stdout==b"interpose-ok\n" and b"zero-page memset sha256=" not in interposed.stderr
        (directory/"interposed.stderr.log").write_bytes(interposed.stderr)
        record["passed"]="error" not in record and not record["forced_cleanup"] and not record["gaps"] and record["hook_recorded"] and record["preload_preserved"]
        records.append(record)
        print(arch,"zero-page memset",record["passed"],"saved",record.get("resident_bytes_saved"),flush=True)
    receipt={"os":platform.platform(),"results":records,"passed":all(r["passed"] for r in records),
             "script_sha256":sha256(Path(__file__)),"source_sha256":sha256(ROOT/"tests/zero_pages.c"),
             "scope":"Real packaged glibc memset: dirty anonymous pages, exact zero bytes, unaligned boundaries, return pointer, byte conversion, nonzero/executable fallbacks, shared aliases, file COW and preserved preloaded interposition."}
    (probe/"receipt.json").write_text(json.dumps(receipt,indent=2)+"\n",encoding="utf-8")
    print(probe/"receipt.json",flush=True)
    if not receipt["passed"]:raise SystemExit("Zero-page checks failed")


if __name__=="__main__":main()
