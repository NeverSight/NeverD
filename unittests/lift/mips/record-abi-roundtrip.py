#!/usr/bin/env python3
"""Replay o32/n32/n64 record entries and direct/indirect calls across compilers.

Each replacement links with the other original objects. The caller observes
defined member bytes, untouched output sentinels, preserved machine state and
exactly one callee entry. Register/stack splits and hidden result pointers are
therefore tested against an independently compiled caller, not a second copy
of NeverD's classification. Padding bytes are not source values.
"""

import argparse
import importlib.util
import io
import json
import os
import random
from pathlib import Path

from elftools.elf.elffile import ELFFile
from unicorn import UC_HOOK_CODE
from unicorn import mips_const as mips

import roundtrip
from mips_validation_artifacts import MIPSValidationArtifacts, digest


CASES = (
    ('b3', 'struct { unsigned char x[3]; }', 3, list(range(3))),
    ('b5', 'struct { unsigned char x[5]; }', 5, list(range(5))),
    ('b6', 'struct { unsigned char x[6]; }', 6, list(range(6))),
    ('b7', 'struct { unsigned char x[7]; }', 7, list(range(7))),
    ('b9', 'struct { unsigned char x[9]; }', 9, list(range(9))),
    ('b10', 'struct { unsigned char x[10]; }', 10, list(range(10))),
    ('b13', 'struct { unsigned char x[13]; }', 13, list(range(13))),
    ('b15', 'struct { unsigned char x[15]; }', 15, list(range(15))),
    ('b16', 'struct { unsigned char x[16]; }', 16, list(range(16))),
    ('b17', 'struct { unsigned char x[17]; }', 17, list(range(17))),
    ('u32', 'struct { unsigned x; }', 4, list(range(4))),
    ('u64', 'struct { unsigned long long x; }', 8, list(range(8))),
    ('f1', 'struct { float x; }', 4, list(range(4))),
    ('f2', 'struct { float x, y; }', 8, list(range(8))),
    ('f3', 'struct { float x, y, z; }', 12, list(range(12))),
    ('d1', 'struct { double x; }', 8, list(range(8))),
    ('d2', 'struct { double x, y; }', 16, list(range(16))),
    ('fd', 'struct { float x; double y; }', 16, [*range(4), *range(8, 16)]),
    ('df', 'struct { double x; float y; }', 16, list(range(12))),
    ('di', 'struct { double x; unsigned long long y; }', 16, list(range(16))),
    ('du', 'union { double x; unsigned long long y; }', 8, list(range(8))),
    ('da', 'struct { double x[2]; }', 16, list(range(16))),
    ('dn', 'struct { struct { double x; } a, b; }', 16, list(range(16))),
    ('p1', 'struct { void *x; }', {'n32': 4, '64': 8},
     {'n32': list(range(4)), '64': list(range(8))}),
    ('pb', 'struct { void *x; unsigned char y; }', {'n32': 8, '64': 16},
     {'n32': list(range(5)), '64': list(range(9))}),
    ('bp', 'struct { unsigned char x; void *y; }', {'n32': 8, '64': 16},
     {'n32': [0, *range(4, 8)], '64': [0, *range(8, 16)]}),
)
DATA = 0x220000
TAIL_VALUE = 0x62481a523967a5c3
MEMORY_RUNTIME = Path(__file__).with_name('mips-source-memory-runtime.c')
CALL_VIEWS_SPEC = importlib.util.spec_from_file_location(
    'mips_floating_abi', Path(__file__).with_name('floating-abi-roundtrip.py'))
CALL_VIEWS = importlib.util.module_from_spec(CALL_VIEWS_SPEC)
CALL_VIEWS_SPEC.loader.exec_module(CALL_VIEWS)


def prefix_values(count):
    return [0x71395317f1c2d3e4 ^ ((i + 1) * 0x0101010101010101)
            for i in range(count)]


def argument_mix(prefix):
    value = TAIL_VALUE
    for argument in prefix_values(prefix):
        value = (((value << 9) | (value >> 55)) & ((1 << 64) - 1)) ^ argument
    return value


