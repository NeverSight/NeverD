#!/usr/bin/env python3
"""Audit compact-memory exports using an independent decoder and C recorder.

Run NeverDX86CompressExpandSourceTests with NEVERD_COMPACT_EXPORT_DIR first.
Native objects retain instruction kind, width, masking, segment and repetition.
The recorder executes source-level operand/return transport without AVX-512.
It does not qualify physical fault behavior or execution on an AVX-512 CPU.
Requires capstone and lief. NeverC's frontend matrix is x86-64 only.
"""
import argparse
import hashlib
import itertools
import json
import re
import subprocess
from pathlib import Path

import capstone
import lief

from verify_x86_memory_address import memory_address

SUFFIXES = ('b', 'w', 'd', 'q', 'ps', 'pd')
ELEMENTS = (1, 2, 4, 8, 4, 8)
TRIPLES = ('x86_64-unknown-linux-gnu', 'x86_64-pc-windows-msvc', 'x86_64-apple-darwin')


def cases(bits=64):
    prefixes = (0, 0x64, 0x65, 0x100) if bits == 64 else (0, 0x64, 0x65, 0x26, 0x2e, 0x36)
    return list(itertools.product((16, 32, 64), range(6), prefixes, range(3)))


def run(command):
    r = subprocess.run(list(map(str, command)), capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f'{command}\n{r.stdout}\n{r.stderr}')
    return r.stdout


def decode(code, mode):
    cs = capstone.Cs(capstone.CS_ARCH_X86, mode)
    result, at = [], 0
    while at < len(code):
        start, prefix, address_override = at, 0, False
        while at < len(code) and code[at] in (0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65, 0x67):
            if code[at] == 0x67:
                address_override = True
            else:
                prefix = code[at]
            at += 1
        if at + 6 <= len(code) and code[at] == 0x62:
            p0, p1, p2, op, modrm = code[at + 1:at + 6]
            # Register forms can appear independently when the compiler assembles
            # the returned wide value. Count only memory forms; audit() still
            # requires the exact native-memory instruction and repetition count.
            if ((p0 & 7) == 2 and (p1 & 3) == 1 and modrm >> 6 != 3
                    and op in (0x62, 0x63, 0x88, 0x89, 0x8a, 0x8b)):
                assert not address_override, (start, 'native pointer width changed')
                end, stack, _, _, _ = memory_address(
                    code, at + 6, p0, modrm, 64 if mode == capstone.CS_MODE_64 else 32)
                assert (p1 & 0x78) == 0x78 and (p2 & 0x18) == 8
                assert p2 & 7, 'native mask was replaced with k0'
                ll = p2 >> 5 & 3
                assert ll != 3
                kind = ((p1 >> 7) & 1) + (0 if op in (0x62, 0x63) else 4 if op in (0x88, 0x8a) else 2)
                expand = op in (0x62, 0x88, 0x89)
                zero = bool(p2 & 0x80)
                assert expand or not zero
                result.append((16 << ll, kind, prefix | (0x100 if stack else 0), 2 if zero else 1 if expand else 0))
                at = end
                continue
        insn = next(cs.disasm(code[start:], start, count=1), None)
        assert insn, (start, code[start:start + 16].hex())
        at = start + insn.size
        if insn.mnemonic == 'ret':
            break
    return result


def audit(path, matrix, bits=64, pipeline=False):
    binary = lief.parse(str(path))
    sections = [s for s in binary.sections if s.name in ('.text', '__text') and s.content]
    assert len(sections) == 1, (path, [s.name for s in sections])
    section = sections[0]
    code = bytes(section.content)
    symbols = sorted((s.value - getattr(section, 'virtual_address', 0), s.name.lstrip('_'))
                     for s in binary.symbols if s.name.lstrip('_').startswith('compact_'))
    assert len(symbols) == len(matrix), (path, len(symbols), len(matrix))
    for i, (start, name) in enumerate(symbols):
        end = symbols[i + 1][0] if i + 1 < len(symbols) else len(code)
        index = 0 if pipeline else int(name.removeprefix('compact_'))
        try:
            decoded = decode(code[start:end], capstone.CS_MODE_64 if bits == 64 else capstone.CS_MODE_32)
        except AssertionError as error:
            raise AssertionError((path, name, error.args)) from error
        assert decoded == [matrix[index]] * (1 if pipeline else 2), (path, name, decoded, matrix[index])
    return {'file': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
            'functions': len(symbols), 'native_instructions': len(symbols) * (1 if pipeline else 2)}


