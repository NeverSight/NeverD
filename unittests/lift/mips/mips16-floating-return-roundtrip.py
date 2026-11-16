#!/usr/bin/env python3
"""Replay MIPS16 scalar and complex FP returns against GNU runtime helpers.

Use an external, unmodified GCC libgcc/config/mips/mips16.S and its generated
include directory. GNU GCC and assembler create the originals. Reconstruct
caller and callee independently through HighC/LLVMC, then link each side and
both against the same runtime. Observe all result bits, the return mode,
FCSR, stack/link and preserved GPR/FPR state in an independent emulator.
Without --neverd, qualify compiler interworking using the owned original C.
With --floating-input, also traverse the classic hard-float entry stub.
"""

import argparse
import json
import random
from pathlib import Path

import roundtrip
from mips_validation_artifacts import MIPSValidationArtifacts, digest
from unicorn import UC_HOOK_CODE
from unicorn import mips_const as mips

def observe(image, little, fr, samples):
    uc, entry = roundtrip.emulator(image, 32, little, 'kernel',
                                   cpu_model=mips.UC_CPU_MIPS32_74KF)
    uc.reg_write(mips.UC_MIPS_REG_CP0_STATUS,
                 (uc.reg_read(mips.UC_MIPS_REG_CP0_STATUS) & ~0x04000000)
                 | (0x24000000 if fr else 0x20000000))
    order = 'little' if little else 'big'
    uc.mem_write(roundtrip.STOP, (0x24030051).to_bytes(4, order))
    returned = []
    uc.hook_add(UC_HOOK_CODE, lambda _u, pc, size, _: returned.append(size)
                if pc == roundtrip.STOP else None)
    saved_gprs = {r: 0xa5010000 + r for r in (*range(16, 24), 30)}
    saved_fprs = {r: (0x3ff0000000000000 if fr else 0x3f800000) + r
                  for r in range(20, 32, 2 if fr else 1)}
    results = []
    for words, mask in samples:
        returned.clear()
        for r in range(1, 32):
            uc.reg_write(getattr(mips, f'UC_MIPS_REG_{r}'), saved_gprs.get(r, 0))
        for r in range(32):
            uc.reg_write(getattr(mips, f'UC_MIPS_REG_F{r}'), saved_fprs.get(r, 0))
        uc.mem_write(roundtrip.DATA, b''.join(w.to_bytes(4, order) for w in words))
        uc.reg_write(mips.UC_MIPS_REG_A0, roundtrip.DATA)
        uc.reg_write(mips.UC_MIPS_REG_A1, mask)
        uc.reg_write(mips.UC_MIPS_REG_SP, roundtrip.STACK)
        uc.reg_write(mips.UC_MIPS_REG_RA, roundtrip.STOP)
        uc.reg_write(mips.UC_MIPS_REG_FCSR, 0)
        uc.emu_start(entry, roundtrip.STOP + 4, count=10000)
        expected = mask
        for i, word in enumerate(words):
            n = i * 7
            expected ^= ((word << n) | (word >> ((32 - n) % 32))) & 0xffffffff
        actual = uc.reg_read(mips.UC_MIPS_REG_V0) & 0xffffffff
        assert actual == expected, (image, words, hex(actual), hex(expected))
        assert returned == [4] and uc.reg_read(mips.UC_MIPS_REG_V1) == 0x51
        assert uc.reg_read(mips.UC_MIPS_REG_SP) == roundtrip.STACK
        assert uc.reg_read(mips.UC_MIPS_REG_RA) == roundtrip.STOP
        assert uc.reg_read(mips.UC_MIPS_REG_FCSR) == 0
        for r, value in saved_gprs.items():
            assert uc.reg_read(getattr(mips, f'UC_MIPS_REG_{r}')) == value
        for r, value in saved_fprs.items():
            assert uc.reg_read(getattr(mips, f'UC_MIPS_REG_F{r}')) == value
        results.append(actual)
    return results


