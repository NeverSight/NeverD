#!/usr/bin/env python3
"""Check exported i386 CS/ES/SS four-source C operand transport on an x64 host.

Run NeverDX86FourSourceSegmentTests with NEVERD_ISA_REGRESSION_ARTIFACT_DIR first.
Only the native operation is replaced by a recorder; this does not qualify
i386 execution, Xeon Phi arithmetic, or segment-descriptor faults.
"""
import argparse
import json
import re
from pathlib import Path

from verify_x86_compact_segments import verify_sources

NAMES = ('v4fmaddps', 'v4fnmaddps', 'v4fmaddss', 'v4fnmaddss', 'vp4dpwssd', 'vp4dpwssds')


def recorder(source, case):
    kind = (case // 2) % 6
    prefix = (0x26, 0x2e, 0x36)[(case // 12) % 3]
    parameter = kind % 2
    address = 0xf2345678 if parameter else 0x4000
    size = 16 if kind in (2, 3) else 64
    source = re.sub(r'__attribute__\(\(target\("[^"\n]*"\)\)\)', '', source)
    head = f'''#include <stdint.h>
#include <stdlib.h>
#define __NEVERC_X86_FOUR_SOURCE__ 1
#define __builtin_ia32_{NAMES[kind]}_native record_four
typedef unsigned int recorder_vector __attribute__((vector_size({size})));
static unsigned recorder_calls;
static recorder_vector record_four(void *address, recorder_vector acc,
 recorder_vector a, recorder_vector b, recorder_vector c, recorder_vector d,
 unsigned short mask, unsigned zero, unsigned prefix) {{
 if ((uintptr_t)address != UINT64_C({address}) || mask != 5 || zero != {parameter} || prefix != {prefix}) abort();
 for (unsigned i=0; i<{size // 4}; ++i)
  if (acc[i] || a[i] || b[i] || c[i] || d[i]) abort();
 ++recorder_calls;
 return acc;
}}
'''
    argument = 'UINT32_C(0xf2345678)' if parameter else ''
    return head + source + f'''
int main(void) {{
 if (four_segment({argument}) != 0x31313131 || recorder_calls != 1) abort();
 return 0;
}}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exports', required=True, type=Path)
    parser.add_argument('--keep', required=True, type=Path)
    parser.add_argument('--clang', default='clang')
    parser.add_argument('--neverc', required=True)
    args = parser.parse_args()
    args.keep.mkdir(parents=True, exist_ok=True)
    report = {'source_runs': verify_sources(args, 108, recorder,
                                           'AllSixKindsKeepSegmentsAndRawOffsets'),
              'host_execution': 'x64 source transport',
              'physical_four_source_execution_qualified': False,
              'i386_descriptor_faults_qualified': False}
    (args.keep / 'qualification.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
