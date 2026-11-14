"""Observe original security services in a private copy; does not assert readiness."""

import argparse
import datetime
import json
from pathlib import Path
import platform
import shutil
import subprocess

from test_bridge import sha
from test_channel import request_stop

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", type=Path, default=ROOT / "build/i686/dnf-host.exe")
    parser.add_argument("--observe-seconds", type=float, default=5)
    args = parser.parse_args()
    if args.observe_seconds <= 0:
        parser.error("--observe-seconds must be positive")
    host = args.host.resolve()
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    private = ROOT / "build/probes" / stamp / "security"
    private.mkdir(parents=True)
    for name in ["lib", "usr"]:
        shutil.copytree(ROOT / ".local/sysroot/i686" / name, private / name)
    source = ROOT / "originals/dp123/home/neople/secsvr"
    shutil.copytree(source, private / "home/neople/secsvr")
    for name in ["dev/shm", "tmp"]:
        (private / name).mkdir(parents=True)
    ld = private / "lib/ld-2.17.so"
    services = []
    receipt = {"recorded_utc": stamp, "os": platform.platform(),
               "probe": str(private.relative_to(ROOT)), "observe_seconds": args.observe_seconds,
               "scope": "Unmodified security ELF/config startup observation. No readiness, game interaction, client or business acceptance.",
               "results": []}
    try:
        for name, directory, arguments in [("gunnersvr", "gunnersvr", ["-t30", "-i1"]),
                                            ("secagent", "zergsvr", []),
                                            ("zergsvr", "zergsvr", ["-t30", "-i1"])]:
            guest = private / "home/neople/secsvr" / directory
            elf = guest / name
            record = {"name": name, "host_sha256": sha(host), "elf_sha256": sha(elf),
                      "loader_sha256": sha(ld), "arguments": arguments,
                      "configuration_sha256": {str(p.relative_to(guest)): sha(p)
                                                for p in sorted((guest / "cfg").rglob("*")) if p.is_file()}}
            command = [str(host), "--root", str(private), sha(ld), "--env", "HOME=/home/neople",
                       "--cwd", "/home/neople/secsvr/" + directory, "--run", str(elf), sha(elf),
                       "/home/neople/secsvr/" + directory + "/" + name, *arguments]
            with (private / (name + "-stdout.private.log")).open("wb") as out, (private / (name + "-stderr.log")).open("wb") as err:
                process = subprocess.Popen(command, stdout=out, stderr=err)
            services.append((process, guest, record))
            receipt["results"].append(record)
            try:
                process.wait(timeout=args.observe_seconds)
            except subprocess.TimeoutExpired:
                pass
    finally:
        markers = ["ZEN_Reactor and ZEN_Epoll_Reactor initialized.", "MMAP Pipe init success",
                   "Comm_Svrd_Appliction::init_instance Success.", "init gamesvr channel succ",
                   "init user info mngr succ", "update dpsdk config info succ",
                   "connect monitor failed", "monitor reporter init fail. ret=5092",
                   "Not pull config but get ip from cfgsvr", "init_instance fail. ret=290010039",
                   "restrict function will closed"]
        for process, guest, record in reversed(services):
            name = record["name"]
            record["forced_cleanup"] = False
            record["stop_signal"] = None
            if process.poll() is None:
                record["stop_signal"] = 15
                request_stop(process.pid)
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    record["forced_cleanup"] = True
                    process.terminate()
                    process.wait(timeout=5)
            record["exit_code"] = process.returncode
            stderr = (private / (name + "-stderr.log")).read_text(encoding="utf-8", errors="replace")
            record["gaps"] = [line for line in stderr.splitlines()
                              if any(x in line for x in ["syscall gap", "unhandled exception", "unsupported", "exhausted", "mapping failed"])]
            logs = [private / (name + "-stdout.private.log"), private / (name + "-stderr.log")]
            logs += [p for p in (guest / "log").glob(name + "*") if p.is_file()]
            text = "\n".join(p.read_text(encoding="utf-8", errors="replace") for p in logs)
            record["markers"] = [marker for marker in markers if marker in text]
            record["logs"] = {str(p.relative_to(private)): sha(p) for p in logs}
            record["shared_user_info_bytes"] = sum(p.stat().st_size for p in guest.rglob("PLAT_USER_INFO_*.MMAP"))
        (private / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        (ROOT / "build/security-probe.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        print("Security observation:", private / "receipt.json", flush=True)
        for record in receipt["results"]:
            print(record["name"], "exit", record["exit_code"], "forced", record["forced_cleanup"], "gaps", record["gaps"], flush=True)


if __name__ == "__main__":
    main()
