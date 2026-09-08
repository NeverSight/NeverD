#!/usr/bin/env python3
"""Compile, recover through the CLI, rebuild Swift only, and compare behavior.

Requires macOS, its Swift toolchain, and a built NeverD. No downloads occur.
Default verification fails on missing declarations, unclassified symbols, any
unrecovered callable, compilation errors, or behavioral differences. Compiler
accessors, allocators and runtime thunks remain in the inventory and counts.
--setup-only verifies the originals against an independent mathematical oracle;
it does not verify decompilation. Use --work-dir to retain failure evidence.
"""
from __future__ import annotations

import argparse
from collections import Counter
from contextlib import nullcontext
import errno
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import struct
import subprocess
import sys
import tempfile

from test_mobile_ios_backend import assert_results, chained_fixups

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "scripts/tests/fixtures/mobile/SwiftBehavior.swift"
HARNESS = ROOT / "scripts/tests/fixtures/mobile/SwiftBehaviorHarness.swift"
MODULE = "SwiftBehavior"
VALUES = (-(2**63), -(2**63) + 1, -65537, -1, 0, 1, 65537, 2**63 - 2, 2**63 - 1)
GLOBALS = {"scalar", "choose", "callScalar", "floatIdentity", "doubleIdentity", "floatAdd", "doubleAdd",
           "mixed", "stackIntegers", "stackFloats", "stackMixed", "pointerRead", "pointerSwap", "sum"}
DECLARATIONS = {*(('global', '', name, 'function') for name in GLOBALS),
                ('class', 'Calculator', 'init', 'initializer'),
                ('class', 'Calculator', 'add', 'function'),
                ('class', 'Calculator', 'selfCall', 'function'),
                ('class', 'Calculator', 'combine', 'function'),
                ('struct', 'Counter', 'init', 'initializer'),
                *(('struct', 'Counter', name, 'function') for name in ('add', 'affine', 'adjust'))}


def run(argv: list[str], *, timeout: int = 120) -> str:
    process = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, start_new_session=True)
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except BaseException:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.communicate()
        raise
    if process.returncode:
        raise RuntimeError(f'{Path(argv[0]).name} exited {process.returncode}:\n{stdout}\n{stderr}')
    return stdout


def execution_results(binary: Path) -> dict[str, int]:
    result = {}
    for line in run([str(binary)], timeout=30).splitlines():
        key, separator, value = line.partition('=')
        if not separator or key in result:
            raise RuntimeError(f'Unexpected or duplicate fixture output: {line!r}')
        result[key] = int(value)
    return result


def wrap(value: int) -> int:
    return (value + 2**63) % 2**64 - 2**63


def bits(value: float, width: int) -> int:
    return int.from_bytes(struct.pack('<f' if width == 32 else '<d', value), 'little')


