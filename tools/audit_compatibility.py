"""Record read-only API candidates in the reference sources; never import code."""

import argparse
import datetime
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
SCOPES = ["remake/source/DNFServer", "remake/source/ChannelOld",
          "remake/secsvr/source", "remake/secsvr/tss_sdk"]
APIS = """fork vfork execve waitpid setsid clone pthread_create pthread_cancel
getrlimit setrlimit epoll_create epoll_create1 epoll_wait epoll_ctl
shm_unlink shmctl shmat shmdt shmget shm_open semget semctl semop
flock fcntl msync mprotect mmap mremap sendmsg recvmsg socketpair
inotify_init inotify_init1 inotify_add_watch inotify_rm_watch
eventfd timer_create timer_settime setitimer sigaltstack sigaction
pthread_kill sched_setaffinity prctl sendfile""".split()


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    reference = args.reference.resolve(strict=True)
    pattern = re.compile(r"\b(" + "|".join(APIS) + r")\s*\(")
    hits, count = {}, 0
    for scope in SCOPES:
        for path in sorted((reference / scope).rglob("*")):
            if path.suffix.lower() not in {".c", ".cpp", ".h", ".inc"} or not path.is_file():
                continue
            count += 1
            digest = None
            for line, text in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                for match in pattern.finditer(text):
                    entry = hits.setdefault(match[1], {"count": 0, "examples": []})
                    entry["count"] += 1
                    if len(entry["examples"]) < 5:
                        digest = digest or sha(path)
                        entry["examples"].append({"path": path.relative_to(reference).as_posix(),
                                                  "line": line, "sha256": digest})
    record = {"recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "reference_root": str(reference), "source_scopes": SCOPES,
              "scanned_source_files": count, "candidate_apis": APIS,
              "scope": "Lexical call candidates, including declarations/comments/duplicates; not original ELF reachability or ABI acceptance. No reference source content copied.",
              "apis": dict(sorted(hits.items())), "reference_material_in_delivery": False,
              "runtime_sources": {path.relative_to(ROOT).as_posix(): sha(path)
                                  for path in sorted((ROOT / "src/runtime").iterdir()) if path.is_file()},
              "auditor_sha256": sha(Path(__file__))}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print("Read-only audit:", count, "source files;", len(hits), "API candidates")


if __name__ == "__main__":
    main()
