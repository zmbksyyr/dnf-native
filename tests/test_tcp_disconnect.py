"""Check Linux poll/epoll read readiness at real TCP FIN and reset."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import socket
import struct
import subprocess
import sys

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from bootstrap import ROOT,BUILD,TOOLCHAIN


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hosts",type=Path,default=BUILD)
    args=parser.parse_args()
    work=BUILD/"probes"/datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")/"tcp-disconnect"
    work.mkdir(parents=True)
    env=dict(os.environ,PATH=str(TOOLCHAIN)+os.pathsep+os.environ.get("PATH",""))
    records=[]
    for arch,target,base in [("i686","i386-linux-gnu","0x8048000"),("x86_64","x86_64-linux-gnu","0x400000")]:
        for reset in (False,True):
            with socket.socket() as listener:
                listener.bind(("127.0.0.1",0));listener.listen();listener.settimeout(5)
                fixture=work/(arch+("-reset" if reset else "-fin"))
                subprocess.run([str(TOOLCHAIN/"clang.exe"),"--target="+target,"-fuse-ld=lld","-nostdlib","-static",
                    "-ffreestanding","-fno-stack-protector","-fno-pic","-O2","-Wall","-Wextra","-Werror",
                    "-Wl,--image-base="+base,"-Wl,-e,_start","-DNR_PORT="+str(listener.getsockname()[1]),
                    "-DNR_RESET="+str(int(reset)),str(ROOT/"tests/tcp_disconnect.c"),"-o",str(fixture)],check=True,env=env)
                host=args.hosts.resolve()/arch/"dnf-host.exe"
                process=subprocess.Popen([str(host),"--run",str(fixture),hashlib.sha256(fixture.read_bytes()).hexdigest(),"disconnect"],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
                with listener.accept()[0] as peer:
                    peer.settimeout(5);assert peer.recv(1)==b"x"
                    if reset:peer.setsockopt(socket.SOL_SOCKET,socket.SO_LINGER,struct.pack("HH",1,0))
                stdout,stderr=process.communicate(timeout=10)
                observed=list(struct.unpack("<ii",stdout)) if len(stdout)==8 else []
                record={"arch":arch,"reset":reset,"observed_poll_epoll":observed,"exit_code":process.returncode,
                    "passed":process.returncode==42 and len(observed)==2 and all(e&1 for e in observed),"host_sha256":hashlib.sha256(host.read_bytes()).hexdigest(),
                    "stderr":stderr.decode("utf-8",errors="replace")}
                records.append(record)
                print(arch,"RST" if reset else "FIN","PASS" if record["passed"] else "FAIL",observed,flush=True)
    (work/"receipt.json").write_text(json.dumps({"os":platform.platform(),"results":records},indent=2)+"\n",encoding="utf-8")
    print("TCP disconnect receipt:",work/"receipt.json")
    if not all(r["passed"] for r in records):raise SystemExit(1)


if __name__=="__main__":main()
