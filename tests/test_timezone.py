"""Verify packaged glibc local time against Windows, including DST transitions."""
import argparse
import datetime as dt
import json
from pathlib import Path
import platform
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from bootstrap import BUILD, TOOLCHAIN, external_output, sha256
from runtime_support import unpack_runtime

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hosts", type=Path, default=BUILD)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = external_output(args.output)
    output.mkdir(parents=True, exist_ok=False)
    oracle = output / "windows-time.exe"
    subprocess.run([str(TOOLCHAIN / "x86_64-w64-mingw32-clang.exe"), "-O2", "-Wall", "-Wextra", "-Werror",
                    "-D_WIN32_WINNT=0x0602", str(ROOT / "tests/timezone_windows.c"), "-o", str(oracle)], check=True)
    year = dt.datetime.now().year
    dates = [f"{year}-01-15T12:00:00", f"{year}-07-15T12:00:00"]
    # Current Windows rules also define first/last-week transitions; exercise
    # UTC seconds directly around northern and southern 2026 DST boundaries.
    if year == 2026:
        dates += ["2026-03-08T09:59:59", "2026-03-08T10:00:00", "2026-11-01T08:59:59", "2026-11-01T09:00:00",
                  "2026-04-04T14:59:59", "2026-04-04T15:00:00", "2026-10-03T15:29:59", "2026-10-03T15:30:00"]
    samples = [str(int(dt.datetime.fromisoformat(d).replace(tzinfo=dt.timezone.utc).timestamp())) for d in dates]
    records = []
    zones = ["system", "Pacific Standard Time", "Nepal Standard Time", "Lord Howe Standard Time", "disabled-Pacific"]
    for arch, triple, base, lib in [("i686", "i386-linux-gnu", "0x8048000", "lib"),
                                    ("x86_64", "x86_64-linux-gnu", "0x400000", "lib64")]:
        directory = unpack_runtime(output / arch, arch)
        fixture, loader = directory / "timezone-probe", directory / lib / "ld-2.17.so"
        subprocess.run([str(TOOLCHAIN / "clang.exe"), "--target=" + triple, "-fuse-ld=lld", "-nostdlib",
                        "-fno-stack-protector", "-fno-pic", "-no-pie", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-Wl,--image-base=" + base, "-Wl,-e,_start", "-Wl,--dynamic-linker=/" + lib + "/ld-2.17.so",
                        str(ROOT / "tests/timezone_guest.c"), "-L" + str(directory / lib), "-l:libc.so.6",
                        "-o", str(fixture)], check=True)
        host = args.hosts.resolve() / arch / "dnf-host.exe"
        for zone in zones:
            lines = subprocess.check_output([str(oracle), zone, *samples], text=True).splitlines()
            timezone, expected = lines[0], lines[1:]
            command = [str(host), "--root", str(directory), sha256(loader),
                       "--env", "DNF_TIME_SAMPLES=" + ",".join(samples)]
            if zone != "system":
                command += ["--env", timezone]
            # Retain the full 32 explicitly supplied environment entries while
            # adding the automatic timezone; default injection uses no slot.
            if zone == "system":
                for index in range(31):
                    command += ["--env", f"FIXTURE_{index}=value"]
            command += ["--run", str(fixture), sha256(fixture), "timezone-probe"]
            before = time.time()
            result = subprocess.run(command, capture_output=True, timeout=20)
            after = time.time()
            text = result.stdout.decode("utf-8").splitlines()
            assert result.returncode == 42, (arch, zone, result.returncode, result.stderr.decode(errors="replace"))
            assert text[1:] == expected, (arch, zone, text, expected)
            _, epoch, calendar = text[0].split()
            assert int(before) <= int(epoch) <= int(after)
            assert int(dt.datetime.strptime(calendar, "%Y-%m-%dT%H:%M:%S%z").timestamp()) == int(epoch)
            if zone == "system":
                assert calendar == dt.datetime.fromtimestamp(int(epoch)).astimezone().strftime("%Y-%m-%dT%H:%M:%S%z")
                assert "guest timezone source=Windows " + timezone in result.stderr.decode()
            records.append({"arch": arch, "zone": zone, "tz": timezone, "now": text[0], "samples": expected,
                            "host_sha256": sha256(host), "fixture_sha256": sha256(fixture), "passed": True})
            print(arch, zone, "PASS", text[0], flush=True)
    (output / "receipt.json").write_text(json.dumps({"os": platform.platform(), "results": records,
        "scope": "Actual packaged glibc localtime/mktime, current Linux realtime and monotonic clocks; Windows API calendar comparison without altering Windows settings.",
        "passed": True}, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