def cases_for_abi(abi, selected):
    pointer_layout = '64' if abi == '64' else 'n32'
    return [(name, definition, size[pointer_layout] if isinstance(size, dict) else size,
             defined[pointer_layout] if isinstance(defined, dict) else defined)
            for name, definition, size, defined in CASES
            if not selected or name in selected]


def sources(case, prefix):
    name, definition, size, _ = case
    params = [f'unsigned long long p{i}' for i in range(prefix)]
    params += [f'T_{name} value', 'unsigned long long tail']
    signature = ', '.join(params)
    declaration = f'T_{name} record_callee({signature});'
    header = (f'typedef {definition} T_{name};\n'
              f'_Static_assert(sizeof(T_{name}) == {size}, "record size");\n'
              + declaration + '\n')
    # Every scalar surrounding the record contributes to its result. An
    # identity callee would miss dropped prefix/tail arguments and wrong
    # register-to-stack advancement at a7, despite passing every byte check.
    mix = '\n'.join(f'  bits = ((bits << 9) | (bits >> 55)) ^ p{i};'
                    for i in range(prefix))
    texts = {'record_callee': (
        f'T_{name} record_callee({signature}) {{\n'
        '  unsigned long long bits = tail;\n' + mix + '\n'
        f'  for (unsigned i = 0; i < {size}; ++i)\n'
        '    ((unsigned char *)&value)[i] ^= (unsigned char)(bits >> ((i & 7) * 8));\n'
        '  return value;\n}\n')}
    arguments = [f'0x{value:016x}ULL' for value in prefix_values(prefix)]
    arguments += ['value', f'0x{TAIL_VALUE:016x}ULL']
    for indirect in (False, True):
        function = 'record_indirect' if indirect else 'record_direct'
        callback = f', T_{name} (*callback)({signature})' if indirect else ''
        prototype = f'void {function}(const unsigned char *in, unsigned char *out{callback})'
        header += prototype + ';\n'
        callee = 'callback' if indirect else 'record_callee'
        texts[function] = (
            prototype + ' {\n' + f'  T_{name} value;\n'
            f'  for (unsigned i = 0; i < {size}; ++i) ((unsigned char *)&value)[i] = in[i];\n'
            f'  T_{name} result = {callee}({", ".join(arguments)});\n'
            f'  for (unsigned i = 0; i < {size}; ++i) out[i] = ((unsigned char *)&result)[i];\n'
            '}\n')
    return header, declaration, texts


