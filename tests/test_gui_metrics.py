"""Measure the real GUI, database tree and controlled x86/x64 service workloads."""
import argparse
import configparser
import ctypes as c
from ctypes import wintypes as w
import datetime
import json
import os
from pathlib import Path
import platform
import re
import shutil
import socket
import statistics
import subprocess
import sys
import time
import traceback

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from bootstrap import BUILD, TOOLCHAIN, sha256
from gui_support import user, kernel, windows, wait_for, title, click, ready, select_service, hidden_startup
from runtime_support import unpack_runtime, request_stop, runtime_diagnostics

ROOT = Path(__file__).resolve().parents[1]


class ProcessEntry(c.Structure):
    _fields_ = [("size", w.DWORD), ("usage", w.DWORD), ("pid", w.DWORD),
                ("heap", c.c_size_t), ("module", w.DWORD), ("threads", w.DWORD),
                ("parent", w.DWORD), ("priority", w.LONG), ("flags", w.DWORD), ("exe", w.WCHAR * 260)]


class Memory(c.Structure):
    _fields_ = [("cb", w.DWORD), ("faults", w.DWORD)] + [(name, c.c_size_t) for name in
                ("peak", "working", "peak_paged", "paged", "peak_nonpaged", "nonpaged", "pagefile", "peak_pagefile")]


kernel.CreateToolhelp32Snapshot.argtypes, kernel.CreateToolhelp32Snapshot.restype = [w.DWORD, w.DWORD], w.HANDLE
kernel.Process32FirstW.argtypes = kernel.Process32NextW.argtypes = [w.HANDLE, c.POINTER(ProcessEntry)]
kernel.OpenProcess.argtypes, kernel.OpenProcess.restype = [w.DWORD, w.BOOL, w.DWORD], w.HANDLE
kernel.CloseHandle.argtypes = [w.HANDLE]
kernel.GetProcessTimes.argtypes = [w.HANDLE] + [c.POINTER(w.FILETIME)] * 4
kernel.GetActiveProcessorCount.argtypes, kernel.GetActiveProcessorCount.restype = [w.WORD], w.DWORD
psapi = c.WinDLL("psapi", use_last_error=True)
psapi.GetProcessMemoryInfo.argtypes = [w.HANDLE, c.POINTER(Memory), w.DWORD]


