from pathlib import Path
import hashlib
import ctypes
import json
import platform
import shutil
import struct
import subprocess
import time

root = Path(__file__).resolve().parents[1]
out = root / 'build-fls-reference'
out.mkdir(exist_ok=False)
source = root / 'scripts/fls-native-probe.c'
compiler = shutil.which('clang')
linker = shutil.which('lld-link')
assert compiler and linker
commands = [
    [compiler, '--target=x86_64-pc-windows-msvc', '-std=c11', '-ffreestanding',
     '-fno-builtin', '-fno-stack-protector', '-O1', '-c', str(source), '-o', str(out / 'probe.obj')],
    [linker, '/nodefaultlib', '/entry:entry', '/subsystem:console', '/machine:x64',
     '/base:0x140000000', '/timestamp:0', '/include:_tls_used', str(out / 'probe.obj'),
     'kernel32.lib', '/out:' + str(out / 'probe.exe')],
]
meta = dict(commit=subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
            source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
            platform=platform.platform(), compiler=compiler, linker=linker, commands=commands)
for index, argv in enumerate(commands):
    q = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
    (out / ('build-' + str(index) + '.log')).write_bytes(q.stdout)
    q.check_returncode()
meta['compiler_version'] = subprocess.check_output([compiler, '--version'], text=True)
meta['binary_sha256'] = hashlib.sha256((out / 'probe.exe').read_bytes()).hexdigest()
(out / 'build.json').write_text(json.dumps(meta, indent=2) + '\n')
rows = []
ctypes.windll.kernel32.SetErrorMode(0x8003)
for mode in ['F', 'N', 'E', 'S', 'G', 'R']:
    for repeat in range(2):
        start = time.monotonic()
        timed_out = False
        stdout_path = out / (mode + '-' + str(repeat) + '.stdout')
        stderr_path = out / (mode + '-' + str(repeat) + '.stderr')
        with stdout_path.open('wb') as stdout, stderr_path.open('wb') as stderr:
            try:
                q = subprocess.run([str(out / 'probe.exe'), '!' + mode], stdout=stdout,
                                   stderr=stderr, timeout=15)
                code = q.returncode
            except subprocess.TimeoutExpired:
                timed_out = True
                code = None
        raw = stdout_path.read_bytes()
        errors = stderr_path.read_bytes()
        assert len(raw) % 64 == 0
        records = [list(struct.unpack('<8Q', raw[i:i+64])) for i in range(0, len(raw), 64)]
        readable = [dict(mode=chr(a[0]), tag=chr(a[1]), values=a[2:]) for a in records]
        row = dict(mode=mode, repeat=repeat, exit_code=code, timed_out=timed_out,
                   seconds=round(time.monotonic()-start, 3), stdout_hex=raw.hex(),
                   stderr_hex=errors.hex(), records=readable, inherited_error_mode=0x8003)
        rows.append(row)
        print(json.dumps(row), flush=True)
        (out / 'partial-observations.json').write_text(json.dumps(rows, indent=2) + '\n')
(out / 'observations.json').write_text(json.dumps(rows, indent=2) + '\n')
assert all(row['exit_code'] == 43 and not row['stderr_hex'] for row in rows)
