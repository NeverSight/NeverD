#!/usr/bin/env python3
"""Check exported i386 CS/ES/SS RAO C operand transport on an x64 host.

Run NeverDX86AtomicSegmentTests with NEVERD_ISA_REGRESSION_ARTIFACT_DIR first.
Native operations are replaced by checked recorders. This validates source
storage and values, not i386 native execution, atomicity, or descriptor faults.
"""
import argparse
import json
import re
from pathlib import Path

from verify_x86_compact_segments import verify_sources

NAMES = ('aadd', 'aand', 'aor', 'axor')


def recorder(source, case):
    kind = (case // 2) % 4
    prefix = (0x26, 0x2e, 0x36)[(case // 8) % 3]
    parameter = (kind + case % 2) % 3 == 2
    address = 0xf2345678 if parameter else 0x4000
    source = re.sub(r'__attribute__\(\(target\("[^"\n]*"\)\)\)', '', source)
    head = f'''#include <stdint.h>
#include <stdlib.h>
#define __NEVERC_X86_LEGACY_ATOMIC__ 1
#define __builtin_ia32_{NAMES[kind]}32_legacy_address32 record_atomic
static unsigned recorder_calls;
static void record_atomic(uint32_t address, uint32_t source, unsigned bits, unsigned prefix) {{
 if (address != UINT32_C({address}) || source != UINT32_C(0x87654321) || bits != 32 || prefix != {prefix}) abort();
 ++recorder_calls;
}}
'''
    argument = 'UINT32_C(0xf2345678)' if parameter else ''
    return head + source + f'''
int main(void) {{
 if (atomic_segment({argument}) != 0x31313131 || recorder_calls != 1) abort();
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
    report = {'source_runs': verify_sources(args, 72, recorder,
                                           'AllFourKindsKeepSegmentsAndRawOffsets'),
              'host_execution': 'x64 source transport',
              'physical_atomicity_qualified': False,
              'i386_descriptor_faults_qualified': False}
    (args.keep / 'qualification.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