def process_tree(pid):
    snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
    assert snapshot != c.c_void_p(-1).value
    entries, entry = {}, ProcessEntry()
    entry.size = c.sizeof(entry)
    try:
        ok = kernel.Process32FirstW(snapshot, c.byref(entry))
        while ok:
            entries[entry.pid] = (entry.parent, entry.exe)
            ok = kernel.Process32NextW(snapshot, c.byref(entry))
    finally:
        kernel.CloseHandle(snapshot)
    def created(process_id):
        handle = kernel.OpenProcess(0x1000, False, process_id)
        if not handle:
            if c.get_last_error() == 87:
                return None
            raise c.WinError(c.get_last_error())
        try:
            times = [w.FILETIME() for _ in range(4)]
            assert kernel.GetProcessTimes(handle, *[c.byref(value) for value in times])
            return (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime
        finally:
            kernel.CloseHandle(handle)
    found = {pid: created(pid)}
    while True:
        children = {}
        for process_id, (parent, _) in entries.items():
            if parent in found and process_id not in found:
                birth = created(process_id)
                if birth is not None and birth >= found[parent]:
                    children[process_id] = birth
        if not children:
            return {p: entries[p][1] for p in found}
        found.update(children)


def counters(pids):
    cpu, memory = 0, 0
    for pid in pids:
        handle = kernel.OpenProcess(0x410, False, pid)
        assert handle, (pid, c.get_last_error())
        try:
            creation, exit_time, system, user_time = (w.FILETIME() for _ in range(4))
            assert kernel.GetProcessTimes(handle, c.byref(creation), c.byref(exit_time), c.byref(system), c.byref(user_time))
            cpu += sum((v.dwHighDateTime << 32) | v.dwLowDateTime for v in (system, user_time))
            usage = Memory()
            usage.cb = c.sizeof(usage)
            assert psapi.GetProcessMemoryInfo(handle, c.byref(usage), c.sizeof(usage))
            memory += usage.working
        finally:
            kernel.CloseHandle(handle)
    return cpu, memory / (1024 * 1024)


def displayed(window):
    match = re.search(r"CPU ([\d.]+)%.*内存 ([\d.]+) MB", title(window))
    return tuple(map(float, match.groups())) if match else None


def measure(window, pid):
    time.sleep(1.2)  # New processes need two real timer samples for CPU deltas.
    pids = process_tree(pid)
    start, started = counters(pids)[0], time.monotonic()
    observations, references = [], []
    for _ in range(8):
        observations.append(wait_for(lambda: displayed(window), "numeric resource summary", 5))
        references.append(counters(pids)[1])
        time.sleep(0.55)
    end, ended = counters(pids)[0], time.monotonic()
    cpu = (end - start) / 1e7 / (ended - started) * 100 / kernel.GetActiveProcessorCount(0xFFFF)
    shown_cpu = statistics.mean(x[0] for x in observations)
    shown_memory = statistics.mean(x[1] for x in observations)
    memory = statistics.mean(references)
    assert abs(shown_memory - memory) < max(6, memory * 0.03), (shown_memory, memory)
    assert abs(shown_cpu - cpu) < max(0.8, cpu * 0.35), (shown_cpu, cpu)
    return {"processes": pids, "displayed_cpu": shown_cpu, "windows_cpu": cpu,
            "displayed_memory_mb": shown_memory, "windows_memory_mb": memory}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gui", type=Path, required=True)
    parser.add_argument("--instance", type=Path, required=True, help="Stopped private fixture, copied before use")
    args = parser.parse_args()
    gui, source = args.gui.resolve(strict=True), args.instance.resolve(strict=True)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    probe = BUILD / "probes" / stamp / "gui-metrics"
    instance = probe / "instance"
    shutil.copytree(source, instance, ignore=shutil.ignore_patterns(
        ".mysql", "service-logs", "home", "dp2", "lib", "lib64", "usr", "tmp", "dev"))
    unpack_runtime(instance, "i686")
    with socket.socket() as port:
        port.bind(("127.0.0.1", 0))
        number = port.getsockname()[1]
    for name, section in [("server.cnf", "mysqld"), ("client.private.cnf", "client")]:
        config = configparser.ConfigParser(interpolation=None)
        config.read(instance / name, encoding="utf-8")
        config[section]["port"] = str(number)
        with (instance / name).open("w", encoding="utf-8") as stream:
            config.write(stream)
    config = configparser.ConfigParser(interpolation=None)
    config["services"] = {"version": "1", "count": "2", "service0": "probe32", "service1": "probe64"}
    fixtures = []
    for arch, triple, base, name, host, lib in [
        ("i686", "i386-linux-gnu", "0x8048000", "probe32", "x86", "lib"),
        ("x86_64", "x86_64-linux-gnu", "0x400000", "probe64", "x64", "lib64")]:
        program = instance / name
        subprocess.run([str(TOOLCHAIN / "clang.exe"), "--target=" + triple, "-fuse-ld=lld", "-nostdlib", "-static",
                        "-ffreestanding", "-fno-stack-protector", "-fno-pic", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-Wl,--image-base=" + base, "-Wl,-e,_start", str(ROOT / "tests/gui_metrics_guest.c"),
                        "-o", str(program)], check=True)
        config[name] = {"host": "dnf-host-" + host + ".exe", "root": ".", "cwd": "/", "elf": "/" + name,
                        "elf_sha256": sha256(program), "loader_sha256": sha256(instance / lib / "ld-2.17.so"),
                        "automatic": "0", "stop_signal": "15", "arg0": "/" + name}
        fixtures.append(program)
    with (instance / "services.ini").open("w", encoding="utf-16") as stream:
        config.write(stream)
    record = {"os": platform.platform(), "gui_sha256": sha256(gui), "script_sha256": sha256(Path(__file__)),
              "fixture_source_sha256": sha256(ROOT / "tests/gui_metrics_guest.c"),
              "host_sha256": {p.name: sha256(p) for p in (gui.parent / "resources").glob("dnf-host-*.exe")},
              "checks": {}, "measurements": {},
              "scope": "Actual GUI and MySQL; controlled static i686/x64 ELF workloads, not game business acceptance.",
              "forced_cleanup": False}
    process = subprocess.Popen([str(gui), "--instance", str(instance)],
                               env=dict(os.environ, LOCALAPPDATA=str(probe / "profile")), startupinfo=hidden_startup())
    window, unrelated, handles = None, None, []
    try:
        window = wait_for(lambda: next(iter(windows(process.pid, "DNFNativeController")), None), "controller")
        record["measurements"]["gui_only"] = measure(window, process.pid)
        print("GUI-only Windows counters matched", flush=True)
        click(window, 111)
        ready(window, "数据库 SQL 检查通过", 60)
        database = record["measurements"]["database_only"] = measure(window, process.pid)
        assert "mysqld.exe" in database["processes"].values()
        assert "mysql-control.exe" in database["processes"].values()
        assert "conhost.exe" not in database["processes"].values()
        record["checks"]["database_does_not_create_hidden_console_helpers"] = True
        assert database["displayed_memory_mb"] > record["measurements"]["gui_only"]["displayed_memory_mb"] + 20
        record["checks"]["gui_wrapper_and_nested_mysqld_included"] = True
        print("GUI and nested MySQL counters matched", flush=True)
        for name in ("probe32", "probe64"):
            select_service(window, name)
            click(window, 221)
            ready(window, name + "：进程运行中", 30)
            wait_for(lambda: any(b"metrics-ready" in p.read_bytes() for p in (instance / "service-logs").glob(name + "*.stdout.private.log")), name + " memory resident")
        both = record["measurements"]["two_architectures"] = measure(window, process.pid)
        assert both["displayed_memory_mb"] > database["displayed_memory_mb"] + 80
        assert both["windows_cpu"] > database["windows_cpu"] + 1
        assert "conhost.exe" not in both["processes"].values()
        record["checks"]["service_hosts_do_not_create_hidden_console_helpers"] = True
        record["checks"]["both_service_working_sets_and_cpu_included"] = True
        print("x86/x64 service CPU and working sets matched", flush=True)
        out, err = (probe / "unrelated.out").open("wb"), (probe / "unrelated.err").open("wb")
        handles += [out, err]
        unrelated = subprocess.Popen([str(gui.parent / "resources/dnf-host-x64.exe"), "--run", str(fixtures[1]),
                                      sha256(fixtures[1]), "unrelated-metrics-probe"], stdout=out, stderr=err)
        wait_for(lambda: b"metrics-ready" in (probe / "unrelated.out").read_bytes(), "unrelated workload")
        excluded = record["measurements"]["unrelated_running"] = measure(window, process.pid)
        assert unrelated.pid not in excluded["processes"]
        assert abs(excluded["displayed_memory_mb"] - both["displayed_memory_mb"]) < 15
        assert counters([unrelated.pid])[1] > 45
        record["checks"]["unrelated_process_excluded"] = True
        request_stop(unrelated.pid)
        assert unrelated.wait(timeout=15) == 0
        select_service(window, "probe32")
        click(window, 222)
        ready(window, "probe32：已停止", 30)
        one = record["measurements"]["one_service"] = measure(window, process.pid)
        assert both["displayed_memory_mb"] - one["displayed_memory_mb"] > 40
        click(window, 221)
        ready(window, "probe32：进程运行中", 30)
        record["measurements"]["restarted"] = measure(window, process.pid)
        record["checks"]["exit_removed_and_restart_resampled_without_cpu_spike"] = True
        click(window, 224)
        ready(window, "服务进程已退出，数据库已停止", 60)
        stopped = record["measurements"]["all_stopped"] = measure(window, process.pid)
        assert "mysqld.exe" not in stopped["processes"].values()
        assert not any("dnf-host" in name for name in stopped["processes"].values())
        assert stopped["displayed_memory_mb"] < database["displayed_memory_mb"]
        record["checks"]["returns_to_gui_only_after_shutdown"] = True
    except Exception as error:
        record["error"] = type(error).__name__ + ": " + str(error)
        record["traceback"] = traceback.format_exc()
    finally:
        if unrelated and unrelated.poll() is None:
            request_stop(unrelated.pid)
            unrelated.wait(timeout=15)
        for handle in handles:
            handle.close()
        if window and process.poll() is None:
            user.PostMessageW(window, 0x10, 0, 0)
        try:
            process.wait(timeout=60)
        except subprocess.TimeoutExpired:
            record["forced_cleanup"] = True
            subprocess.run(["taskkill.exe", "/PID", str(process.pid), "/T", "/F"], capture_output=True, timeout=15)
            process.wait(timeout=10)
        record["runtime"] = runtime_diagnostics(instance)
        record["passed"] = "error" not in record and not record["forced_cleanup"] and not record["runtime"]["faults"]
        (probe / "receipt.json").write_text(json.dumps(record, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print("GUI process-tree metrics:", record["passed"], probe, flush=True)
    if not record["passed"]:
        raise SystemExit(record.get("error", "Metrics regression failed; inspect receipt"))


if __name__ == "__main__":
    main()
