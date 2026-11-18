"""Real shared-socket reads, full send buffer and UDP MSG_DONTWAIT checks."""
import argparse
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


def exercise(command, case, port):
    errors, observed = [], {}
    with socket.socket(type=socket.SOCK_DGRAM if case == 2 else socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if case != 3:
            listener.bind(('127.0.0.1', port))
        listener.settimeout(8)
        if case not in (2, 3):
            listener.listen(1)
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

        def peer():
            try:
                if case == 3:
                    marker = process.stdout.read(1)
                    assert marker == b'L', ('listen marker', marker)
                    with socket.create_connection(('127.0.0.1', port), 8) as stream:
                        stream.settimeout(8)
                        data = stream.recv(1)
                        assert data == b'x', ('server greeting', data)
                        stream.sendall(b'y')
                        data = stream.recv(1)
                        assert data == b'', ('server close', data)
                    return
                if case == 2:
                    data, address = listener.recvfrom(8)
                    assert data == b'u', data
                    listener.sendto(b'v', address)
                    return
                with listener.accept()[0] as stream:
                    stream.settimeout(8)
                    if case == 0:
                        assert stream.recv(1) == b'R'
                        stream.sendall(b'y')
                        assert stream.recv(1) == b''
                    else:
                        report = process.stdout.read(8)
                        assert len(report) == 8, report
                        expected, = struct.unpack('<Q', report)
                        data = bytearray()
                        stream.settimeout(.2)
                        while True:
                            try:
                                part = stream.recv(65536)
                            except socket.timeout:
                                break
                            assert part
                            data.extend(part)
                        stream.sendall(b'z')
                        stream.settimeout(8)
                        while part := stream.recv(65536):
                            data.extend(part)
                        observed.update(reported=expected, received=len(data))
                        assert len(data) == expected and data == b'x' * expected, observed
            except Exception as exc:
                errors.append(repr(exc))

        thread = threading.Thread(target=peer)
        thread.start()
        try:
            process.wait(timeout=12)
        except subprocess.TimeoutExpired:
            errors.append('guest timeout')
            process.terminate()
            process.wait(timeout=5)
        thread.join(9)
        stdout, stderr = process.communicate()
    return dict(case=case, exit=process.returncode, observed=observed, errors=errors,
                stdout=stdout.hex(), stderr=stderr.decode(errors='replace'),
                passed=process.returncode == 42 and not errors and not stdout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--hosts', type=Path)
    parser.add_argument('--linux-distro')
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
        for case in range(4):
            # Do not reuse a port from a preceding Linux listener: WSL's
            # localhost forwarding can outlive the Linux process briefly.
            with socket.socket() as reserved:
                reserved.bind(('127.0.0.1', 0))
                port = reserved.getsockname()[1]
            fixture = BUILD / 'fixtures' / arch / f'net-nowait-{case}'
            fixture.parent.mkdir(parents=True, exist_ok=True)
            subprocess.run([str(TOOLCHAIN / 'clang.exe'), '--target=' + target, '-fuse-ld=lld', '-nostdlib', '-static',
                            '-ffreestanding', '-fno-stack-protector', '-fno-pic', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-Wl,--image-base=' + base, '-Wl,-e,_start', f'-DNR_PORT={port}', f'-DNR_CASE={case}',
                            str(Path(__file__).with_name('net_nowait.c')),
                            str(Path(__file__).with_name('futex_clone.S')), '-o', str(fixture)], check=True)
            host = (args.hosts or BUILD) / arch / 'dnf-host.exe'
            digest = hashlib.sha256(fixture.read_bytes()).hexdigest()
            record = exercise([str(host), '--run', str(fixture), digest, 'net-nowait'], case, port)
            record['port'] = port
            record.update(arch=arch, fixture_sha256=digest, host_sha256=hashlib.sha256(host.read_bytes()).hexdigest())
            if args.linux_distro:
                def linux_path(path):
                    text = str(path.resolve()).replace('\\', '/')
                    return '/mnt/' + text[0].lower() + text[2:]
                result = subprocess.run(['wsl.exe', '-d', args.linux_distro, '-u', 'root', '--', 'python3', '-B',
                                         linux_path(Path(__file__)), '--native', linux_path(fixture), '--case', str(case),
                                         '--port', str(port)],
                                        check=True, capture_output=True, text=True, timeout=30)
                record['linux'] = json.loads(result.stdout)
            records.append(record)
            print(arch, case, 'PASS' if record['passed'] else 'FAIL',
                  'Linux=' + str(record.get('linux', {}).get('passed')), flush=True)
    (BUILD / 'net-nowait-checks.json').write_text(json.dumps({'os': platform.platform(),
        'linux_distro': args.linux_distro, 'results': records}, indent=2), encoding='utf8')
    if not all(r['passed'] and r.get('linux', {'passed': True})['passed'] for r in records):
        raise SystemExit('Per-call nonblocking socket checks failed')


if __name__ == '__main__':
    main()
