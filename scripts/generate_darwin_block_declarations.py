#!/usr/bin/env python3
"""Extract C block ABIs and exports, with explicit lifetime evidence."""
import argparse
import ctypes
import json
from pathlib import Path
import re
import tempfile

try:
    from .generate_darwin_declarations import CDeclarations, CXType, load_exports
    from .generate_objc_declarations import PrintingClang, CXCursor, TARGETS, catalog_rows
except ImportError:
    from generate_darwin_declarations import CDeclarations, CXType, load_exports
    from generate_objc_declarations import PrintingClang, CXCursor, TARGETS, catalog_rows


def noescape_attribute(pretty):
    pretty = re.sub(r'"(?:\\.|[^"\\])*"', '""', pretty)
    return bool(re.search(r'__attribute__\(\(noescape\)\)', pretty))


# Apple's dispatch API contract explicitly specifies Block_copy/Block_release
# for these functions. This is audited semantic evidence, not an inference
# from an unannotated block type. Require the complete parent and callback
# encodings as well as compiler/export agreement across every profile.
# https://github.com/apple-oss-distributions/libdispatch/blob/main/dispatch/queue.h
# https://developer.apple.com/documentation/dispatch/dispatch_after
# https://developer.apple.com/documentation/dispatch/dispatch_group_async
# https://developer.apple.com/documentation/dispatch/dispatch_group_notify
# https://developer.apple.com/documentation/dispatch/dispatch_source_set_event_handler
COPYING_CONSUMERS = {
    'dispatch_after': ('v24Q0@8@?16', 2, 'v8@?0'),
    'dispatch_async': ('v16@0@?8', 1, 'v8@?0'),
    'dispatch_barrier_async': ('v16@0@?8', 1, 'v8@?0'),
    'dispatch_group_async': ('v24@0@8@?16', 2, 'v8@?0'),
    'dispatch_group_notify': ('v24@0@8@?16', 2, 'v8@?0'),
    'dispatch_source_set_event_handler': ('v16@0@?8', 1, 'v8@?0'),
}


def block_lifetime(name, parent, index, callback, pretty):
    if noescape_attribute(pretty):
        return 'NonEscaping'
    if COPYING_CONSUMERS.get(name) == (parent, index, callback):
        return 'Copied'
    return None