def recorder(source, *, mask_bits=64, calls_expected=2):
    # Erase only ISA attributes, then substitute instrumented interfaces. The
    # actual generated expression, types, byte copies and uses stay unchanged.
    source = re.sub(r'__attribute__\(\(target\("[^"\n]*"\)\)\)', '', source)
    head = '#include <stdint.h>\n#include <string.h>\n#include <stdlib.h>\n#define __NEVERC_X86_COMPRESS_EXPAND__ 1\n#define __NEVERC_X86_NATIVE_MEMORY_STACK_REFERENCE__ 1\n'
    head += f'#define EXPECTED_CALLS {calls_expected}\n'
    head += '''static unsigned expected_bytes, expected_kind, expected_prefix, expected_mode, calls;
static uint64_t expected_mask;
static unsigned char memory[80]; static uintptr_t expected_address;
static void check(unsigned bytes, unsigned kind, unsigned prefix, unsigned mode, void *p, uint64_t mask) {
 if (bytes != expected_bytes || kind != expected_kind || prefix != expected_prefix || mode != expected_mode || (mask && (uintptr_t)p != expected_address) || calls >= EXPECTED_CALLS || mask != (calls ? 0 : expected_mask)) abort();
 ++calls;
}
'''
    for size, kind, expand in itertools.product((16, 32, 64), range(6), (False, True)):
        op = ('vp' if kind < 4 else 'v') + ('expand' if expand else 'compress') + SUFFIXES[kind]
        name = op + str(size * 8)
        head += f'typedef unsigned int T_{name} __attribute__((vector_size({size})));\n'
        head += f'#define __builtin_ia32_{name}_native record_{name}\n'
        ret = f'T_{name}' if expand else 'void'
        head += f'static __attribute__((noinline)) {ret} record_{name}(void *p, unsigned long long mask, T_{name} v, '
        if expand:
            head += 'unsigned zero, '
        head += 'unsigned prefix) {\n'
        lane_mask = (1 << (size // ELEMENTS[kind])) - 1
        head += f'check({size},{kind},prefix,' + ('zero ? 2 : 1' if expand else '0') + f',p,mask & UINT64_C({lane_mask}));\n'
        head += f'unsigned char raw[{size}], result[{size}]; memcpy(raw,&v,{size}); unsigned cursor=0;\n'
        element = ELEMENTS[kind]
        if expand:
            head += f'if (zero) memset(result,0,{size}); else memcpy(result,raw,{size});\n'
        head += f'for (unsigned lane=0; lane<{size//element}; ++lane) if ((mask>>lane)&1) {{\n'
        if expand:
            head += f'memcpy(result+lane*{element},memory+cursor,{element});\n'
        else:
            head += f'memcpy(memory+cursor,raw+lane*{element},{element});\n'
        head += f'cursor+={element}; }}\n'
        if expand:
            head += f'memcpy(&v,result,{size}); return v;\n'
        head += '}\n'
    tail = '\n'
    for index, (size, kind, prefix, mode) in enumerate(cases()):
        # Keep each case independently optimizable. A single enormous main
        # makes compiler loop/alias analysis dominate the qualification run.
        tail += f'static __attribute__((noinline)) void check_case_{index}(void) {{\n'
        element = ELEMENTS[kind]
        lanes = size // element
        masks = sorted({0, (1 << 64) - 1, 1, 1 << (lanes - 1), 1 | (1 << (lanes - 1)),
                        0xabcd1357, 0x80000000, 0xffff0000,
                        0xaaaaaaaaaaaaaaaa, 0x5555555555555555, ((1 << 64) - 1) ^ ((1 << lanes) - 1)})
        for mask in masks:
            effective_mask = mask & ((1 << mask_bits) - 1)
            tail += '{ unsigned char input[64], output[80], expected[80], before[80];\n'
            tail += f'expected_bytes={size}; expected_kind={kind}; expected_prefix={prefix}; expected_mode={mode}; calls=0; expected_address=(uintptr_t)0x12345678u;\n'
            tail += f'expected_mask=UINT64_C({effective_mask & ((1 << lanes) - 1)});\n'
            tail += 'for(unsigned i=0;i<64;++i) input[i]=(unsigned char)(i*37+19);\n'
            tail += 'for(unsigned i=0;i<80;++i) before[i]=memory[i]=(unsigned char)(i*13+7);\n'
            tail += 'memset(output,0xa5,80); memcpy(expected,output,80);\n'
            if mode:
                tail += f'memcpy(expected,input,{size});\n' if mode == 1 else f'memset(expected,0,{size});\n'
            else:
                tail += 'memcpy(expected,before,80);\n'
            # Build expected bytes from an independently enumerated active-lane list.
            active = [i for i in range(lanes) if (effective_mask >> i) & 1]
            for packed, lane in enumerate(active):
                tail += (f'memcpy(expected+{lane*element},before+{packed*element},{element});\n' if mode else
                         f'memcpy(expected+{packed*element},input+{lane*element},{element});\n')
            tail += f'compact_{index}((void*)expected_address,UINT64_C({mask}),input,output);\n'
            tail += 'if(calls!=EXPECTED_CALLS) abort();\n'
            tail += ('if(memcmp(output,expected,80) || memcmp(memory,before,80)) abort();\n' if mode else
                     'if(memcmp(memory,expected,80)) abort();\n')
            tail += '}\n'
        tail += '}\n'
    tail += 'int main(void) {\n'
    tail += ''.join(f'check_case_{index}();\n' for index in range(len(cases())))
    return head + source + tail + 'return 0; }\n'


def pipeline_recorder(exports, route):
    sources = []
    for index, (_, _, _, mode) in enumerate(cases()):
        source = (exports / 'pipeline' / f'{index}-{route}.c').read_text()
        signature = re.search(r'\bcompact_pipeline\(([^\n]*)\) \{', source)
        assert signature, (index, route, 'missing pipeline function')
        params = signature.group(1).split(', ')
        assert len(params) == (4 if mode else 3), (index, route, params)
        address_type = params[0].removesuffix(' arg0')
        assert address_type in ('void*', 'int64_t', 'uint64_t'), address_type
        # Call the recovered signature normally, so the C compiler applies
        # its declared parameter conversions. No incompatible ABI casts.
        source = source.replace('compact_pipeline', f'pipeline_{index}')
        source += (f'\nvoid compact_{index}(void *address, uint64_t mask, void *input, void *output) {{ '
                   f'(void)pipeline_{index}(({address_type})(uintptr_t)address,(int32_t)mask,input'
                   + (',output' if mode else '') + '); }\n')
        sources.append(source)
    # Each raw fixture first executes KMOVW k2,esi and performs one compact
    # memory instruction. Its result is stored through the fourth argument.
    return recorder('\n'.join(sources), mask_bits=16, calls_expected=1)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--exports', required=True, type=Path)
    p.add_argument('--keep', required=True, type=Path)
    p.add_argument('--llc', required=True, type=Path)
    p.add_argument('--neverc', required=True, type=Path)
    p.add_argument('--clang', default='clang')
    p.add_argument('--pipeline', action='store_true')
    args = p.parse_args()
    args.keep.mkdir(parents=True, exist_ok=True)
    reports, recorders = [], []
    for arch, bits in (('x64', 64), ('x86', 32)):
        for obj in args.exports.glob(arch + '-*.o'):
            reports.append(audit(obj, cases(bits), bits))
        for opt, triple in itertools.product(('O0', 'O2'), TRIPLES):
            target = triple if bits == 64 else triple.replace('x86_64', 'i386')
            ir = args.exports / (arch + '.ll')
            selected = ir
            if opt == 'O2':
                selected = args.keep / f'{arch}-{opt}.ll'
                run([args.llc.parent / 'opt', '-passes=default<O2>', '-S', ir, '-o', selected])
            obj = args.keep / f'{arch}-{opt}-{target}.o'
            run([args.llc, '-mtriple=' + target, '-' + opt, '-verify-machineinstrs', '-filetype=obj', selected, '-o', obj])
            reports.append(audit(obj, cases(bits), bits))
            for route, compiler, dialect in itertools.product(('high', 'llvm'), (args.clang, args.neverc), ('att', 'intel')):
                if compiler == args.neverc and bits == 32:
                    continue
                output = args.keep / f'{arch}-{route}-{Path(compiler).name}-{opt}-{target}-{dialect}.o'
                cmd = [compiler, '--target=' + target, '-' + opt, '-ffreestanding', '-fno-stack-protector', '-fno-lto',
                       '-fno-function-sections', '-masm=' + dialect, '-c', args.exports / f'{arch}-{route}.c', '-o', output]
                if compiler == args.neverc:
                    cmd += ['-fno-builtin-mimalloc', '-mllvm', '-verify-machineinstrs']
                run(cmd)
                reports.append(audit(output, cases(bits), bits))
            print({'arch': arch, 'optimization': opt, 'triple': target, 'objects': len(reports)}, flush=True)
    for route in ('high', 'llvm'):
        source = args.keep / f'{route}-recorder.c'
        source.write_text(recorder((args.exports / f'x64-{route}.c').read_text()))
        for compiler, opt in itertools.product((args.clang, args.neverc), ('O0', 'O2')):
            exe = args.keep / f'{route}-recorder-{Path(compiler).name}-{opt}'
            cmd = [compiler, '-' + opt, '-march=x86-64', '-Wno-psabi', '-fno-lto', source, '-o', exe]
            if compiler == args.neverc:
                cmd += ['-fno-builtin-mimalloc']
            run(cmd)
            run([exe])
            recorders.append({'source': str(source), 'compiler': str(compiler), 'optimization': opt})
            print({'recorder': route, 'compiler': str(compiler), 'optimization': opt}, flush=True)
    if args.pipeline:
        for route in ('high', 'llvm'):
            source = args.keep / f'pipeline-{route}-recorder.c'
            source.write_text(pipeline_recorder(args.exports, route))
            for compiler, opt in itertools.product((args.clang, args.neverc), ('O0', 'O2')):
                exe = args.keep / f'pipeline-{route}-recorder-{Path(compiler).name}-{opt}'
                cmd = [compiler, '-' + opt, '-march=x86-64', '-Wno-psabi', '-fno-lto', source, '-o', exe]
                if compiler == args.neverc:
                    cmd += ['-fno-builtin-mimalloc']
                run(cmd)
                run([exe])
                recorders.append({'source': str(source), 'compiler': str(compiler), 'optimization': opt})
                print({'pipeline_recorder': route, 'compiler': str(compiler), 'optimization': opt}, flush=True)
        matrix = cases()
        for i, case in enumerate(matrix):
            obj = args.exports / 'pipeline' / f'{i}.o'
            reports.append(audit(obj, [case], pipeline=True))
            for route, compiler, opt in itertools.product(('high', 'llvm'), (args.clang, args.neverc), ('O0', 'O2')):
                source = args.exports / 'pipeline' / f'{i}-{route}.c'
                output = args.keep / f'pipeline-{i}-{route}-{Path(compiler).name}-{opt}.o'
                cmd = [compiler, '--target=' + TRIPLES[0], '-' + opt, '-ffreestanding', '-fno-lto', '-fno-function-sections', '-c', source, '-o', output]
                if compiler == args.neverc:
                    cmd += ['-fno-builtin-mimalloc', '-mllvm', '-verify-machineinstrs']
                run(cmd)
                reports.append(audit(output, [case], pipeline=True))
            if (i + 1) % 18 == 0:
                print({'pipeline_cases': i + 1, 'objects': len(reports)}, flush=True)
    report = {'objects': reports, 'source_recorders': recorders,
              'physical_avx512_execution_qualified': False, 'hardware_fault_behavior_qualified': False}
    (args.keep / 'qualification.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