def expected_results() -> dict[str, int]:
    result = {}
    for x_index, x in enumerate(VALUES):
        result[f'class-init:{x_index}'] = x
        result[f'struct-init:{x_index}'] = x
        for y_index, y in enumerate(VALUES):
            index = x_index * len(VALUES) + y_index
            for name, value in {
                'scalar': wrap(x + y), 'choose': min(x, y), 'callScalar': wrap((x + y) * 3),
                'class-add': wrap(x + y), 'class-selfCall': wrap(x + y - 5), 'class-combine': wrap(x - y),
                'struct-add': wrap(x + y), 'struct-affine': wrap(x * 7 + y),
                'struct-adjust': wrap(x * 7 + y),
            }.items():
                result[f'{name}:{index}'] = value
    for name, values in {
        'floatIdentity': (0, 0x80000000, 0x7f800000, 0xff800000, 0x7fc12345, 1, 0xc0f00000),
        'doubleIdentity': (0, 0x8000000000000000, 0x7ff0000000000000, 0xfff0000000000000,
                           0x7ff8000000001234, 1, 0xc020800000000000),
    }.items():
        result.update((f'{name}:{index}', value) for index, value in enumerate(values))
    for index in range(-3, 4):
        suffix = index + 3
        result[f'floatAdd:{suffix}'] = bits(index * 2.25 - 7.5, 32)
        result[f'doubleAdd:{suffix}'] = bits(index * 3.125 - 1024.5, 64)
        result[f'mixed:{suffix}'] = bits(index - 13.25 + 10000000000 + 2.5, 64)
        result[f'stackIntegers:{suffix}'] = index - 32767 + 65537 + 10000000000 - 127 + 32767 - 65537 - 10000000000 - 126 - 32766 + 65536 + 17
        result[f'stackFloats:{suffix}'] = bits(sum((index, -3.25, 5.5, -7.75, 11.25, -13.5, 17.75, -19.25, 23.5, -29.75, 31.25, -37.5)), 32)
        result[f'stackMixed:{suffix}'] = bits(sum((index, -3, 5, -7, 11, -13, 17, -19,
                                                2.25, -3.5, 5.75, -7.25, 11.5, -13.75, 17.25, -19.5,
                                                -127, 2.5, -32767, 3.25, 65537, -23.5, 10000000000)), 64)
    cells = (-(2**31), -1, 0, 1, 2**31 - 1)
    for index, value in enumerate(cells):
        result[f'pointerRead:{index}'] = result[f'pointerSwap:{index}'] = value
        result[f'pointerAfter:{index}'] = cells[len(cells) - index - 1]
    sum_values = [-5, 7, 0, 9, -3] + [
        -(1 << 31) if index % 7 == 0 else (1 << 31) - 1 if index % 7 == 1 else index * index - 97
        for index in range(5, 35)
    ]
    for count in range(-1, len(sum_values) + 1):
        result[f'sum:{count + 1}'] = sum(sum_values[:max(count, 0)])
    return result