class BlockDeclarations(PrintingClang, CDeclarations):
    def __init__(self, library):
        super().__init__(library)
        self.bind('clang_Cursor_getArgument', CXCursor, CXCursor, ctypes.c_uint)
        self.bind('clang_getCanonicalType', CXType, CXType)
        self.bind('clang_getPointeeType', CXType, CXType)
        self.bind('clang_getResultType', CXType, CXType)
        self.bind('clang_getNumArgTypes', ctypes.c_int, CXType)
        self.bind('clang_getArgType', CXType, CXType, ctypes.c_uint)
        self.bind('clang_isFunctionTypeVariadic', ctypes.c_uint, CXType)
        self.bind('clang_Type_getSizeOf', ctypes.c_longlong, CXType)

    def scalar(self, value, depth=0):
        if depth > 16:
            return None
        value = self.clang_getCanonicalType(value)
        kind, size = value.kind, self.clang_Type_getSizeOf(value)
        if kind == 2:
            return 'v', 0
        if kind == 3 and size == 1:
            return 'B', 1
        if kind in (4, 5, 8, 9, 10, 11) and size in (1, 2, 4, 8):
            return {1: 'C', 2: 'S', 4: 'I', 8: 'Q'}[size], size
        if kind in (13, 14, 16, 17, 18, 19) and size in (1, 2, 4, 8):
            return {1: 'c', 2: 's', 4: 'i', 8: 'q'}[size], size
        if (kind, size) in ((21, 4), (22, 8)):
            return 'f' if size == 4 else 'd', size
        if kind in (27, 28, 29, 109) and size == 8:
            return '@', 8
        if kind == 102 and size == 8:
            return '@?', 8
        if kind == 101 and size == 8:
            pointee = self.clang_getCanonicalType(self.clang_getPointeeType(value))
            if pointee.kind in (105, 112):  # Opaque record or array pointee.
                return '^v', 8
            encoded = self.scalar(pointee, depth + 1)
            return ('^' + encoded[0], 8) if encoded else None
        return None

    def callback(self, value):
        value = self.clang_getCanonicalType(value)
        if value.kind != 102:  # Function pointers are not block objects.
            return None
        function = self.clang_getPointeeType(value)
        count = self.clang_getNumArgTypes(function)
        if (function.kind != 111 or not 0 <= count <= 62 or
                self.clang_isFunctionTypeVariadic(function) or
                self.clang_getFunctionTypeCallingConv(function) != 1):
            return None
        result = self.scalar(self.clang_getResultType(function))
        arguments = [self.scalar(self.clang_getArgType(function, i)) for i in range(count)]
        if not result or any(not arg or arg[0] == 'v' for arg in arguments):
            return None
        # Normalized encoding with the implicit block object at byte zero.
        # Physical registers and stack slots are assigned by the ABI owner.
        offset, parameters = 8, '@?0'
        for encoding, size in arguments:
            parameters += encoding + str(offset)
            offset += size
        return result[0] + str(offset) + parameters

    def declaration(self, cursor):
        ordinary = super().declaration(cursor)
        if ordinary is None or not ordinary[1]:
            return ordinary
        blocks = []
        for index in range(self.clang_Cursor_getNumArguments(cursor)):
            parameter = self.clang_Cursor_getArgument(cursor, index)
            signature = self.callback(self.clang_getCursorType(parameter))
            lifetime = block_lifetime(ordinary[0], ordinary[1], index,
                                      signature, self.pretty(parameter))
            if signature and lifetime:
                blocks.append([index, signature, lifetime])
        # Alternatives without either kind of lifetime evidence veto the contract.
        return ordinary[0], json.dumps([ordinary[1], blocks], separators=(',', ':')) if blocks else ''


def render(profiles, exports, version, compiler):
    lines = ['// clang-format off',
             '// Generated by scripts/generate_darwin_block_declarations.py.',
             f'// Compiler-derived block ABI and export facts: MacOSX SDK {version}.',
             '// Lifetime: compiler noescape or audited copying consumers in generator.',
             f'// {compiler}', '// Target profiles: ' + ', '.join(TARGETS),
             '// No SDK implementation is included.']
    for name, arm, intel in catalog_rows(profiles):
        if not arm or not intel:
            continue
        a, b = json.loads(arm), json.loads(intel)
        if [(arg[0], arg[2]) for arg in a[1]] != [(arg[0], arg[2]) for arg in b[1]]:
            continue
        modules = ['|'.join(sorted(index.get(name, ()))) for index in exports]
        if not all(modules):
            continue
        for (parameter, left, lifetime), (_, right, _) in zip(a[1], b[1]):
            if lifetime not in ('NonEscaping', 'Copied'):
                continue
            row = [name, a[0], b[0], parameter, left, right, *modules]
            lines.append('{' + ', '.join(map(json.dumps, row)) +
                         ', DarwinBlockParameterContract::Lifetime::' + lifetime + '},')
    lines.append('    // clang-format on')
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--libclang', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    sdk = args.sdk.resolve(strict=True)
    clang = BlockDeclarations(args.libclang)
    with tempfile.TemporaryDirectory(prefix='neverd-block-declarations-') as work:
        source = Path(work) / 'declarations.m'
        source.write_text('#import <Foundation/Foundation.h>\n#include <dispatch/dispatch.h>\n')
        profiles = [clang.extract(source, sdk, target) for target in TARGETS]
    version = json.loads((sdk / 'SDKSettings.json').read_text())['Version']
    output = render(profiles, load_exports(sdk), version,
                    clang.string(clang.clang_getClangVersion()))
    if args.check:
        if args.output.read_text() != output:
            parser.error('generated catalog differs; regenerate with this SDK')
    else:
        args.output.write_text(output)
    print(f'verified {sum(line.startswith(chr(123)) for line in output.splitlines())} block lifetime contracts')


if __name__ == '__main__':
    main()
