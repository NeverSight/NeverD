"""Replay the captured C fixtures independently on their failing Windows host."""
from pathlib import Path
import ctypes
import hashlib
import json
import os
import subprocess

root = Path(__file__).resolve().parent
manifest = json.loads((root / 'manifest.json').read_text())
assert manifest['failed_source'] == os.environ['EXPECTED_SOURCE_SHA']
out = Path('llvmc-source-evidence').resolve()
out.mkdir()
compiler = Path(os.environ['NEVERD_SOURCE_CLANG'])
reader = compiler.parent / 'llvm-readobj.exe'
ctypes.windll.kernel32.SetErrorMode(3)
rows = []

def digest(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

def run(name, command, env=None):
    result = subprocess.run([str(x) for x in command], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            env=env, timeout=60)
    (out / (name + '.log')).write_text(result.stdout)
    return result.returncode, result.stdout

_, version = run('compiler-version', [compiler, '--version'])
code, runtime = run('runtime-directory', [compiler, '-print-runtime-dir'])
assert code == 0
runtime = Path(runtime.strip())
metadata = dict(manifest, workflow_source=os.environ['GITHUB_SHA'],
                compiler=str(compiler), compiler_sha256=digest(compiler),
                compiler_version=version, runtime_directory=str(runtime),
                original_path=os.environ.get('PATH', ''))
(out / 'source.json').write_text(json.dumps(metadata, indent=2) + '\n')

names = ['EnterCriticalSection', '_onexit', '_set_app_type',
         '_configure_narrow_argv', '_configure_wide_argv', '__setusermatherr',
         '_initialize_onexit_table', '_register_onexit_function',
         '_execute_onexit_table', '_register_thread_local_exe_atexit_callback']
remap = ''.join('#define ' + x + ' neverd_fixture' + x + '\n' for x in names)

for case in manifest['cases']:
    original = root / case['file']
    assert digest(original) == case['sha256']
    wide = case['name'].startswith('WideFrameStore')
    flags = (['-O0', '-fno-inline', '-fblocks', '-fsanitize=address,undefined',
              '-fno-sanitize-recover=all'] if wide else ['-O0'])
    if case['name'].startswith('WindowsCRT'):
        flags += ['-fsanitize=undefined', '-fsanitize-trap=all']
    for variant in ['baseline', 'matched_runtime' if wide else 'renamed_mocks']:
        name = case['name'] + '-' + variant
        source, exe = out / (name + '.c'), out / (name + '.exe')
        source.write_text((remap if variant == 'renamed_mocks' else '') +
                          original.read_text())
        cmd = [compiler, '-std=c11', '-fno-strict-aliasing',
               '-Werror=implicit-function-declaration', '-Werror=return-type',
               *flags, source, '-o', exe]
        result, _ = run(name + '-compile', cmd)
        row = dict(case=case['name'], variant=variant, compile=result,
                   command=[str(x) for x in cmd], source_sha256=digest(source))
        if result == 0:
            row['exe_sha256'] = digest(exe)
            if reader.exists():
                run(name + '-imports', [reader, '--coff-imports', exe])
            env = dict(os.environ)
            if variant == 'matched_runtime':
                env['PATH'] = str(runtime) + ';' + str(compiler.parent) + ';' + env['PATH']
            row['run'], _ = run(name + '-execute', [exe], env)
        rows.append(row)
        (out / 'results.json').write_text(json.dumps(rows, indent=2) + '\n')
        if exe.exists(): exe.unlink()
# Diagnose DLL selection without retaining generated programs or installed models.
dlls = []
for directory in [runtime, compiler.parent, *map(Path, os.environ['PATH'].split(';'))]:
    candidate = directory / 'clang_rt.asan_dynamic-x86_64.dll'
    if candidate.is_file() and str(candidate) not in [r['path'] for r in dlls]:
        row = {'path': str(candidate), 'sha256': digest(candidate)}
        if reader.exists():
            run('asan-exports-' + str(len(dlls)), [reader, '--coff-exports', candidate])
        dlls.append(row)
(out / 'runtime-candidates.json').write_text(json.dumps(dlls, indent=2) + '\n')
for path in out.iterdir():
    if path.suffix.lower() in ('.obj', '.pdb', '.ilk', '.lib', '.exp'):
        path.unlink()
print(json.dumps(rows, indent=2))
