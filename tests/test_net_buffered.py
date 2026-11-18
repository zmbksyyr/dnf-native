"""Delayed single/split TCP payload, cached poll/select/epoll and receive-ring wrap."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import socket
import struct
import subprocess
import sys
import threading
import time


def exercise(command, case, port):
    errors = []
    listener = None
    if case != 4:
        listener = socket.socket()
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(('127.0.0.1', port))
        listener.listen(1)
        listener.settimeout(8)
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def peer():
        try:
            if case == 4:
                assert process.stdout.read(1) == b'L'
                stream = socket.create_connection(('127.0.0.1', port), 8)
            else:
                stream = listener.accept()[0]
            with stream:
                stream.settimeout(8)
                assert stream.recv(1) == b'x'
                size = 49152 if case == 1 else 262144 if case == 2 else 131072 if case == 3 else 4096
                data = bytes(i % 251 for i in range(size))
                if case in (1, 3):
                    for offset in range(0, size, 16384):
                        stream.sendall(data[offset:offset+16384])
                        time.sleep(.02)
                    if case == 3:
                        time.sleep(.05)
                else:
                    stream.sendall(data)
                if case != 2:
                    stream.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                                      struct.pack('HH' if os.name == 'nt' else 'ii', 1, 0))
        except Exception as exc:
            errors.append(repr(exc))

    worker = threading.Thread(target=peer)
    worker.start()
    try:
        process.wait(timeout=15)
    except subprocess.TimeoutExpired:
        errors.append('guest timeout')
        process.terminate()
        process.wait(timeout=5)
    worker.join(9)
    if listener:
        listener.close()
    stdout, stderr = process.communicate()
    return dict(case=case, exit=process.returncode, errors=errors,
                stdout_hex=stdout.hex(), stderr=stderr.decode(errors='replace'),
                passed=process.returncode == 42 and stdout == b'buffered-ok\n' and not errors)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--hosts', type=Path)
    parser.add_argument('--linux-distro')
    parser.add_argument('--buffered-reset', action='store_true',
                        help='Expose the known Winsock loss of unread reset payloads; currently fails Windows')
    parser.add_argument('--probe-limits', action='store_true',
                        help='Also expose loss when an unread reset exceeds the owned receive buffer')
    parser.add_argument('--native', type=Path)
    parser.add_argument('--case', type=int)
    parser.add_argument('--port', type=int)
    args = parser.parse_args()
    if args.native:
        print(json.dumps(exercise([str(args.native)], args.case, args.port)))
        return
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
    from bootstrap import TOOLCHAIN, BUILD
    records = []
    for arch, target, base in [('i686', 'i386-linux-gnu', '0x8048000'), ('x86_64', 'x86_64-linux-gnu', '0x400000')]:
        cases = [0, 1, 2, 3, 4] if args.probe_limits else [0, 1, 2, 4] if args.buffered_reset else [2]
        for case in cases:
            with socket.socket() as reserved:
                reserved.bind(('127.0.0.1', 0))
                port = reserved.getsockname()[1]
            fixture = BUILD / 'fixtures' / arch / f'net-buffered-{case}'
            fixture.parent.mkdir(parents=True, exist_ok=True)
            subprocess.run([str(TOOLCHAIN / 'clang.exe'), '--target='+target, '-fuse-ld=lld', '-nostdlib', '-static',
                            '-ffreestanding', '-fno-stack-protector', '-fno-pic', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-Wl,--image-base='+base, '-Wl,-e,_start', f'-DNR_PORT={port}', f'-DNR_CASE={case}',
                            str(Path(__file__).with_name('net_buffered.c')), '-o', str(fixture)], check=True)
            host = (args.hosts or BUILD) / arch / 'dnf-host.exe'
            digest = hashlib.sha256(fixture.read_bytes()).hexdigest()
            record = exercise([str(host), '--run', str(fixture), digest, 'net-buffered'], case, port)
            record.update(arch=arch, fixture_sha256=digest, host_sha256=hashlib.sha256(host.read_bytes()).hexdigest())
            if args.linux_distro:
                def linux_path(path):
                    value = str(path.resolve()).replace('\\', '/')
                    return '/mnt/'+value[0].lower()+value[2:]
                result = subprocess.run(['wsl.exe', '-d', args.linux_distro, '-u', 'root', '--', 'python3', '-B',
                                         linux_path(Path(__file__)), '--native', linux_path(fixture), '--case', str(case),
                                         '--port', str(port)], capture_output=True, text=True, check=True, timeout=30)
                record['linux'] = json.loads(result.stdout)
            records.append(record)
            print(arch, case, 'PASS' if record['passed'] else 'FAIL', 'Linux='+str(record.get('linux', {}).get('passed')), flush=True)
    (BUILD / 'net-buffered-checks.json').write_text(json.dumps(dict(
        recorded_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(), os=platform.platform(),
        linux_distro=args.linux_distro, results=records), indent=2), encoding='utf8')
    if not all(r['passed'] and r.get('linux', {'passed': True})['passed'] for r in records):
        raise SystemExit('Buffered TCP checks failed')


if __name__ == '__main__':
    main()
