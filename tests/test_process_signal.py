"""Check cross-process SIGUSR2/SIGTERM and blocked delivery with native ELF pairs."""
import datetime
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import time

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from bootstrap import TOOLCHAIN, BUILD
from runtime_support import unpack_runtime, request_stop

ROOT = Path(__file__).resolve().parents[1]
BIN = TOOLCHAIN
ARCHES = {"i686": ("i386-linux-gnu", "0x8048000"), "x86_64": ("x86_64-linux-gnu", "0x400000")}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build(arch, name, target=None):
    triple, base = ARCHES[arch]
    path = BUILD / "fixtures" / arch / name
    path.parent.mkdir(parents=True, exist_ok=True)
    command = [str(BIN / "clang.exe"), "--target=" + triple, "-fuse-ld=lld", "-nostdlib", "-static",
               "-ffreestanding", "-fno-stack-protector", "-fno-pic", "-O2", "-Wall", "-Wextra", "-Werror",
               "-Wl,--image-base=" + base, "-Wl,-e,_start", str(ROOT / "tests/process_signal.c"), "-o", str(path)]
    if target is not None:
        command.append("-DNR_TARGET=" + str(target))
    subprocess.run(command, check=True)
    return path


def command(arch, fixture):
    return [str(BUILD / arch / "dnf-host.exe"), "--run", str(fixture), sha(fixture), "signal-fixture"]