def validate_coverage(output: Path, original: Path, architecture: str) -> dict:
    report = json.loads((output / 'report.json').read_text())
    coverage = json.loads((output / 'metadata/swift-methods.json').read_text())
    inventory = json.loads((output / 'metadata/swift-signatures.json').read_text())
    if (report.get('status'), report.get('platform'), report.get('architecture')) != ('success', 'ios', architecture):
        raise RuntimeError('The mobile report does not identify the successful selected architecture')
    if report.get('swift_method_recovery') != coverage:
        raise RuntimeError('Standalone Swift coverage differs from report.json')
    identity = lambda row: (row.get('entry'), row.get('mangled_symbol'))
    methods, symbols = coverage.get('methods', []), coverage.get('non_method_symbols', [])
    if Counter(map(identity, methods)) != Counter(map(identity, inventory.get('methods', []))):
        raise RuntimeError('Callable inventory entries were omitted, substituted, or duplicated')
    if Counter(map(identity, symbols)) != Counter(map(identity, inventory.get('symbols', []))):
        raise RuntimeError('Metadata or unclassified symbols were omitted from coverage')
    # nm provides an independent inventory of every symbol retained in the
    # input dylib, including undefined metadata references and compiler thunks.
    raw_names = Counter(name for name in run(['/usr/bin/nm', '-a', '-j', str(original)]).splitlines()
                        if name.lstrip('_').startswith(('$s', '$S', 'T0')))
    reported_names = Counter(row.get('mangled_symbol') for row in [*methods, *symbols])
    if raw_names != reported_names:
        raise RuntimeError(f'Swift symbol inventory is incomplete: missing={raw_names - reported_names}, extra={reported_names - raw_names}')
    if any(row.get('classification') != 'callable' or not row.get('node_kind') for row in methods):
        raise RuntimeError('A callable lacks its structural demangling classification')
    if any(row.get('classification') not in {'metadata', 'unknown'} or not row.get('node_kind') for row in symbols):
        raise RuntimeError('A non-method symbol lacks an explicit classification')
    errors = []
    declared = {}
    for row in methods:
        key = tuple(row.get(field) for field in ('context_kind', 'context_name', 'name', 'declaration_kind'))
        # A class's allocator is a separate compiler callable; the initializing
        # constructor is its source declaration. Both remain in full coverage.
        if row.get('context_kind') == 'class' and row.get('node_kind') == 'Allocator':
            continue
        if key in DECLARATIONS:
            if key in declared:
                errors.append(f'duplicate declaration {key}')
            declared[key] = row
    for key in sorted(DECLARATIONS):
        if key not in declared:
            errors.append(f'missing declared method {key}')
        elif declared[key].get('status') != 'recovered':
            errors.append(f'unrecovered declared method {key}: {declared[key].get("reason")}')
    unrecovered = [row for row in methods if row.get('status') != 'recovered']
    for row in unrecovered:
        if row.get('status') != 'unrecovered' or not isinstance(row.get('reason'), str) or not row['reason']:
            errors.append(f'missing explicit failure reason for {row.get("mangled_symbol")}')
        else:
            errors.append(f'{row.get("node_kind")}: {row.get("mangled_symbol")}: {row["reason"]}')
    recovered = len(methods) - len(unrecovered)
    compiler_projections = 0
    for row in methods:
        if row.get('status') != 'recovered':
            continue
        if row.get('declaration_kind') == 'runtime':
            compiler_projections += 1
            evidence = row.get('compiler_projection_evidence')
            if (row.get('source_representation') != 'compiler-generated-from-type'
                    or not row.get('compiler_projection_kind')
                    or not isinstance(evidence, list) or not evidence
                    or any(not isinstance(item, str) or not item.strip() for item in evidence)):
                errors.append(f'compiler callable lacks explicit projection evidence: {row.get("mangled_symbol")}')
        elif (row.get('source_representation') != 'native-method-body'
              or 'compiler_projection_kind' in row or 'compiler_projection_evidence' in row):
            errors.append(f'ordinary method lacks native body provenance: {row.get("mangled_symbol")}')
    expected_counts = {'method_count': len(methods), 'recovered_method_count': recovered,
                       'unrecovered_method_count': len(unrecovered), 'symbol_count': len(methods) + len(symbols),
                       'source_body_method_count': recovered - compiler_projections,
                       'compiler_projection_method_count': compiler_projections}
    if any(type(coverage.get(key)) is not int or coverage[key] != count for key, count in expected_counts.items()):
        errors.append('aggregate counts disagree with the actual inventory')
    if coverage.get('unclassified_symbol_count') or any(row.get('classification') == 'unknown' for row in symbols):
        errors.append('unclassified symbols could conceal callable coverage')
    if (coverage.get('status'), coverage.get('coverage_status')) != ('recovered', 'recovered'):
        errors.append(f'coverage is {coverage.get("status")}/{coverage.get("coverage_status")}')
    if errors:
        raise RuntimeError('Incomplete Swift recovery:\n' + '\n'.join(errors))
    return coverage


