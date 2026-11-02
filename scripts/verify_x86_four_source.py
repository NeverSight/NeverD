#!/usr/bin/env python3
"""Independently audit native four-source memory exports and C value transport.

Export NeverDX86FourSourceSourceTests with NEVERD_FOUR_SOURCE_EXPORT_DIR first.
Requires capstone and lief. The recorder substitutes a distinguishable integer
operation for native arithmetic; it qualifies operand and result transport,
not physical Xeon Phi execution, FP exception delivery or hardware faults.
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

NAMES = ('v4fmaddps', 'v4fnmaddps', 'v4fmaddss', 'v4fnmaddss', 'vp4dpwssd', 'vp4dpwssds')
OPCODES = (0x9a, 0xaa, 0x9b, 0xab, 0x52, 0x53)
TRIPLES = ('x86_64-unknown-linux-gnu', 'x86_64-pc-windows-msvc', 'x86_64-apple-darwin')


def cases(bits=64):
    prefixes = (0, 0x64, 0x65, 0x100) if bits == 64 else (0, 0x64, 0x65, 0x26, 0x2e, 0x36)
    return list(itertools.product(range(6), prefixes, range(2)))


def run(command):
    r = subprocess.run(list(map(str, command)), capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f'{command}\n{r.stdout}\n{r.stderr}')
    return r.stdout


def decode(code, bits):
    cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64 if bits == 64 else capstone.CS_MODE_32)
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
            p0, p1, p2, opcode, modrm = code[at + 1:at + 6]
            if (p0 & 7) == 2 and (p1 & 3) == 3 and opcode in OPCODES:
                assert not address_override, (start, 'native pointer width changed')
                assert p1 & 0x84 == 4 and not p2 & 0x10, (start, 'reserved encoding')
                source = ((~p1 >> 3) & 15) | ((~p2 & 8) << 1)
                assert source == 4, (start, 'unaligned or wrong source block', source)
                end, stack, _, _, _ = memory_address(code, at + 6, p0, modrm, bits)
                assert p2 & 7, (start, 'native mask replaced by k0')
                kind = OPCODES.index(opcode)
                assert (p2 >> 5 & 3) == (0 if kind in (2, 3) else 2)
                result.append((kind, prefix | (0x100 if stack else 0), int(bool(p2 & 0x80))))
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
                     for s in binary.symbols if s.name.lstrip('_').startswith('four_'))
    assert len(symbols) == len(matrix), (path, len(symbols), len(matrix))
    for i, (start, name) in enumerate(symbols):
        end = symbols[i + 1][0] if i + 1 < len(symbols) else len(code)
        index = 0 if pipeline else int(name.removeprefix('four_'))
        try:
            decoded = decode(code[start:end], bits)
        except AssertionError as error:
            raise AssertionError((path, name, error.args)) from error
        assert decoded == [matrix[index]] * (1 if pipeline else 2), (path, name, decoded, matrix[index])
    return {'file': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
            'functions': len(symbols), 'native_instructions': len(symbols) * (1 if pipeline else 2)}


def recorder(source, calls_expected=2):
    source = re.sub(r'__attribute__\(\(target\("[^"\n]*"\)\)\)', '', source)
    head = '''#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define __NEVERC_X86_FOUR_SOURCE__ 1
#define __NEVERC_X86_NATIVE_MEMORY_STACK_REFERENCE__ 1
static unsigned kind_expected, prefix_expected, zero_expected, calls;
static uint16_t mask_expected;
static unsigned char input[320];
static uint32_t word(const unsigned char *p) { uint32_t x; memcpy(&x,p,4); return x; }
'''
    head += f'#define EXPECTED_CALLS {calls_expected}\n'
    for kind, name in enumerate(NAMES):
        scalar = kind in (2, 3)
        size = 16 if scalar else 64
        lanes = 1 if scalar else 16
        all_lanes = (1 << lanes) - 1
        head += f'typedef unsigned int V{kind} __attribute__((vector_size({size})));\n'
        head += f'#define __builtin_ia32_{name}_native record_{name}\n'
        head += f'static __attribute__((noinline)) V{kind} record_{name}(const void *p,V{kind} a,V{kind} b,V{kind} c,V{kind} d,V{kind} e,unsigned short mask,unsigned zero,unsigned prefix) {{\n'
        head += f'mask &= {all_lanes};\n'
        head += f'if(kind_expected!={kind} || prefix!=prefix_expected || zero!=zero_expected || calls>=EXPECTED_CALLS || mask!=(calls ? 0 : mask_expected) || (mask && (uintptr_t)p!=0x12345678u)) abort(); ++calls;\n'
        head += f'unsigned char old[{size}],s[4][{size}],out[{size}]; memcpy(old,&a,{size}); memcpy(s[0],&b,{size}); memcpy(s[1],&c,{size}); memcpy(s[2],&d,{size}); memcpy(s[3],&e,{size});\n'
        head += f'if((mask || !zero || {int(scalar)}) && memcmp(old,input,{size})) abort();\n'
        head += f'if(mask) for(unsigned i=0;i<4;++i) if(memcmp(s[i],input+(i+1)*{size},{4 if scalar else size})) abort();\n'
        head += f'memcpy(out,old,{size}); for(unsigned lane=0;lane<{lanes};++lane) {{ uint32_t x=0;\n'
        head += f'if((mask>>lane)&1) {{ x=word(old+4*lane) ^ (word(s[0]+4*lane)+3u*word(s[1]+4*lane)+5u*word(s[2]+4*lane)+7u*word(s[3]+4*lane)+{0x10203040+kind}u+lane); memcpy(out+4*lane,&x,4); }} else if(zero) memcpy(out+4*lane,&x,4); }}\n'
        head += f'memcpy(&a,out,{size}); return a; }}\n'
    tail = '\n'
    for index, (kind, prefix, zero) in enumerate(cases()):
        scalar = kind in (2, 3)
        size, lanes = (16, 1) if scalar else (64, 16)
        masks = (0, 1, 0xffff, 0x8000, 0xaaaa, 0x5555, 0xffff0000, 0xabcd1357)
        tail += f'static __attribute__((noinline)) void check_{index}(void) {{\n'
        for mask in masks:
            active = mask & ((1 << lanes) - 1)
            raw = bytes((i * 37 + 19) & 255 for i in range(320))
            expected = bytearray(raw[:size]) + bytearray([0xa5] * (80 - size))
            for lane in range(lanes):
                if (active >> lane) & 1:
                    old = int.from_bytes(raw[4*lane:4*lane+4], 'little')
                    values = [int.from_bytes(raw[(i+1)*size+4*lane:(i+1)*size+4*lane+4], 'little') for i in range(4)]
                    result = old ^ ((sum(v*w for v, w in zip(values, (1, 3, 5, 7))) + 0x10203040 + kind + lane) & 0xffffffff)
                    expected[4*lane:4*lane+4] = result.to_bytes(4, 'little')
                elif zero:
                    expected[4*lane:4*lane+4] = bytes(4)
            tail += '{ unsigned char output[80]; static const unsigned char expected[80]={' + ','.join(map(str, expected)) + '};\n'
            tail += f'kind_expected={kind}; prefix_expected={prefix}; zero_expected={zero}; mask_expected={active}; calls=0;\n'
            tail += 'for(unsigned i=0;i<320;++i) input[i]=(unsigned char)(i*37+19); memset(output,0xa5,80);\n'
            tail += f'four_{index}((void*)(uintptr_t)0x12345678u,(uint16_t)UINT64_C({mask}),input,output);\n'
            tail += 'if(calls!=EXPECTED_CALLS || memcmp(output,expected,80)) abort(); }\n'
        tail += '}\n'
    tail += 'int main(void) {\n' + ''.join(f'check_{i}();\n' for i in range(len(cases()))) + 'return 0; }\n'
    return head + source + tail


def pipeline_recorder(exports, route):
    sources = []
    for index, _ in enumerate(cases()):
        source = (exports / 'pipeline' / f'{index}-{route}.c').read_text()
        signature = re.search(r'\bfour_pipeline\(([^\n]*)\) \{', source)
        assert signature, (index, route, 'missing pipeline function')
        params = signature.group(1).split(', ')
        assert len(params) == 4, (index, route, params)
        address_type = params[0].removesuffix(' arg0')
        assert address_type in ('void*', 'int64_t', 'uint64_t'), address_type
        source = source.replace('four_pipeline', f'pipeline_{index}')
        source += f'\nvoid four_{index}(void *address,uint16_t mask,void *input,void *output) {{ (void)pipeline_{index}(({address_type})(uintptr_t)address,(int32_t)mask,input,output); }}\n'
        sources.append(source)
    return recorder('\n'.join(sources), calls_expected=1)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('exports', 'keep', 'llc', 'neverc'):
        p.add_argument('--' + name, required=True, type=Path)
    p.add_argument('--clang', default='clang')
    args = p.parse_args()
    args.keep.mkdir(parents=True, exist_ok=True)
    reports, recorders = [], []
    for arch, bits in (('x64', 64), ('x86', 32)):
        for obj in args.exports.glob(arch + '-*.o'):
            reports.append(audit(obj, cases(bits), bits))
        for opt, triple in itertools.product(('O0', 'O2'), TRIPLES):
            target = triple if bits == 64 else triple.replace('x86_64', 'i386')
            selected = args.exports / (arch + '.ll')
            if opt == 'O2':
                selected = args.keep / f'{arch}-{opt}.ll'
                run([args.llc.parent / 'opt', '-passes=default<O2>', '-S', args.exports / (arch + '.ll'), '-o', selected])
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
    for pipeline, route in itertools.product((False, True), ('high', 'llvm')):
        source = args.keep / f'{"pipeline" if pipeline else "wrapper"}-{route}-recorder.c'
        source.write_text(pipeline_recorder(args.exports, route) if pipeline else recorder((args.exports / f'x64-{route}.c').read_text()))
        for compiler, opt in itertools.product((args.clang, args.neverc), ('O0', 'O2')):
            exe = source.with_suffix('.' + Path(compiler).name + '-' + opt)
            cmd = [compiler, '-' + opt, '-march=x86-64', '-Wno-psabi', '-fno-lto', source, '-o', exe]
            if compiler == args.neverc:
                cmd += ['-fno-builtin-mimalloc']
            else:
                cmd += ['-fsanitize=undefined', '-fno-sanitize-recover=undefined']
            run(cmd)
            run([exe])
            recorders.append({'source': str(source), 'compiler': str(compiler), 'optimization': opt,
                              'undefined_behavior_sanitizer': compiler != args.neverc})
            print({'recorder': str(source), 'compiler': str(compiler), 'optimization': opt}, flush=True)
    for i, case in enumerate(cases()):
        reports.append(audit(args.exports / 'pipeline' / f'{i}.o', [case], pipeline=True))
        for route, compiler, opt in itertools.product(('high', 'llvm'), (args.clang, args.neverc), ('O0', 'O2')):
            output = args.keep / f'pipeline-{i}-{route}-{Path(compiler).name}-{opt}.o'
            cmd = [compiler, '--target=' + TRIPLES[0], '-' + opt, '-ffreestanding', '-fno-lto', '-fno-function-sections',
                   '-c', args.exports / 'pipeline' / f'{i}-{route}.c', '-o', output]
            if compiler == args.neverc:
                cmd += ['-fno-builtin-mimalloc', '-mllvm', '-verify-machineinstrs']
            run(cmd)
            reports.append(audit(output, [case], pipeline=True))
        if (i + 1) % 6 == 0:
            print({'pipeline_cases': i + 1, 'objects': len(reports)}, flush=True)
    report = {'objects': reports, 'source_recorders': recorders,
              'physical_four_source_execution_qualified': False, 'hardware_fault_behavior_qualified': False}
    (args.keep / 'qualification.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
