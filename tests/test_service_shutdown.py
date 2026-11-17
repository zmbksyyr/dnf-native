"""Exercise original manager/DBMW shutdown during worker startup and after it."""

import argparse
import datetime
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import time
import traceback

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from bootstrap import BUILD, sha256
from gui_support import (user, windows, wait_for, click, ready, ready_stopped, rows, service_row,
                         select_service, hidden_startup)
from runtime_support import runtime_diagnostics


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--instance", type=Path, required=True)
    parser.add_argument("--gui", type=Path, required=True)
    args = parser.parse_args()
    source, gui = args.instance.resolve(strict=True), args.gui.resolve(strict=True)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    probe = BUILD / "probes" / stamp / "service-shutdown"
    instance = probe / "instance"
    shutil.copytree(source, instance, ignore=shutil.ignore_patterns(".mysql", "service-logs"))
    targets = ["manager", "dbmw_guild", "dbmw_mnt", "dbmw_stat"]
    originals = {str(p.relative_to(instance)): sha256(p) for name in targets
                 for p in (instance / "home/neople" / name).glob("df_*_r")}
    record = {"recorded_utc": stamp, "os": platform.platform(), "gui_sha256": sha256(gui),
              "script_sha256": sha256(Path(__file__)), "elf_sha256": originals,
              "scope": "Original network worker shutdown only; no business acceptance.",
              "forced_cleanup": False, "cycles": []}
    process = subprocess.Popen([str(gui), "--instance", str(instance)], startupinfo=hidden_startup())
    window = None
    try:
        window = wait_for(lambda: next(iter(windows(process.pid, "DNFNativeController")), None), "controller")
        assert len(rows(window)) == 19
        click(window, 111); ready(window, "数据库 SQL 检查通过")
        for name, delay in [(name, 0) for name in targets] + [("manager", 8), ("dbmw_guild", 8)]:
            previous = set((instance / "service-logs").glob(name + "-*.stderr.private.log"))
            select_service(window, name)
            click(window, 221); ready(window, name + "：进程运行中", 30)
            log = wait_for(lambda: next(iter(set((instance / "service-logs").glob(name + "-*.stderr.private.log")) - previous), None), "new stderr")
            wait_for(lambda: log.read_text(encoding="utf-8", errors="replace").count("guest thread enter") >= 3,
                     "three original network workers created", 30)
            if delay:
                time.sleep(delay)
            start = time.monotonic()
            click(window, 222); ready_stopped(window, name, 45)
            text = log.read_text(encoding="utf-8", errors="replace")
            cycle = {"service": name, "settle_seconds": delay,
                     "stop_seconds": round(time.monotonic() - start, 3),
                     "joined_workers": sorted(map(int, re.findall(r"service shutdown joined worker=(\d+).* result=0", text))),
                     "waited_for_initialization": "service shutdown await initialization" in text}
            record["cycles"].append(cycle)
            assert int(re.search(r"代码 (\d+)$", service_row(window, name))[1]) == 1
            assert cycle["joined_workers"] == [0, 1, 2]
            assert "service shutdown workers quiesced; resuming original Free" in text
            assert b"Application Stop!" in log.with_name(log.name.replace(".stderr.", ".stdout.")).read_bytes()
            print("Shutdown:", name, "settle", delay, "joined", cycle["joined_workers"], flush=True)
        assert any(c["waited_for_initialization"] for c in record["cycles"] if not c["settle_seconds"])
        click(window, 112); ready(window, "数据库已正常停止")
    except Exception as error:
        record["error"] = type(error).__name__ + ": " + str(error)
        record["traceback"] = traceback.format_exc()
    finally:
        if process.poll() is None:
            if window: user.PostMessageW(window, 0x10, 0, 0)
            try:
                process.wait(timeout=120)
            except subprocess.TimeoutExpired:
                record["forced_cleanup"] = True
                subprocess.run(["taskkill.exe", "/PID", str(process.pid), "/T", "/F"], capture_output=True, timeout=15)
                process.wait(timeout=10)
        diagnostics = runtime_diagnostics(instance)
        record["runtime_faults"], record["runtime_gaps"] = diagnostics["faults"], diagnostics["gaps"]
        record["elf_unchanged"] = all(sha256(instance / path) == digest for path, digest in originals.items())
        record["exit_code"] = process.returncode
        record["service_logs"] = {p.name: sha256(p) for p in sorted((instance / "service-logs").glob("*.log"))}
        (probe / "receipt.json").write_text(json.dumps(record, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")
        print("Shutdown receipt:", probe / "receipt.json", flush=True)
    if record.get("error") or record["forced_cleanup"] or record["runtime_gaps"] or not record["elf_unchanged"] or process.returncode:
        raise SystemExit(record.get("error", "shutdown validation failed"))


if __name__ == "__main__":
    main()