def verify(arguments: argparse.Namespace, work: Path) -> None:
    swift = shutil.which('swiftc')
    if sys.platform != 'darwin' or not swift:
        raise RuntimeError('This execution test requires macOS and its Swift compiler/SDK')
    if not arguments.setup_only and not arguments.neverd:
        raise RuntimeError('Pass --neverd PATH or --setup-only')
    demangler = run(['xcrun', '--find', 'swift-demangle']).strip()
    module_cache = arguments.module_cache.resolve() if arguments.module_cache else work / 'module-cache'
    architectures = ('arm64', 'x86_64') if arguments.arch == 'all' else (arguments.arch,)
    fixups = ('classic', 'default') if arguments.fixups == 'both' else (arguments.fixups,)
    expected = expected_results()
    completed = 0
    failures = []
    for architecture in architectures:
        for fixup in fixups:
            label = f'{architecture}-{fixup}'
            variant = work / label
            variant.mkdir()
            try:
                flags = [swift, '-target', f'{architecture}-apple-macosx13.0', '-O',
                         '-module-cache-path', str(module_cache)]
                linker = ['-Xlinker', '-no_fixup_chains'] if fixup == 'classic' else []
                library = variant / 'libSwiftBehavior.dylib'
                run([*flags, '-emit-library', '-emit-module', '-module-name', MODULE,
                     '-emit-module-path', str(variant / f'{MODULE}.swiftmodule'), *linker,
                     '-Xlinker', '-install_name', '-Xlinker', '@rpath/libSwiftBehavior.dylib',
                     str(FIXTURE), '-o', str(library)], timeout=arguments.timeout)
                chained = chained_fixups(library)
                if fixup == 'classic' and chained:
                    raise RuntimeError('The classic variant unexpectedly contains chained fixups')
                original = variant / 'original'
                run([*flags, '-parse-as-library', '-D', 'ORIGINAL_FIXTURE', '-I', str(variant), '-L', str(variant),
                     '-lSwiftBehavior', '-Xlinker', '-rpath', '-Xlinker', str(variant), str(HARNESS), '-o', str(original)], timeout=arguments.timeout)
                try:
                    baseline = execution_results(original)
                except OSError as error:
                    if arguments.arch == 'all' and architecture != platform.machine() and error.errno in (errno.ENOEXEC, 86):
                        print(f'SKIP {label}: this host cannot execute {architecture}', flush=True)
                        break
                    raise
                assert_results(baseline, expected, f'Original {label}')
                (variant / 'expected.json').write_text(json.dumps(expected, indent=2))
                print(f'PASS original {label}: {len(expected)} oracle checks, chained_fixups={chained}', flush=True)
                if not arguments.setup_only:
                    output = variant / 'recovered'
                    run([str(arguments.neverd.resolve()), 'mobile', str(library), '-o', str(output),
                         '--platform=ios', f'--arch={architecture}',
                         '--swift-demangle', demangler, f'--timeout={arguments.timeout}'], timeout=arguments.timeout * 3 + 60)
                    coverage = validate_coverage(output, library, architecture)
                    source = output / 'sources/swift.swift'
                    if not source.is_file() or not source.read_text().strip():
                        raise RuntimeError('The CLI produced no actual Swift source')
                    rebuilt = variant / 'rebuilt'
                    # No original fixture, dylib, bridge, module import, or
                    # handcrafted declaration participates in this compilation.
                    run([*flags, '-parse-as-library', str(source), str(HARNESS), '-o', str(rebuilt)], timeout=arguments.timeout)
                    assert_results(execution_results(rebuilt), baseline, f'Recovered {label}')
                    print(f'PASS recovered {label}: {coverage["source_body_method_count"]} native bodies, '
                          f'{coverage["compiler_projection_method_count"]} compiler projections, '
                          f'{len(expected)} behavior checks', flush=True)
                completed += 1
            except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
                (variant / 'failure.txt').write_text(str(error))
                failures.append(f'{label}: {error}')
                print(f'FAIL {label}: {error}', file=sys.stderr, flush=True)
    if failures:
        raise RuntimeError(f'{len(failures)} Swift variants failed; evidence: {work}\n' + '\n'.join(failures))
    if not completed:
        raise RuntimeError('No Swift variant was executed')
    print(f'Verified {completed} {"original-only" if arguments.setup_only else "recovered"} variants')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--neverd', type=Path)
    parser.add_argument('--setup-only', action='store_true')
    parser.add_argument('--arch', choices=('all', 'arm64', 'x86_64'), default='all')
    parser.add_argument('--fixups', choices=('both', 'classic', 'default'), default='both')
    parser.add_argument('--timeout', type=int, default=300, help='seconds per compiler or mobile backend step')
    parser.add_argument('--module-cache', type=Path, help='optional reusable Swift compiler module cache; defaults inside the work directory')
    parser.add_argument('--work-dir', type=Path, help='new directory retaining every generated artifact and failure')
    arguments = parser.parse_args()
    if arguments.timeout <= 0:
        parser.error('--timeout must be positive')
    if arguments.work_dir:
        work = arguments.work_dir.resolve()
        work.mkdir(parents=True, exist_ok=False)
        context = nullcontext(str(work))
    else:
        context = tempfile.TemporaryDirectory(prefix='neverd-swift-execution-')
    with context as directory:
        verify(arguments, Path(directory))
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired, struct.error) as error:
        print(f'error: {error}', file=sys.stderr)
        raise SystemExit(1)
