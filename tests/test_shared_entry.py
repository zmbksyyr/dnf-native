"""Check zero-entry DSO loading and keep real process/interpreter entry validation."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import platform
import struct
import subprocess

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from bootstrap import TOOLCHAIN, BUILD
from runtime_support import unpack_runtime

ROOT = Path(__file__).resolve().parents[1]
BIN = TOOLCHAIN


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hosts", type=Path, default=BUILD)
    args = parser.parse_args()
    records = []
    for arch, triple, lib, base in [("i686", "i386-linux-gnu", "lib", "0x8048000"),
                                   ("x86_64", "x86_64-linux-gnu", "lib64", "0x400000")]:
        directory = BUILD / "fixtures/shared-entry" / arch
        unpack_runtime(directory, arch)
        ld = directory / lib / "ld-2.17.so"
        so = directory / "entry-zero.so"
        common = [str(BIN / "clang.exe"), "--target=" + triple, "-fuse-ld=lld", "-nostdlib",
                  "-fno-stack-protector", "-O2", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests/shared_entry.c")]
        subprocess.run([*common, "-shared", "-fPIC", "-DNR_SHARED", "-o", str(so)], check=True)
        raw = bytearray(so.read_bytes())
        fmt = "<I" if arch == "i686" else "<Q"
        assert struct.unpack_from(fmt, raw, 24)[0] == 0
        struct.pack_into(fmt, raw, 24, 1)  # ELF header bytes are outside executable PT_LOAD.
        invalid = directory / "entry-invalid.so"
        invalid.write_bytes(raw)
        host = args.hosts.resolve() / arch / "dnf-host.exe"
        for name, defines in [("dlopen-zero", []), ("dlopen-invalid", ["-DNR_INVALID_DSO"])]:
            program = directory / name
            command = [*common, "-fno-pic", "-no-pie", "-Wl,-e,_start", "-Wl,-E",
                       "-Wl,--image-base=" + base, "-Wl,--dynamic-linker=/" + lib + "/ld-2.17.so",
                       "-L" + str(directory / lib), "-l:libdl.so.2", "-l:libc.so.6", *defines, "-o", str(program)]
            subprocess.run(command, check=True)
            command = [str(host), "--root", str(directory), sha(ld), "--run", str(program), sha(program), name]
            result = subprocess.run(command, capture_output=True, timeout=15)
            stderr = result.stderr.decode("utf-8", "replace")
            passed = result.returncode == 42 and "unhandled exception" not in stderr
            records.append({"arch": arch, "case": name, "host_sha256": sha(host),
                            "program_sha256": sha(program), "dso_sha256": sha(so if not defines else invalid),
                            "exit_code": result.returncode, "stderr": stderr, "passed": passed})
            print(arch, name, "PASS" if passed else "FAIL", "exit", result.returncode, flush=True)
        for name, interpreter in [("zero-process-entry", False), ("zero-interpreter-entry", True)]:
            program = directory / "dlopen-zero" if interpreter else so
            if interpreter:
                # This private interpreter copy is no longer used after this case.
                raw = bytearray(ld.read_bytes())
                struct.pack_into(fmt, raw, 24, 0)
                ld.write_bytes(raw)
            command = [str(host), "--root", str(directory), sha(ld), "--run", str(program), sha(program), name]
            result = subprocess.run(command, capture_output=True, timeout=15)
            stderr = result.stderr.decode("utf-8", "replace")
            passed = result.returncode == 1 and "no bounded executable entry" in stderr
            records.append({"arch": arch, "case": name, "host_sha256": sha(host),
                            "exit_code": result.returncode, "stderr": stderr, "passed": passed})
            print(arch, name, "PASS" if passed else "FAIL", "exit", result.returncode, flush=True)
    result = {"recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "os": platform.platform(),
              "scope": "Self-built DSO through upstream glibc: constructor/export/destructor, bad DSO entry and required executable entries. No game acceptance.",
              "results": records}
    (BUILD / "shared-entry-checks.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if not all(row["passed"] for row in records):
        raise SystemExit("See ../x/dnf-native-output/shared-entry-checks.json")


if __name__ == "__main__":
    main()
