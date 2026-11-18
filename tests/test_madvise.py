"""Build raw Linux DONTNEED semantic fixtures for x86 and x64 hosts."""
import datetime
import json
from pathlib import Path
import platform
import subprocess
import sys

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from bootstrap import BUILD, TOOLCHAIN, sha256
from runtime_support import unpack_runtime

ROOT=Path(__file__).resolve().parents[1]

def main():
    records=[]
    for arch,triple,base in [('i686','i386-linux-gnu','0x8048000'),('x86_64','x86_64-linux-gnu','0x400000')]:
        for concurrent in (False,True):
            fixture=BUILD/'fixtures'/arch/('madvise-concurrent' if concurrent else 'madvise')
            fixture.parent.mkdir(parents=True,exist_ok=True)
            build=[str(TOOLCHAIN/'clang.exe'),'--target='+triple,'-fuse-ld=lld','-nostdlib','-static',
                '-ffreestanding','-fno-stack-protector','-fno-pic','-fomit-frame-pointer','-O2','-Wall','-Wextra','-Werror',
                '-Wl,--image-base='+base,'-Wl,-e,_start',str(ROOT/'tests/madvise.c'),'-o',str(fixture)]
            if concurrent:build.extend(['-DNR_CONCURRENT',str(ROOT/'tests/futex_clone.S')])
            subprocess.run(build,check=True)
            host=BUILD/arch/'dnf-host.exe'
            p=subprocess.run([str(host),'--run',str(fixture),sha256(fixture),'madvise'],capture_output=True,timeout=20)
            expected=b'madvise-read-ok\n' if concurrent else b'madvise-zero-ok\n'
            record={'arch':arch,'concurrent_reader':concurrent,'fixture_sha256':sha256(fixture),'host_sha256':sha256(host),'exit':p.returncode,
                    'stdout':p.stdout.decode(errors='replace'),'stderr':p.stderr.decode(errors='replace'),
                    'passed':p.returncode==42 and p.stdout==expected}
            records.append(record)
            print(arch,'DONTNEED concurrent='+str(concurrent),record['passed'],'exit',p.returncode,flush=True)
        directory=unpack_runtime(BUILD/'fixtures/madvise-pthread'/arch,arch)
        lib='lib64' if arch=='x86_64' else 'lib'
        program=directory/'madvise-pthread'
        loader=directory/lib/'ld-2.17.so'
        subprocess.run([str(TOOLCHAIN/'clang.exe'),'--target='+triple,'-fuse-ld=lld','-nostdlib',
            '-fno-stack-protector','-fno-pic','-no-pie','-O2','-Wall','-Wextra','-Werror',
            '-Wl,--image-base='+base,'-Wl,-e,_start','-Wl,--dynamic-linker=/'+lib+'/ld-2.17.so',
            str(ROOT/'tests/madvise_pthread.c'),'-L'+str(directory/lib),'-l:libpthread.so.0','-l:libc.so.6',
            '-o',str(program)],check=True)
        p=subprocess.run([str(host),'--root',str(directory),sha256(loader),'--run',str(program),sha256(program),'madvise-pthread'],
                         capture_output=True,timeout=30)
        records.append({'arch':arch,'case':'glibc-pthread-stack-reclaim','fixture_sha256':sha256(program),
            'host_sha256':sha256(host),'exit':p.returncode,'stdout':p.stdout.decode(errors='replace'),
            'stderr':p.stderr.decode(errors='replace'),'passed':p.returncode==42 and p.stdout==b'madvise-pthread-ok\n'})
        print(arch,'glibc pthread stack reclaim',records[-1]['passed'],'exit',p.returncode,flush=True)
    (BUILD/'madvise-checks.json').write_text(json.dumps({'os':platform.platform(),
        'recorded_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'scope':'Private anonymous DONTNEED exact zero contents, mixed protections, rounded range, unchanged neighbors, holes, initially PROT_NONE, 2000 resets with a concurrent reader and 24 original-glibc pthread self-discard/join/reclaim rounds',
        'results':records},indent=2),encoding='utf8')
    if not all(r['passed'] for r in records):raise SystemExit('DONTNEED checks failed')

if __name__=='__main__':main()