def main():
    receivers = {arch: build(arch, "signal-receiver") for arch in ARCHES}
    records = []
    for arch, (triple, base) in ARCHES.items():
        for default in (False, True):
            fixture = BUILD / "fixtures" / arch / ("pipe-signal-default" if default else "pipe-signal")
            build_pipe = [str(BIN / "clang.exe"), "--target=" + triple, "-fuse-ld=lld", "-nostdlib", "-static",
                          "-ffreestanding", "-fno-stack-protector", "-fno-pic", "-O2", "-Wall", "-Wextra", "-Werror",
                          "-Wl,--image-base=" + base, "-Wl,-e,_start", str(ROOT / "tests/pipe_signal.c"), "-o", str(fixture)]
            if default:
                build_pipe.append("-DNR_DEFAULT")
            subprocess.run(build_pipe, check=True)
            result = subprocess.run(command(arch, fixture), capture_output=True, timeout=10)
            passed = (result.returncode == 141 if default else
                      result.returncode == 42 and result.stdout == b"native-pipe-signal-ok\n")
            records.append({"receiver_arch": arch, "case": "pipe-signal-default" if default else "pipe-signal",
                            "host_sha256": sha(BUILD / arch / "dnf-host.exe"), "fixture_sha256": sha(fixture),
                            "build": build_pipe, "exit_code": result.returncode,
                            "stdout": result.stdout.decode(errors="replace"), "stderr": result.stderr.decode(errors="replace"),
                            "passed": passed})
            print(arch, "pipe SIGPIPE default=" + str(default), "PASS" if passed else "FAIL", flush=True)
        for blocked in (False, True):
            fault = BUILD / "fixtures" / arch / ("signal-fault-blocked" if blocked else "signal-fault")
            build_fault = [str(BIN / "clang.exe"), "--target=" + triple, "-fuse-ld=lld", "-nostdlib", "-static",
                           "-ffreestanding", "-fno-stack-protector", "-fno-pic", "-O2", "-Wall", "-Wextra", "-Werror",
                           "-Wl,--image-base=" + base, "-Wl,-e,_start", str(ROOT / "tests/signal_fault.c"), "-o", str(fault)]
            if blocked:
                build_fault.append("-DNR_BLOCKED")
            subprocess.run(build_fault, check=True)
            result = subprocess.run(command(arch, fault), capture_output=True, timeout=10)
            passed = (result.returncode == 125 and b"unhandled exception" in result.stderr if blocked else
                      result.returncode == 42 and result.stdout == b"native-fault-recovery-ok\n")
            records.append({"receiver_arch": arch, "case": "blocked_fault" if blocked else "fault_recovery",
                            "host_sha256": sha(BUILD / arch / "dnf-host.exe"), "fixture_sha256": sha(fault),
                            "exit_code": result.returncode, "stdout": result.stdout.decode(errors="replace"),
                            "stderr": result.stderr.decode(errors="replace"), "passed": passed})
            print(arch, "fault blocked=" + str(blocked), "PASS" if passed else "FAIL", flush=True)
        fixture = BUILD / "fixtures" / arch / "signal-siginfo"
        fixture.parent.mkdir(parents=True, exist_ok=True)
        build_command = [str(BIN / "clang.exe"), "--target=" + triple, "-fuse-ld=lld", "-nostdlib", "-static",
                         "-ffreestanding", "-fno-stack-protector", "-fno-pic", "-O2", "-Wall", "-Wextra", "-Werror",
                         "-Wl,--image-base=" + base, "-Wl,-e,_start", str(ROOT / "tests/signal_siginfo.c"), "-o", str(fixture)]
        subprocess.run(build_command, check=True)
        result = subprocess.run(command(arch, fixture), capture_output=True, timeout=10)
        stdout, stderr = result.stdout.decode(), result.stderr.decode(errors="replace")
        # One deliberate SA_ONSTACK rejection must remain visible.
        gaps=[line for line in stderr.splitlines() if "syscall gap" in line]
        passed = result.returncode == 42 and stdout == "native-siginfo-ok\n" and len(gaps)==1
        records.append({"receiver_arch": arch, "sender_arch": arch, "case": "sa_siginfo",
                        "host_sha256": sha(BUILD / arch / "dnf-host.exe"),
                        "fixture_sha256": sha(fixture), "exit_code": result.returncode,
                        "stdout": stdout, "stderr": stderr, "passed": passed})
        print(arch, "SA_SIGINFO delivery", "PASS" if passed else "FAIL", flush=True)
        directory=unpack_runtime(BUILD / "fixtures/signal-glibc" / arch,arch)
        lib="lib64" if arch=="x86_64" else "lib"
        program,loader=directory/"signal-glibc",directory/lib/"ld-2.17.so"
        subprocess.run([str(BIN/"clang.exe"),"--target="+triple,"-fuse-ld=lld","-nostdlib","-fno-stack-protector",
                        "-fno-pic","-no-pie","-O2","-Wall","-Wextra","-Werror","-DNR_GLIBC",
                        "-Wl,--image-base="+base,"-Wl,-e,_start","-Wl,--dynamic-linker=/"+lib+"/ld-2.17.so",
                        str(ROOT/"tests/signal_siginfo.c"),"-L"+str(directory/lib),"-l:libpthread.so.0","-l:libc.so.6","-o",str(program)],check=True)
        result=subprocess.run([str(BUILD/arch/"dnf-host.exe"),"--root",str(directory),sha(loader),
                               "--run",str(program),sha(program),"signal-glibc"],capture_output=True,timeout=15)
        stderr=result.stderr.decode(errors="replace")
        passed=result.returncode==42 and result.stdout==b"native-glibc-siginfo-ok\n" and "syscall gap" not in stderr
        records.append({"receiver_arch":arch,"case":"glibc-sigaction-raise","exit_code":result.returncode,
                        "host_sha256":sha(BUILD/arch/"dnf-host.exe"),"program_sha256":sha(program),"loader_sha256":sha(loader),
                        "stderr":stderr,"passed":passed})
        print(arch,"original glibc sigaction/raise/restorer","PASS" if passed else "FAIL",flush=True)
        for blocked in (False, True):
            program = directory / ("thread-signal-blocked" if blocked else "thread-signal")
            build_command = [str(BIN / "clang.exe"), "--target=" + triple, "-fuse-ld=lld", "-nostdlib",
                             "-fno-stack-protector", "-fno-pic", "-no-pie", "-O2", "-Wall", "-Wextra", "-Werror",
                             "-Wl,--image-base=" + base, "-Wl,-e,_start", "-Wl,--dynamic-linker=/" + lib + "/ld-2.17.so",
                             str(ROOT / "tests/process_signal_threaded.c"), "-L" + str(directory / lib),
                             "-l:libpthread.so.0", "-l:libc.so.6", "-o", str(program)]
            if blocked:
                build_command.append("-DNR_BLOCK_LEADER")
            subprocess.run(build_command, check=True)
            host = BUILD / arch / "dnf-host.exe"
            child = subprocess.Popen([str(host), "--root", str(directory), sha(loader), "--run", str(program),
                                      sha(program), "thread-signal"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE)
            forced, error = False, None
            try:
                assert child.stdout.readline() == b"thread-signal-ready\n"
                request_stop(child.pid, 12)
                # The leader stays inside read while its worker makes syscalls.
                # An unmasked leader's signal must remain pending until return.
                time.sleep(.1)
                child.stdin.write(b"u"); child.stdin.flush()
                child.wait(timeout=5)
            except Exception as failure:
                error = repr(failure)
            finally:
                if child.poll() is None:
                    forced = True; child.terminate(); child.wait(timeout=5)
                child.stdin.close()
            stderr = child.stderr.read().decode(errors="replace")
            child.stderr.close(); child.stdout.close()
            passed = child.returncode == 42 and not forced and error is None
            records.append({"receiver_arch": arch, "case": "thread-signal-blocked-leader" if blocked else "thread-signal-leader",
                            "host_sha256": sha(host), "fixture_sha256": sha(program), "exit_code": child.returncode,
                            "stderr": stderr, "forced_cleanup": forced, "error": error, "passed": passed})
            print(arch, "process signal leader blocked=" + str(blocked), "PASS" if passed else "FAIL", flush=True)
    for receive_arch in ARCHES:
        for send_arch in ARCHES:
            receiver = subprocess.Popen(command(receive_arch, receivers[receive_arch]), stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            forced, error, sent = False, None, None
            try:
                sender = build(send_arch, "signal-sender", receiver.pid)
                for round_index in range(16):
                    marker = receiver.stdout.readline()
                    if marker != b"signal-ready\n":
                        raise RuntimeError("Receiver did not reach signal round " + str(round_index + 1))
                    sent = subprocess.run(command(send_arch, sender), capture_output=True, timeout=5)
                    if sent.returncode != 42:
                        raise RuntimeError("Signal sender failed at round " + str(round_index + 1))
                    receiver.stdin.write(b"u")
                    receiver.stdin.flush()
                receiver.wait(timeout=5)
            except (OSError, RuntimeError, subprocess.TimeoutExpired) as failure:
                error = str(failure)
            finally:
                if receiver.poll() is None:
                    forced = True
                    receiver.terminate()
                    receiver.wait(timeout=5)
                receiver.stdin.close()
            stderr = receiver.stderr.read().decode("utf-8", "replace")
            passed = not error and not forced and sent is not None and sent.returncode == 42 and receiver.returncode == 42
            record = {"receiver_arch": receive_arch, "sender_arch": send_arch,
                      "receiver_host_sha256": sha(Path(command(receive_arch, receivers[receive_arch])[0])),
                      "sender_host_sha256": sha(BUILD / send_arch / "dnf-host.exe"),
                      "receiver_fixture_sha256": sha(receivers[receive_arch]),
                      "sender_fixture_sha256": sha(sender) if sent else None,
                      "receiver_exit_code": receiver.returncode, "sender_exit_code": sent.returncode if sent else None,
                      "receiver_stderr": stderr, "sender_stderr": sent.stderr.decode("utf-8", "replace") if sent else None,
                      "rounds": 16, "forced_cleanup": forced, "error": error, "passed": passed}
            records.append(record)
            print(send_arch, "->", receive_arch, "PASS" if passed else "FAIL", flush=True)
    result = {"recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "os": platform.platform(),
              "scope": "Own native ELF processes: broken-pipe write/writev EPIPE and actual SIGPIPE handler, writer thread identity, blocked/coalesced, ignored and default termination; synchronous SIGSEGV for unmapped/read-only/PROT_NONE memory with si_addr, si_code and ucontext recovery; blocked fault diagnostic exit; SA_SIGINFO sender/origin, Linux GPR/sigmask frame read/write and rt_sigreturn; deliberate unsupported-altstack rejection; cross-process kill(0), blocked SIGUSR2 and SIGTERM for 16 rounds per architecture pair. No game shutdown acceptance.",
              "results": records}
    (BUILD / "process-signal-checks.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if not all(row["passed"] for row in records):
        raise SystemExit("Cross-process signal checks failed")


if __name__ == "__main__":
    main()
