"""Check the verified i386 glibc query gateway and its unchanged fallbacks."""
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from bootstrap import BUILD, TOOLCHAIN
from runtime_support import unpack_runtime, request_stop

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    directory = unpack_runtime(BUILD / "fixtures/sysinfo/i686", "i686")
    program, loader = directory / "sysinfo", directory / "lib/ld-2.17.so"
    build = [str(TOOLCHAIN / "clang.exe"), "--target=i386-linux-gnu", "-fuse-ld=lld", "-nostdlib",
             "-fno-stack-protector", "-fno-pic", "-no-pie", "-msse2", "-O2", "-Wall", "-Wextra", "-Werror",
             "-Wl,--image-base=0x8048000", "-Wl,-e,_start", "-Wl,--dynamic-linker=/lib/ld-2.17.so",
             str(ROOT / "tests/sysinfo.c"), "-L" + str(directory / "lib"), "-l:libpthread.so.0", "-l:libc.so.6",
             "-o", str(program)]
    subprocess.run(build, check=True)
    host = BUILD / "i686/dnf-host.exe"
    command = [str(host), "--root", str(directory), sha(loader), "--run", str(program), sha(program), "sysinfo"]
    records = []
    for trace in (False, True):
        env = dict(os.environ)
        env.pop("DNF_NATIVE_TRACE", None)
        if trace:
            env["DNF_NATIVE_TRACE"] = "1"
        log = directory / ("trace.stderr.log" if trace else "direct.stderr.log")
        error, marker = None, b""
        with log.open("wb") as stream:
            process = subprocess.Popen(command, env=env, stdout=subprocess.PIPE, stderr=stream)
            try:
                marker = process.stdout.readline()
                if marker != b"sysinfo-signal-ready\n":
                    raise RuntimeError("sysinfo receiver did not reach fast-loop signal check")
                request_stop(process.pid, 12)
                stdout, _ = process.communicate(timeout=30)
            except (OSError, RuntimeError, subprocess.TimeoutExpired) as failure:
                error = str(failure)
                process.terminate()
                stdout, _ = process.communicate(timeout=5)
        stderr = log.read_text(encoding="utf-8", errors="replace")
        passed = error is None and process.returncode == 42 and stdout == b"native-sysinfo-state-ok\n" and "syscall gap" not in stderr
        if trace:
            passed = passed and "nr=13 " in stderr and "nr=20 " in stderr and "nr=224 " in stderr
        records.append({"arch": "i686", "case": "trace-fallback" if trace else "query-state",
                        "command": command, "host_sha256": sha(host), "fixture_sha256": sha(program),
                        "loader_sha256": sha(loader), "exit_code": process.returncode,
                        "stdout": (marker + stdout).decode(errors="replace"), "stderr": stderr, "error": error, "passed": passed})
        print("i686 sysinfo", "trace" if trace else "direct", "PASS" if passed else "FAIL", "exit", process.returncode, flush=True)
    receipt = {"recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "os": platform.platform(),
               "scope": "Original i386 glibc GS:16 queries: GPR/ESP/flags including DF, x87/XMM/MXCSR, raw-query comparison, changed callback, in-place detour and verified byte/protection restore, time(pointer)/EFAULT, pending and external signals during a query-only loop, pthread identity and enabled trace fallback. Not integrated game acceptance.",
               "build": build, "results": records}
    (BUILD / "sysinfo-checks.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    if not all(r["passed"] for r in records):
        raise SystemExit("sysinfo checks failed")


if __name__ == "__main__":
    main()