def observe(image, little, abi, case, function, prefix):
    _, _, size, defined = case
    o32 = abi == '32'
    mask = (1 << (32 if o32 else 64)) - 1
    cpu, entry = roundtrip.emulator(
        image, 32 if o32 else 64, little, function,
        cpu_model=mips.UC_CPU_MIPS32_74KF if o32 else mips.UC_CPU_MIPS64_MIPS64R2_GENERIC)
    elf = ELFFile(io.BytesIO(image.read_bytes()))
    symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    callee = symbols['record_callee']
    cpu.mem_map(DATA, 0x1000)
    status = cpu.reg_read(mips.UC_MIPS_REG_CP0_STATUS) | (1 << 29)
    fr = 1 << 26
    cpu.reg_write(mips.UC_MIPS_REG_CP0_STATUS, status & ~fr if o32 else status | fr)
    gprs = {r: (0x71395217d1c30000 + r) & mask for r in (*range(16, 24), 28, 30)}
    preserved_fp = {'32': range(20, 32), 'n32': range(20, 32, 2), '64': range(24, 32)}
    fprs = {r: (0x3ff013579bdf0000 + r) & mask for r in preserved_fp[abi]}
    rng = random.Random(0x4d495053)
    values = [bytes(size), bytes([0xff]) * size]
    values += [bytes(rng.getrandbits(8) for _ in range(size)) for _ in range(6)]
    rows, calls = [], []
    mix = argument_mix(prefix)
    returned = []
    return_pc = None
    result_pointer = None
    poison_v0 = False

    def visit(uc, pc, instruction_size, user):
        nonlocal return_pc, result_pointer
        if pc == callee:
            calls.append(pc)
            return_pc = uc.reg_read(mips.UC_MIPS_REG_RA)
            if o32:
                result_pointer = uc.reg_read(mips.UC_MIPS_REG_4)
        elif pc == return_pc:
            # n32/n64 hidden-result functions need not return their buffer
            # pointer in v0. Change it after the callee's delay slot and before
            # the caller observes it; both compiler and reconstructed callers
            # must keep using the originally supplied output storage.
            returned.append(pc)
            return_pc = None
            if o32:
                assert uc.reg_read(mips.UC_MIPS_REG_V0) == result_pointer, (image, function)
            if poison_v0:
                uc.reg_write(mips.UC_MIPS_REG_V0, 0x5a71395362481a53)

    hook = cpu.hook_add(UC_HOOK_CODE, visit)
    try:
        attempts = [(value, poison) for value in values
                    for poison in ((False, True) if not o32 and size > 16 else (False,))]
        for value, poison_v0 in attempts:
            for r in range(1, 32):
                cpu.reg_write(getattr(mips, f'UC_MIPS_REG_{r}'),
                              gprs.get(r, (0x5a0000005a000000 + r) & mask))
            for r in range(32):
                cpu.reg_write(getattr(mips, f'UC_MIPS_REG_F{r}'),
                              fprs.get(r, (0xa5000000a5000000 + r) & mask))
            cpu.mem_write(roundtrip.STACK - 8192, bytes([0xcc]) * 16384)
            cpu.mem_write(DATA, value + bytes(128 - size))
            cpu.mem_write(DATA + 240, bytes([0xa5]) * 144)
            cpu.reg_write(mips.UC_MIPS_REG_SP, roundtrip.STACK)
            cpu.reg_write(mips.UC_MIPS_REG_RA, roundtrip.STOP)
            cpu.reg_write(mips.UC_MIPS_REG_4, DATA)
            cpu.reg_write(mips.UC_MIPS_REG_5, DATA + 256)
            cpu.reg_write(mips.UC_MIPS_REG_6, callee)
            cpu.reg_write(mips.UC_MIPS_REG_FCSR, 0)
            calls.clear()
            returned.clear()
            return_pc = None
            cpu.emu_start(entry, roundtrip.STOP, count=20000)
            output = bytes(cpu.mem_read(DATA + 256, 128))
            context = (str(image), function, value.hex(), poison_v0)
            assert calls == [callee], (context, calls)
            assert len(returned) == 1, (context, returned)
            expected = bytes(byte ^ ((mix >> ((i & 7) * 8)) & 0xff)
                             for i, byte in enumerate(value))
            assert all(output[i] == expected[i] for i in defined), (context, output.hex())
            assert bytes(cpu.mem_read(DATA + 240, 16)) == bytes([0xa5]) * 16, context
            assert output[size:] == bytes([0xa5]) * (128 - size), context
            assert bytes(cpu.mem_read(DATA, size)) == value, context
            assert cpu.reg_read(mips.UC_MIPS_REG_PC) == roundtrip.STOP, context
            assert cpu.reg_read(mips.UC_MIPS_REG_RA) == roundtrip.STOP, context
            assert cpu.reg_read(mips.UC_MIPS_REG_SP) == roundtrip.STACK, context
            assert cpu.reg_read(mips.UC_MIPS_REG_FCSR) == 0, context
            for r, expected in gprs.items():
                assert cpu.reg_read(getattr(mips, f'UC_MIPS_REG_{r}')) == expected, (context, r)
            for r, expected in fprs.items():
                assert cpu.reg_read(getattr(mips, f'UC_MIPS_REG_F{r}')) == expected, (context, r)
            rows.append(dict(input=value.hex(), poisoned_v0=poison_v0,
                             defined_output=bytes(output[i] for i in defined).hex()))
    finally:
        cpu.hook_del(hook)
    return rows


