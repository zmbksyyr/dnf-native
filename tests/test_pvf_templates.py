"""Exercise name-independent layout semantics in original ELF functions, not a full startup."""
import datetime
import json
from pathlib import Path
import platform
import subprocess
import sys
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from bootstrap import BUILD, ROOT, sha256
from resource_baseline import RESOURCES, verify_baseline


def main():
    verify_baseline()
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    work = BUILD / "probes" / stamp / "pvf-templates"
    work.mkdir(parents=True)
    root = work / "root"
    with zipfile.ZipFile(RESOURCES / "native.zip") as archive:
        for name in archive.namelist():
            if not name.startswith(("lib/", "usr/")) and name not in (
                "home/neople/game/df_game_r", "home/neople/game/Script.pvf", "home/neople/game/libnxencryption.so"):
                continue
            target = root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(archive.read(name))
    (root / "tmp").mkdir()
    # Private host only: enter a fixture at original main, after real ctors.
    text = (ROOT / "src/runtime/run.c").read_text(encoding="utf-8")
    changes = {
        "static LONG CALLBACK trap_handler(EXCEPTION_POINTERS *exception) {":
            "void nr_probe_install(NrRun *run);\nint nr_probe_trap(EXCEPTION_POINTERS *exception);\n"
            "static LONG CALLBACK trap_handler(EXCEPTION_POINTERS *exception) {",
        "    if (!nr_current()) return EXCEPTION_CONTINUE_SEARCH;":
            "    if (!nr_current()) return EXCEPTION_CONTINUE_SEARCH;\n"
            "    if (nr_probe_trap(exception)) return EXCEPTION_CONTINUE_EXECUTION;",
        "    InitializeCriticalSection(&run->memory_lock);":
            "    InitializeCriticalSection(&run->memory_lock);\n    nr_probe_install(run);",
    }
    for old, new in changes.items():
        assert text.count(old) == 1
        text = text.replace(old, new)
    private_run = work / "fixture_run.c"
    private_run.write_text(text, encoding="utf-8")
    build = next(x for x in json.loads((BUILD / "build-receipt.json").read_text()) if x["arch"] == "i686")
    for name, expected in build["sources"].items():
        assert sha256(ROOT / name) == expected, "Build current hosts before testing: " + name
    command = [str(private_run) if Path(arg) == ROOT / "src/runtime/run.c" else arg for arg in build["command"]]
    host = work / "pvf-templates.exe"
    command[command.index("-o") + 1] = str(host)
    command += ["-I", str(ROOT / "src/runtime"), str(ROOT / "tests/pvf_templates.c")]
    compiled = subprocess.run(command, cwd=ROOT, capture_output=True)
    (work / "compile.log").write_bytes(compiled.stdout + compiled.stderr)
    compiled.check_returncode()
    game = root / "home/neople/game/df_game_r"
    loader = root / "lib/ld-2.17.so"
    cases = [("mixed", 42), ("present-default", 42), ("missing-layer", 126),
             ("negative-layer", 126), ("invalid-job", 126), ("invalid-type", 126)]
    record = {"os": platform.platform(), "scope": __doc__, "build_command": command,
              "host_sha256": sha256(host), "elf_sha256": sha256(game), "pvf_sha256": sha256(game.parent / "Script.pvf"),
              "fixture_sha256": sha256(ROOT / "tests/pvf_templates.c"), "cases": []}
    for case, expected in cases:
        args = [str(host), "--root", str(root), sha256(loader), "--env", "LD_LIBRARY_PATH=/home/neople/game",
                "--cwd", "/home/neople/game", "--run", str(game), sha256(game), "/home/neople/game/df_game_r", "Script.pvf", case]
        result = subprocess.run(args, capture_output=True, timeout=45)
        (work / (case + ".stdout.log")).write_bytes(result.stdout)
        (work / (case + ".stderr.log")).write_bytes(result.stderr)
        row = {"case": case, "command": args, "expected_exit": expected, "exit_code": result.returncode,
               "stdout_sha256": sha256(work / (case + ".stdout.log")),
               "stderr_sha256": sha256(work / (case + ".stderr.log"))}
        row["passed"] = result.returncode == expected and b"unhandled exception" not in result.stderr
        if case == "mixed":
            row["passed"] &= result.stdout.count(b"metadata=preserved") == 36
        if expected == 126:
            row["passed"] &= b"missing layout required to supply layer or unsupported animation mode" in result.stderr
        record["cases"].append(row)
        (work / "receipt.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
        print("PVF templates", case, "PASS" if row["passed"] else "FAIL", flush=True)
        assert row["passed"], "See fixture logs: " + str(work)
    print("PVF template semantics receipt:", work / "receipt.json", flush=True)


if __name__ == "__main__":
    main()
