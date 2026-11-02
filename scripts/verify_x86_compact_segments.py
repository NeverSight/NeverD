#!/usr/bin/env python3
"""Check operand transport in exported i386 CS/ES/SS compact-memory C.

Run NeverDX86CompactSegmentTests with NEVERD_ISA_REGRESSION_ARTIFACT_DIR first.
The recorders replace only the compact operation and remove target attributes;
they execute the generated integer/vector plumbing on x64. Clang runs with ASan
and UBSan. NeverC checks the same source transport with its x64 frontend. These
are not i386 processor, AVX-512 execution, or segment-descriptor fault results.
"""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def recorder(source, case):
    mode = (case // 2) % 3
    prefix = (0x26, 0x2e, 0x36)[(case // 6) % 3]
    address = 0xf2345678 if mode == 1 else 0x4000
    source = re.sub(r'__attribute__\(\(target\("[^"\n]*"\)\)\)', '', source)
    head = f'''#include <stdint.h>
#include <stdlib.h>
#define __NEVERC_X86_COMPRESS_EXPAND__ 1
#define __builtin_ia32_vpcompressd128_native record_compress
#define __builtin_ia32_vpexpandd128_native record_expand
typedef unsigned int recorder_vector __attribute__((vector_size(16)));
static unsigned recorder_calls;
static void record_input(void *address, unsigned long long mask, recorder_vector data, unsigned prefix) {{
 if ((uintptr_t)address != UINT64_C({address}) || mask != 5 || prefix != {prefix}) abort();
 for (unsigned i=0; i<4; ++i) if (data[i]) abort();
 ++recorder_calls;
}}
static void record_compress(void *address, unsigned long long mask, recorder_vector data, unsigned prefix) {{
 record_input(address, mask, data, prefix);
}}
static recorder_vector record_expand(void *address, unsigned long long mask, recorder_vector data, unsigned zero, unsigned prefix) {{
 if (zero != {int(mode == 2)}) abort();
 record_input(address, mask, data, prefix);
 return data;
}}
'''
    argument = 'UINT32_C(0xf2345678)' if mode == 1 else ''
    return head + source + f'''
int main(void) {{
 if (compact_segment({argument}) != 0x31313131 || recorder_calls != 1) abort();
 return 0;
}}
'''


def run(arguments, log):
    result = subprocess.run(list(map(str, arguments)), capture_output=True, text=True)
    log.write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f'{arguments}\n{result.stdout}\n{result.stderr}')


def verify_sources(args, case_count, build_recorder,
                   test_name='NativeAndBothCRoutesKeepSegmentsAndRawOffsets'):
    records = []
    source_root = args.exports / test_name
    for case in range(case_count):
        for route in ('high', 'llvm-c'):
            original = source_root / str(case) / (route + '.c')
            source = args.keep / f'{case}-{route}.c'
            source.write_text(build_recorder(original.read_text(), case))
            for compiler, name in ((args.clang, 'clang'), (args.neverc, 'neverc')):
                for opt in ('O0', 'O2'):
                    stem = f'{case}-{route}-{name}-{opt}'
                    exe = args.keep / stem
                    command = [compiler, '--target=x86_64-linux-gnu', '-march=x86-64',
                               '-std=gnu23', '-Wno-psabi', '-fno-lto', '-fno-strict-aliasing',
                               '-' + opt, source, '-o', exe]
                    command += (['-fsanitize=address,undefined'] if name == 'clang'
                                else ['-fno-builtin-mimalloc'])
                    run(command, args.keep / (stem + '-build.log'))
                    run([exe], args.keep / (stem + '-run.log'))
                    records.append({'source': str(original), 'source_sha256': digest(original),
                                    'recorder_sha256': digest(source), 'compiler': compiler,
                                    'optimization': opt, 'sanitized': name == 'clang'})
        if (case + 1) % 6 == 0:
            print({'cases': case + 1, 'source_runs': len(records)}, flush=True)
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exports', required=True, type=Path)
    parser.add_argument('--keep', required=True, type=Path)
    parser.add_argument('--clang', default='clang')
    parser.add_argument('--neverc', required=True)
    args = parser.parse_args()
    args.keep.mkdir(parents=True, exist_ok=True)
    records = verify_sources(args, 54, recorder)
    packed = args.exports / 'NarrowLiteralsFillTheCompletePackedInput/0/packed-constants.c'
    for opt in ('O0', 'O2'):
        stem = 'packed-constants-neverc-' + opt
        exe = args.keep / stem
        run([args.neverc, '--target=x86_64-linux-gnu', '-march=x86-64', '-std=gnu23',
             '-Wno-psabi', '-fno-lto', '-fno-builtin-mimalloc', '-' + opt, packed, '-o', exe],
            args.keep / (stem + '-build.log'))
        run([exe], args.keep / (stem + '-run.log'))
    report = {'source_runs': records, 'packed_constant_comparisons_neverc': 252,
              'packed_source_sha256': digest(packed), 'host_execution': 'x64 source transport',
              'physical_avx512_execution_qualified': False,
              'i386_descriptor_faults_qualified': False}
    (args.keep / 'qualification.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