def run(args, artifacts):
    views = CALL_VIEWS.CallSiteViews(args.sdk) if args.neverd else None
    rows = []
    compilers = {'clang': args.clang, 'gcc': args.gcc}

    def command(argv):
        roundtrip.run_command(artifacts, argv, artifacts.work / 'command.log')

    for abi in args.abi or ('32', 'n32', '64'):
        cases = cases_for_abi(abi, args.case)
        architecture = 'mips32r2' if abi == '32' else 'mips64r2'
        machine = ['-march=' + architecture, '-mabi=' + abi,
                   '-mfp32' if abi == '32' else '-mfp64']
        for endian in args.endian or ('be', 'le'):
            little = endian == 'le'
            byte_order = '-EL' if little else '-EB'
            target_arch = 'mips' if abi == '32' else 'mips64'
            triple = target_arch + ('el' if little else '') + '-linux-gnu'
            emulation = ('elf64' if abi == '64' else 'elf32') + ('ltsmip' if little else 'btsmip')
            if abi == 'n32':
                emulation += 'n32'
            common = [*machine, '-mhard-float',
                      '-mno-abicalls', '-fno-pic', '-G0', '-ffreestanding', '-fno-builtin',
                      '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables']
            for original_compiler in args.original_compiler or compilers:
                for original_opt in args.original_opt or ('-O0', '-O2'):
                    for case in cases:
                        for prefix in args.prefix if args.prefix is not None else (0, 7):
                            profile = f'{abi}-{endian}-{original_compiler}-{original_opt[1:]}-{case[0]}-{prefix}'
                            folder = artifacts.work / profile
                            folder.mkdir()
                            header, declaration, texts = sources(case, prefix)

                            def compile_source(compiler, text, stem, opt):
                                source, asm, obj = (folder / (stem + ext) for ext in ('.c', '.s', '.o'))
                                source.write_text(text)
                                artifacts.keep_source(source)
                                target = ['--target=' + triple, '-fno-addrsig'] if compiler == 'clang' else [byte_order]
                                command([compilers[compiler], *target, *common, opt, '-S', source, '-o', asm])
                                artifacts.keep_source(asm)
                                command([args.assembler, *machine, byte_order, asm, '-o', obj])
                                artifacts.discard(source, asm)
                                return obj

                            def link(objects, image):
                                command([args.ld, '-m', emulation, '-Ttext', '0x10000',
                                         '-e', 'record_direct', *objects, '-o', image])

                            caller_compiler = 'gcc' if original_compiler == 'clang' else 'clang'
                            objects = {name: compile_source(
                                original_compiler if name == 'record_callee' else caller_compiler,
                                header + text, name, original_opt) for name, text in texts.items()}
                            runtime = compile_source(original_compiler, MEMORY_RUNTIME.read_text(), 'memory', original_opt)
                            original = folder / 'original.elf'
                            link([*objects.values(), runtime], original)
                            original_hash = digest(original)
                            expected = {function: observe(original, little, abi, case, function, prefix)
                                        for function in ('record_direct', 'record_indirect')}
                            rows.append(dict(profile=profile, route='original', image_sha256=original_hash,
                                             observations=expected))
                            artifacts.write('observations.json', rows)
                            if args.neverd:
                                declaration_file = folder / 'declarations.h'
                                declaration_file.write_text(header)
                                artifacts.keep_source(declaration_file)
                                command([args.neverd, 'types', original, '--declare', declaration_file, '--save'])
                                artifacts.discard(declaration_file)
                                for function in args.function or texts:
                                    for route in args.route or ('highc', 'llvmc'):
                                        source = folder / 'reconstructed.c'
                                        if function == 'record_indirect':
                                            symbols = ELFFile(io.BytesIO(original.read_bytes())).get_section_by_name('.symtab')
                                            entry = symbols.get_symbol_by_name(function)[0]['st_value']
                                            source.write_text(views.source(original, 'c' if route == 'highc' else route,
                                                                          declaration, [], True, entry=entry))
                                        else:
                                            command([args.neverd, 'decompile', original, '--func', function,
                                                     *(['--llvm'] if route == 'llvmc' else []), '-o', source])
                                        artifacts.keep_source(source)
                                        source_text, source_hash = source.read_text(), digest(source)
                                        if function == 'record_callee':
                                            # A raw integer function can accidentally
                                            # reproduce this record's register bits.
                                            # Compile a source-level use too: record
                                            # arguments/results must retain their C
                                            # value types, not only compatible words.
                                            # A raw LLVMC carrier may omit the
                                            # otherwise-unused source typedef.
                                            # Supply that independent caller type
                                            # so its absence cannot mask a wrong
                                            # function parameter/result contract.
                                            if f'T_{case[0]}' not in source_text:
                                                source_text += f'\ntypedef {case[1]} T_{case[0]};\n'
                                            arguments = ['0' for _ in range(prefix)] + ['value', '0']
                                            source_text += (
                                                f'\nstatic T_{case[0]} __neverd_record_entry_contract(T_{case[0]} value) {{\n'
                                                f'  return record_callee({", ".join(arguments)});\n}}\n')
                                        for compiler in args.rebuild_compiler or compilers:
                                            for opt in args.rebuild_opt or ('-O0', '-O2'):
                                                obj = compile_source(compiler, source_text, 'rebuilt', opt)
                                                image = folder / 'rebuilt.elf'
                                                link([obj if name == function else original_obj
                                                      for name, original_obj in objects.items()] + [runtime], image)
                                                targets = ('record_direct', 'record_indirect') if function == 'record_callee' else (function,)
                                                observations = {target: observe(image, little, abi, case, target, prefix)
                                                                for target in targets}
                                                assert all(value == expected[target] for target, value in observations.items())
                                                rows.append(dict(profile=profile, route=route, function=function, compiler=compiler,
                                                                 optimization=opt, original_sha256=original_hash,
                                                                 source_sha256=source_hash, image_sha256=digest(image),
                                                                 observations=observations))
                                                artifacts.write('observations.json', rows)
                                                artifacts.discard(obj, image)
                                        artifacts.discard(source)
                            artifacts.discard(*objects.values(), runtime, original)
    originals = [row for row in rows if row['route'] == 'original']
    rebuilt = [row for row in rows if row['route'] != 'original']
    summary = dict(original_images=len(originals), rebuilt_images=len(rebuilt),
                   original_executions=sum(len(v) for row in originals for v in row['observations'].values()),
                   rebuilt_executions=sum(len(v) for row in rebuilt for v in row['observations'].values()),
                   driver_sha256=digest(Path(__file__)))
    artifacts.write('summary.json', summary)
    print(json.dumps(summary))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('clang', 'gcc', 'assembler', 'ld', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--neverd', type=Path)
    parser.add_argument('--sdk', type=Path)
    for name, choices in (
            ('abi', ('32', 'n32', '64')), ('endian', ('be', 'le')),
            ('original-compiler', ('clang', 'gcc')), ('rebuild-compiler', ('clang', 'gcc')),
            ('original-opt', ('-O0', '-O2')), ('rebuild-opt', ('-O0', '-O2')),
            ('route', ('highc', 'llvmc')),
            ('function', ('record_callee', 'record_direct', 'record_indirect')),
            ('case', tuple(case[0] for case in CASES))):
        parser.add_argument('--' + name, action='append', choices=choices)
    parser.add_argument('--prefix', action='append', type=int, choices=(0, 1, 7))
    args = parser.parse_args()
    if args.neverd and not args.sdk:
        args.sdk = args.neverd.resolve().with_name(
            'libneverd.dylib' if os.uname().sysname == 'Darwin' else 'libneverd.so')
    with MIPSValidationArtifacts(args.output, 'mips-record-abi-roundtrip',
                                 (args.clang, args.gcc, args.assembler, args.ld, MEMORY_RUNTIME,
                                  *([args.neverd, args.sdk] if args.neverd else []))) as artifacts:
        run(args, artifacts)


if __name__ == '__main__':
    main()