def run(args, artifacts):
    def command(argv):
        roundtrip.run_command(artifacts, argv, artifacts.work / 'command.log')

    rows = []
    originals = []
    kinds = {'sf': ('float', 1), 'df': ('double', 2),
             'sc': ('_Complex float', 2), 'dc': ('_Complex double', 4)}
    compilers = {'clang': args.clang, 'gcc': args.gcc}
    rng = random.Random(0x4d313652)
    for kind in args.kind or (('sf', 'df') if args.floating_input else kinds):
        ctype, count = kinds[kind]
        patterns = [0, 1, 0x80000000, 0x7f800000, 0xff800000, 0x7fa12345,
                    0xffc54321, 0x7ff12345, 0xffffffff]
        samples = [([patterns[(j + i) % len(patterns)] for i in range(count)],
                    0x71395217) for j in range(len(patterns))]
        samples += [([rng.getrandbits(32) for _ in range(count)], rng.getrandbits(32))
                    for _ in range(23)]
        parameter = f'{ctype} value' if args.floating_input else 'const u32 *input'
        prelude = ('typedef unsigned int u32;\n'
                   '#define M16 __attribute__((mips16,noinline))\n'
                   f'union payload {{ {ctype} value; u32 words[{count}]; }};\n'
                   f'M16 {ctype} fp_callee({parameter});\n')
        callee_text = prelude + f'M16 {ctype} fp_callee({parameter}) {{\n'
        if args.floating_input:
            callee_text += 'return value;\n}\n'
        else:
            callee_text += 'union payload p;\n' + ''.join(
                f'p.words[{i}] = input[{i}];\n' for i in range(count))
            callee_text += 'return p.value;\n}\n'
        caller_text = prelude + 'u32 kernel(const u32 *input, u32 mask) {\n'
        caller_text += 'union payload p;\n'
        if args.floating_input:
            caller_text += ''.join(f'p.words[{i}] = input[{i}];\n'
                                   for i in range(count))
            caller_text += 'p.value = fp_callee(p.value);\n'
        else:
            caller_text += 'p.value = fp_callee(input);\n'
        caller_text += 'u32 out = mask;\n'
        for i in range(count):
            caller_text += (f'out ^= p.words[{i}];\n' if i == 0 else
                            f'out ^= (p.words[{i}] << {i*7}) | (p.words[{i}] >> {32-i*7});\n')
        caller_text += 'return out;\n}\n'
        for endian_name in args.endian or ('be', 'le'):
            little = endian_name == 'le'
            for fr in args.fr or (0, 1):
                for mode in args.mode or ('classic', 'mips16'):
                    profile = f'{kind}-{endian_name}-fr{fr}-{mode}'
                    directory = artifacts.work / profile
                    directory.mkdir()
                    endian = '-EL' if little else '-EB'
                    fp = '-mfp64' if fr else '-mfp32'
                    common = ['-march=mips32r2', '-mabi=32', fp, '-mhard-float',
                              '-mno-abicalls', '-fno-pic', '-G0', '-ffreestanding',
                              '-fno-stack-protector', '-fno-unwind-tables',
                              '-fno-asynchronous-unwind-tables']

                    def build(compiler, source, obj, instruction_mode, optimization):
                        assembly = obj.with_suffix('.s')
                        target = (['--target=mipsel-linux-gnu' if little else
                                   '--target=mips-linux-gnu', '-fno-addrsig']
                                  if compiler == args.clang else
                                  [endian, '-minterlink-compressed'])
                        command([compiler, *target, *common,
                                 *(['-mips16'] if instruction_mode == 'mips16' else []),
                                 optimization, '-S', source, '-o', assembly])
                        artifacts.keep_source(assembly)
                        command([args.assembler, '-march=mips32r2', '-mabi=32', fp,
                                 endian, assembly, '-o', obj])
                        artifacts.discard(assembly)

                    caller, callee = directory / 'caller.c', directory / 'callee.c'
                    caller.write_text(caller_text)
                    callee.write_text(callee_text)
                    artifacts.keep_source(caller)
                    artifacts.keep_source(callee)
                    runtime_asm = directory / 'runtime.s'
                    runtime_obj = directory / 'runtime.o'
                    command([args.gcc, endian, *common, '-DL_m16ret' + kind,
                             '-E', '-P', '-x', 'assembler-with-cpp', args.runtime_source,
                             '-o', runtime_asm])
                    artifacts.keep_source(runtime_asm)
                    command([args.assembler, '-march=mips32r2', '-mabi=32', fp,
                             endian, runtime_asm, '-o', runtime_obj])
                    artifacts.discard(runtime_asm)
                    cobj, fobj = directory / 'caller.o', directory / 'callee.o'
                    build(args.gcc, caller, cobj, mode, '-O2')
                    build(args.gcc, callee, fobj, 'mips16', '-O2')
                    original = directory / 'original.elf'

                    def link(objects, image):
                        command([args.ld, '-m', 'elf32ltsmip' if little else 'elf32btsmip',
                                 '-Ttext', '0x10000', '-e', 'kernel', *objects,
                                 runtime_obj, '-o', image])

                    link([cobj, fobj], original)
                    expected = observe(original, little, fr, samples)
                    original_hash = digest(original)
                    originals.append({'profile': profile, 'sha256': original_hash,
                                      'cases': len(expected)})
                    artifacts.write('originals.json', originals)
                    routes = ('highc', 'llvmc') if args.neverd else ('original-source',)
                    if args.neverd:
                        declaration = directory / 'declaration.h'
                        declared_parameter = (parameter if args.floating_input
                                              else 'const unsigned int *input')
                        declaration.write_text(f'{ctype} fp_callee({declared_parameter});\n')
                        artifacts.keep_source(declaration)
                        command([args.neverd, 'types', original, '--declare', declaration, '--save'])
                        artifacts.discard(declaration)
                    for route in routes:
                        sources = {'caller': caller, 'callee': callee}
                        if args.neverd:
                            for which, function in [('caller', 'kernel'), ('callee', 'fp_callee')]:
                                sources[which] = directory / f'{which}-{route}.c'
                                command([args.neverd, 'decompile', original, '--func', function,
                                         *(['--llvm'] if route == 'llvmc' else []),
                                         '-o', sources[which]])
                                artifacts.keep_source(sources[which])
                        for compiler_name in args.rebuild_compiler or ('clang', 'gcc'):
                            compiler = compilers[compiler_name]
                            for which in ('caller', 'callee', 'both'):
                                for opt in ('-O0', '-O2'):
                                    newc, newf = directory / 'newcaller.o', directory / 'newcallee.o'
                                    if which in ('caller', 'both'):
                                        build(compiler, sources['caller'], newc,
                                              'classic' if args.neverd else mode, opt)
                                    if which in ('callee', 'both'):
                                        build(compiler, sources['callee'], newf, 'classic', opt)
                                    image = directory / 'rebuilt.elf'
                                    link([newc if which != 'callee' else cobj,
                                          newf if which != 'caller' else fobj], image)
                                    assert observe(image, little, fr, samples) == expected
                                    rows.append({'profile': profile, 'changed': which,
                                                 'route': route, 'compiler': compiler_name,
                                                 'optimization': opt, 'cases': len(expected),
                                                 'original_sha256': original_hash,
                                                 'caller_source_sha256': digest(sources['caller']),
                                                 'callee_source_sha256': digest(sources['callee']),
                                                 'rebuilt_sha256': digest(image)})
                                    artifacts.write('results.json', rows)
                                    artifacts.discard(image)
                                    for obj in (newc, newf):
                                        if obj.exists():
                                            artifacts.discard(obj)
                        if args.neverd:
                            artifacts.discard(*sources.values())
                    artifacts.discard(caller, callee, cobj, fobj, runtime_obj, original)
    summary = {'builds': len(rows), 'executions': sum(r['cases'] for r in rows),
               'profiles': len(originals), 'original_cases': sum(r['cases'] for r in originals),
               'runtime_source_sha256': digest(args.runtime_source),
               'floating_input': args.floating_input,
               'neverd': str(args.neverd) if args.neverd else None}
    artifacts.write('summary.json', summary)
    print(json.dumps(summary))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('gcc', 'clang', 'assembler', 'ld', 'runtime-source', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--neverd', type=Path)
    parser.add_argument('--floating-input', action='store_true')
    parser.add_argument('--kind', action='append', choices=('sf', 'df', 'sc', 'dc'))
    parser.add_argument('--endian', action='append', choices=('be', 'le'))
    parser.add_argument('--mode', action='append', choices=('classic', 'mips16'))
    parser.add_argument('--fr', action='append', type=int, choices=(0, 1))
    parser.add_argument('--rebuild-compiler', action='append', choices=('clang', 'gcc'))
    args = parser.parse_args()
    if args.floating_input and any(k not in ('sf', 'df') for k in args.kind or ()):
        parser.error('--floating-input currently requires scalar sf/df results')
    with MIPSValidationArtifacts(args.output, 'mips16-floating-return-roundtrip',
                                 (args.gcc, args.clang, args.assembler, args.ld,
                                  args.runtime_source, *([args.neverd] if args.neverd else []))) as artifacts:
        run(args, artifacts)

