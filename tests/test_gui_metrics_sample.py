"""Run the production sampler with real and stale-parent snapshot ancestry."""
import argparse
import datetime
import json
import os
from pathlib import Path
import platform
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from bootstrap import BUILD, TOOLCHAIN, sha256

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gui-build-dir", type=Path, required=True)
    parser.add_argument("--pid", type=int, default=os.getpid(), help="Read-only target; defaults to this test's own process tree")
    args = parser.parse_args()
    gui = args.gui_build_dir.resolve(strict=True)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    probe = BUILD / "probes" / stamp / "gui-metrics-sample"
    probe.mkdir(parents=True)
    exe = probe / "metrics-probe.exe"
    command = [str(TOOLCHAIN / "x86_64-w64-mingw32-clang++.exe"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
               "-O2", "-static", "-municode", "-DUNICODE", "-D_UNICODE", "-D_WIN32_WINNT=0x0601",
               str(ROOT / "tests/gui_metrics_probe.cpp"),
               *[str(gui / (name + ".o")) for name in ("miniz", "miniz_tinfl", "miniz_zip")],
               "-DMINIZ_NO_STDIO", "-DMINIZ_NO_TIME", "-DMINIZ_NO_ZLIB_APIS", "-DMINIZ_NO_DEFLATE_APIS",
               "-luser32", "-lgdi32", "-lshell32", "-lole32", "-ladvapi32", "-lws2_32", "-lbcrypt", "-luxtheme",
               "-lcomctl32", "-lpsapi", "-o", str(exe)]
    subprocess.run(command, check=True)
    record = {"os": platform.platform(), "target_pid": args.pid, "command": command,
              "source_sha256": sha256(ROOT / "src/controller/controller_gui.cpp"),
              "probe_source_sha256": sha256(ROOT / "tests/gui_metrics_probe.cpp"), "cases": [],
              "scope": "Production sampler reads real Windows creation times and counters. Synthetic stale ancestry exists only in the test's snapshot view; no target process is changed."}
    for stale in (False, True):
        result = subprocess.run([str(exe), str(args.pid), str(int(stale))], capture_output=True, timeout=15)
        output = result.stdout.decode("utf-8")
        record["cases"].append({"stale_parent_injected": stale, "exit_code": result.returncode, "output": output})
        print("Sampler stale-parent=" + str(stale), result.returncode, output.strip(), flush=True)
    record["passed"] = all(case["exit_code"] == 0 for case in record["cases"])
    (probe / "receipt.json").write_text(json.dumps(record, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(probe / "receipt.json", flush=True)
    if not record["passed"]:
        raise SystemExit("Process ancestry sampling failed")


if __name__ == "__main__":
    main()
