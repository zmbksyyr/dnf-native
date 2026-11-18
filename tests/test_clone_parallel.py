"""Race concurrent raw clone calls and check child TLS, IDs and clear-tid joins."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from bootstrap import BUILD, TOOLCHAIN


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--hosts', type=Path, default=BUILD)
    parser.add_argument('--linux-distro')
    parser.add_argument('--label', default='clone-parallel')
    args = parser.parse_args()
    records = []
    for arch, target, base in [('i686', 'i386-linux-gnu', '0x8048000'), ('x86_64', 'x86_64-linux-gnu', '0x400000')]:
        fixture = BUILD / 'fixtures' / arch / 'clone-parallel'
        fixture.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(TOOLCHAIN / 'clang.exe'), '--target='+target, '-fuse-ld=lld', '-nostdlib', '-static',
                        '-ffreestanding', '-fno-stack-protector', '-fno-pic', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-Wl,--image-base='+base, '-Wl,-e,_start', str(Path(__file__).with_name('clone_parallel.c')),
                        str(Path(__file__).with_name('futex_clone.S')), '-o', str(fixture)], check=True)
        digest = hashlib.sha256(fixture.read_bytes()).hexdigest()
        host = args.hosts / arch / 'dnf-host.exe'
        commands = [('windows', [str(host), '--run', str(fixture), digest, 'clone-parallel'])]
        if args.linux_distro:
            path = str(fixture.resolve()).replace('\\', '/')
            commands.append(('linux', ['wsl.exe', '-d', args.linux_distro, '-u', 'root', '--', '/mnt/'+path[0].lower()+path[2:]]))
        for system, command in commands:
            for round_no in range(10):
                try:
                    p = subprocess.run(command, capture_output=True, timeout=12)
                    record = dict(arch=arch, system=system, round=round_no, exit=p.returncode,
                                  stderr=p.stderr.decode(errors='replace'), passed=p.returncode==42)
                except subprocess.TimeoutExpired as exc:
                    record = dict(arch=arch, system=system, round=round_no, passed=False,
                                  stderr=(exc.stderr or b'').decode(errors='replace'), timeout=True)
                record.update(fixture_sha256=digest, host_sha256=hashlib.sha256(host.read_bytes()).hexdigest())
                records.append(record)
                print(arch, system, round_no, record.get('exit', 'timeout'), flush=True)
                if not record['passed']:
                    break
    (BUILD / (args.label+'.json')).write_text(json.dumps(dict(os=platform.platform(), results=records), indent=2), encoding='utf8')
    if not all(r['passed'] for r in records):
        raise SystemExit('Concurrent clone checks failed')


if __name__ == '__main__':
    main()
