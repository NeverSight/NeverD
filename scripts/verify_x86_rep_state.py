#!/usr/bin/env python3
"""Qualify NeverD REP exports against the NeverC independent machine oracle.

First run NeverDX86RepStateTests with NEVERD_REP_EXPORT_DIR set. Supply the
qualified NeverC checkout's tests/verify-rep-state.py as --oracle. Native x64
checks use original instruction bytes, a sequential memory/flags model and
real page-fault restart. i386 checks use Unicorn; port completion is controlled
and never performs host port I/O. Requires capstone, lief and (for i386) unicorn.
The pipeline route cross-compiles complete decompiled functions in all three
object formats; it checks encoding without executing their fixtures.
"""

import argparse
import ctypes as ct
import importlib.util
import json
from pathlib import Path
import platform
import re
import shutil
import sys
import tempfile
from types import SimpleNamespace

import capstone
import lief


def load_oracle(path):
    spec = importlib.util.spec_from_file_location('neverd_rep_reference', path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def inspect_object(path, forms, bits):
    binary = lief.parse(str(path))
    section = binary.get_section('.text')
    decoder = capstone.Cs(capstone.CS_ARCH_X86,
                          capstone.CS_MODE_64 if bits == 64 else capstone.CS_MODE_32)
    decoder.detail = True
    result = {}
    for form in forms:
        symbol = binary.get_symbol(form.name)
        assert symbol is not None, form.name
        code = bytes(section.content[symbol.value:symbol.value + symbol.size])
        instructions = list(decoder.disasm(code, 0))
        repeats = [inst for inst in instructions
                   if inst.mnemonic.startswith(('rep ', 'repe ', 'repne '))
                   and form.op in inst.mnemonic]
        assert len(repeats) == 1, (form, [(i.mnemonic, i.op_str) for i in instructions])
        inst = repeats[0]
        assert inst.addr_size * 8 == form.address, form
        assert bytes(inst.bytes).count(0xf2 if form.repeat else 0xf3) == 1, form
        if form.segment and form.segment != 0x3e:
            assert form.segment in inst.bytes, form
        memory = [operand for operand in inst.operands
                  if operand.type == capstone.x86.X86_OP_MEM]
        assert memory and all(operand.size == form.width for operand in memory), form
        result[form] = code, inst.address, inst.size
    return result


def check_wide_direction(oracle, directory, matrix, obj):
    """Owned asm's i32 condition must survive builtin byte normalization."""
    lib = ct.CDLL(str(directory / (obj.stem + '.so')))
    lib.rep_invoke.argtypes = [ct.c_void_p, ct.POINTER(oracle.U64),
                              ct.POINTER(oracle.U64), ct.c_uint, oracle.U64]
    lib.rep_invoke.restype = oracle.U64
    memory = (ct.c_ubyte * 256)()
    base = ct.addressof(memory)
    initial = bytes((i * 19 + 7) & 255 for i in range(256))
    cases = 0
    for form in matrix:
        if form.port or form.segment or form.address != 64:
            continue
        for direction in (0, 256, 0x80000000, 0xffffffff):
            for count in (0, 3):
                accumulator = 0xfedcba9876543210
                operands = [base + 160, base + 192, count] if form.op in ('movs', 'cmps') else \
                           [base + (160 if form.op == 'lods' else 192), count, accumulator]
                values = (oracle.U64 * 5)(*operands, direction, 0xd501)
                reference = (oracle.U64 * 4)(*[oracle.SENTINEL] * 4)
                actual = (oracle.U64 * 4)(*[oracle.SENTINEL] * 4)
                ct.memmove(base, initial, len(initial))
                lib.rep_invoke(getattr(lib, 'ref_' + form.name), values, reference, 0, 0)
                expected_memory = bytes(memory)
                ct.memmove(base, initial, len(initial))
                host_flags = lib.rep_invoke(getattr(lib, form.name), values, actual, 0, 0)
                assert list(actual) == list(reference), (form, direction, count, list(actual), list(reference))
                assert bytes(memory) == expected_memory, (form, direction, count)
                assert not (host_flags & 0x400), (form, direction, count)
                cases += 1
    return cases


def check_pipeline(args, oracle):
    targets = {'elf': 'unknown-linux-gnu', 'coff': 'pc-windows-msvc',
               'macho': 'apple-darwin'}
    families = ('movs', 'stos', 'lods', 'cmps', 'scas', 'ins', 'outs')
    sources = args.exports.resolve() / 'pipeline'
    cases = 0
    with tempfile.TemporaryDirectory(prefix='neverd-rep-pipeline-') as temporary:
        directory = Path(temporary)
        for fmt, target in targets.items():
            for family in families:
                for pipeline in ('raw', 'opt'):
                    for route in ('high', 'llvm'):
                        name = f'{args.bits}-{fmt}-{family}-{pipeline}-{route}'
                        source = sources / (name + '.c')
                        # Use compiler intrinsic headers, without a target CRT.
                        text = source.read_text().replace(
                            '#include <string.h>',
                            'extern void *memcpy(void *, const void *, __SIZE_TYPE__);\n'
                            'extern void *memset(void *, int, __SIZE_TYPE__);')
                        local = directory / (name + '.c')
                        local.write_text(text)
                        for opt in ('O0', 'O2'):
                            obj = directory / (name + '-' + opt + '.o')
                            triple = ('x86_64' if args.bits == 64 else 'i386') + '-' + target
                            command = [args.compiler, '--target=' + triple,
                                       '-std=gnu11', '-' + opt, '-ffreestanding',
                                       '-fno-lto', '-fno-stack-protector', '-fno-strict-aliasing',
                                       '-Werror=implicit-function-declaration', '-c', local, '-o', obj]
                            if args.neverc:
                                command += ['-fno-builtin-mimalloc', '-mllvm', '-verify-machineinstrs']
                            oracle.run(command)
                            binary = lief.parse(str(obj))
                            section_name = '__text' if fmt == 'macho' else '.text'
                            # COFF may have an empty .text plus function COMDAT
                            # sections with that same name.
                            sections = [s for s in binary.sections if s.name == section_name]
                            assert sections, name
                            decoder = capstone.Cs(capstone.CS_ARCH_X86,
                                                  capstone.CS_MODE_64 if args.bits == 64 else capstone.CS_MODE_32)
                            decoder.detail = True
                            repeats = [inst for section in sections
                                       for inst in decoder.disasm(bytes(section.content), 0)
                                       if inst.mnemonic.startswith(('rep ', 'repe ', 'repne '))
                                       and family in inst.mnemonic]
                            assert len(repeats) == 1, (name, opt, len(repeats))
                            assert repeats[0].addr_size * 8 == (32 if args.bits == 64 else 16), name
                            cases += 1
            print(f'{fmt}: complete {args.bits}-bit pipeline C compiles at O0/O2', flush=True)
    return {'route': 'pipeline', 'bits': args.bits, 'compiler': str(args.compiler),
            'compiled_objects': cases, 'executed': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exports', type=Path, required=True)
    parser.add_argument('--oracle', type=Path, required=True)
    parser.add_argument('--route', choices=('high', 'llvm', 'ir', 'object', 'pipeline'), required=True)
    parser.add_argument('--compiler', default='clang')
    parser.add_argument('--neverc', action='store_true')
    parser.add_argument('--llc', type=Path)
    parser.add_argument('--bits', type=int, choices=(32, 64), default=64)
    parser.add_argument('--host-cc', default='cc')
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    if args.route == 'ir' and not args.llc:
        parser.error('--route ir requires --llc')
    if args.neverc and args.bits != 64:
        parser.error('NeverC supports x64 only')
    if args.route != 'pipeline' and args.bits == 64 and (platform.system() != 'Linux' or
                            platform.machine() not in ('x86_64', 'AMD64')):
        parser.error('native REP oracle requires Linux x86-64')
    oracle = load_oracle(args.oracle.resolve())
    if args.route == 'pipeline':
        result = check_pipeline(args, oracle)
        if args.report:
            args.report.write_text(json.dumps(result, indent=2) + '\n')
        print('PASS:', json.dumps(result), flush=True)
        return
    matrix = list(oracle.forms(args.bits))
    stem = args.exports.resolve() / f'rep-{args.bits}'
    source = Path(f'{stem}.ll' if args.route == 'ir' else
                  f'{stem}-object.o' if args.route == 'object' else f'{stem}-{args.route}.c')
    counts = []
    wide_direction_cases = 0
    # Native LLVM's direction is already i8. Only the owned asm/C status
    # boundary admits conditions wider than a byte.
    wide_direction = args.route == 'high' or '# neverd-rep-state-v1:' in Path(f'{stem}.ll').read_text()
    with tempfile.TemporaryDirectory(prefix='neverd-rep-state-') as temporary:
        directory = Path(temporary)
        if args.route == 'ir' and '# neverd-rep-state-v1:' in source.read_text():
            mir = directory / 'effects.mir'
            oracle.run([args.llc, '-O2', '-mattr=+sahf', '-stop-after=finalize-isel',
                        source, '-o', mir])
            units = [line for line in mir.read_text().splitlines()
                     if 'INLINEASM ' in line and 'neverd-rep-state-v1:' in line]
            assert len(units) == len(matrix), len(units)
            assert all(re.search(r'clobber, implicit-def [^,$]*\$eflags', line) and
                       re.search(r'clobber, implicit-def [^,$]*\$df', line)
                       for line in units)
            print(f'MIR: {len(units)} units declare EFLAGS and DF clobbers', flush=True)
        for opt in (('production',) if args.route == 'object' else ('O0', 'O2')):
            obj = directory / f'{args.route}_{args.bits}_{opt}.o'
            if args.route == 'object':
                shutil.copyfile(source, obj)
                command = None
            elif args.route == 'ir':
                command = [args.llc, '-'+opt, '-mattr=+sahf', '-relocation-model=pic',
                           '-verify-machineinstrs', '-filetype=obj', source, '-o', obj]
            else:
                command = [args.compiler, '-std=gnu11', '-'+opt, '-ffreestanding',
                           '-fno-lto', '-fno-stack-protector', '-fno-strict-aliasing', '-fPIC', '-m'+str(args.bits),
                           '-c', source, '-o', obj]
                if args.neverc:
                    command += ['-fno-builtin-mimalloc', '-mllvm', '-verify-machineinstrs']
            if command is not None:
                oracle.run(command)
            code = inspect_object(obj, matrix, args.bits)
            if args.bits == 32:
                observed = oracle.emulated32_suite(matrix, code)
                print(f'{opt}: {observed[0]} emulated memory cases, '
                      f'{observed[1]} controlled port cases', flush=True)
            else:
                observed = oracle.native_suite(SimpleNamespace(host_cc=args.host_cc),
                                                directory, matrix, obj, code)
                if wide_direction:
                    wide_direction_cases += check_wide_direction(oracle, directory, matrix, obj)
                print(f'{opt}: {observed[0]} memory/flags cases, '
                      f'{observed[1]} hardware fault restarts, '
                      f'{observed[2]} controlled port completions', flush=True)
            counts.append(observed)
    result = {'route': args.route, 'bits': args.bits, 'forms': len(matrix),
              'compiler': 'NeverD Codegen' if args.route == 'object' else str(args.llc or args.compiler),
              'totals': [sum(values) for values in zip(*counts)],
              'hardware_restart': args.bits == 64, 'physical_port_io': False,
              'wide_direction_cases': wide_direction_cases}
    if args.report:
        args.report.write_text(json.dumps(result, indent=2) + '\n')
    print('PASS:', json.dumps(result), flush=True)


if __name__ == '__main__':
    main()
