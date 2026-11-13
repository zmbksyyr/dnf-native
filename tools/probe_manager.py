"""Start original manager with its existing nofork mode and record actual gaps."""

import datetime
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    instance = ROOT / "build/probes" / stamp / "manager"
    instance.mkdir(parents=True)
    source = ROOT / ".local/sysroot/i686"
    for name in ["lib", "usr"]:
        shutil.copytree(source / name, instance / name)
    original = ROOT / "originals/dp123/home/neople/manager"
    guest_dir = instance / "home/neople/manager"
    for name in ["cfg", "table"]:
        shutil.copytree(original / name, guest_dir / name)
    for name in ["pid", "log"]:
        (guest_dir / name).mkdir()
    ld, elf = instance / "lib/ld-2.17.so", original / "df_manager_r"
    command = [str(ROOT / "build/i686/dnf-host.exe"), "--root", str(instance), hashlib.sha256(ld.read_bytes()).hexdigest(),
               "--cwd", "/home/neople/manager", "--run", str(elf), hashlib.sha256(elf.read_bytes()).hexdigest(),
               "/home/neople/manager/df_manager_r", "manager", "nofork"]
    stdout_path, stderr_path = instance / "stdout.log", instance / "stderr.log"
    forced = False
    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        process = subprocess.Popen(command, stdout=stdout, stderr=stderr)
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            forced = True
            process.terminate()
            process.wait(timeout=5)
    stdout = stdout_path.read_text(encoding="utf-8", errors="replace")
    stderr = stderr_path.read_text(encoding="utf-8", errors="replace")
    markers = ["Application Init() Success!", "Application App Config Load_Table() Success!",
               "Application Server Config Load_Table() Success!"]
    reached = {marker: marker in stdout for marker in markers}
    gaps = sorted({line.split(" pc=", 1)[0] for line in stderr.splitlines() if "syscall gap" in line})
    result = {"recorded_utc": stamp, "os": platform.platform(), "command": command,
              "host_sha256": hashlib.sha256(Path(command[0]).read_bytes()).hexdigest(),
              "elf_sha256": hashlib.sha256(elf.read_bytes()).hexdigest(),
              "configuration_sha256": {str(p.relative_to(guest_dir)): hashlib.sha256(p.read_bytes()).hexdigest()
                                       for name in ["cfg", "table"] for p in (guest_dir / name).rglob("*") if p.is_file()},
              "scope": "Original manager existing nofork path and configuration initialization only; no service readiness or protocol acceptance.",
              "forced_cleanup": forced, "exit_code": process.returncode, "initialization_markers": reached,
              "syscall_gaps": gaps, "stdout_sha256": hashlib.sha256(stdout_path.read_bytes()).hexdigest(),
              "stderr_sha256": hashlib.sha256(stderr_path.read_bytes()).hexdigest()}
    (ROOT / "build/manager-probe.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print("Manager initialization:", reached, "gaps:", gaps)
    if not all(reached.values()):
        raise SystemExit("Manager did not reach the recorded initialization path")


if __name__ == "__main__":
    main()
