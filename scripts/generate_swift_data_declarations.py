#!/usr/bin/env python3
"""Extract standard Swift metadata, witness, and conformance identities.

Only direct addresses of external, non-TLS globals qualify. Four
Darwin compiler/export profiles must agree; no runtime layout is inferred.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile

try:
    from .generate_darwin_declarations import export_index
    from .generate_swift_metadata_declarations import TARGETS, EXPORT_TARGETS
except ImportError:
    from generate_darwin_declarations import export_index
    from generate_swift_metadata_declarations import TARGETS, EXPORT_TARGETS


METADATA_TYPES = ("Any", "AnyHashable", "String", "Substring", "Bool", "Int", "Int8",
                  "Int16", "Int32", "Int64", "UInt", "UInt8", "UInt16",
                  "UInt32", "UInt64", "Float", "Double")
HASHABLE_TYPES = tuple(name for name in METADATA_TYPES
                       if name not in ("Any", "Substring"))


def hashable_value(name):
    return "Swift.AnyHashable(0)" if name == "AnyHashable" else f"Swift.{name}()"


def metadata_storage(ir, probes):
    if len(ir) > 16 * 1024 * 1024:
        raise ValueError("metadata storage IR exceeds the input budget")
    declarations = {}
    for name, value in re.findall(r'^@"([^"\n]+)" = ([^\n]+)$', ir, re.M):
        declarations.setdefault(name, []).append(value)
    result = set()
    for probe in probes:
        if not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", probe):
            raise ValueError("invalid probe identifier")
        bodies = re.findall(r'^define [^\n]*@' + probe +
                            r'\(\)[^\n]*\{\n(.*?)^\}', ir, re.M | re.S)
        if len(bodies) != 1:
            raise ValueError("missing or ambiguous metadata storage probe")
        body = [line.strip() for line in bodies[0].splitlines()
                if line.strip() and not line.lstrip().startswith(';')]
        if body and body[0] == 'entry:':
            body.pop(0)
        direct = (re.fullmatch(r'ret ptr @"([^"\n]+)"', body[0])
                  if len(body) == 1 else None)
        # Any.self is the metadata member embedded eight bytes into Swift's
        # exported full-existential storage, rather than the storage base.
        existential = (re.fullmatch(
            r'ret ptr getelementptr inbounds \(i8, ptr @"([^"\n]+)", i64 8\)',
            body[0]) if len(body) == 1 else None)
        returned = direct or existential
        if not returned:
            continue
        name = returned[1]
        values = declarations.get(name, [])
        expected = ('external global %swift.type, align 8' if direct else
                    'external global %swift.full_existential_type')
        if len(values) == 1 and values[0] == expected:
            result.add(name)
    return result


def witness_storage(ir, probes, metadata):
    """Read the witness argument of a compiler-generated generic Hashable call.

    The Swift source owns the generic constraint and call ABI. Only its exact
    direct global argument qualifies; this grants no witness-table layout or
    permission to call a witness member.
    """
    if len(ir) > 16 * 1024 * 1024:
        raise ValueError("witness storage IR exceeds the input budget")
    declarations = {}
    for name, value in re.findall(r'^@"([^"\n]+)" = ([^\n]+)$', ir, re.M):
        declarations.setdefault(name, []).append(value)
    callee = re.findall(r'^declare ([^\n]*@neverd_hashable_probe[^\n]*)$', ir, re.M)
    if len(callee) != 1 or not re.fullmatch(
            r'swiftcc void @neverd_hashable_probe\(ptr noalias, ptr, ptr\)'
            r'(?: local_unnamed_addr)?(?: #[0-9]+)?', callee[0]):
        raise ValueError("generic witness probe has an unexpected call ABI")
    result = set()
    for probe in probes:
        if not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", probe):
            raise ValueError("invalid probe identifier")
        bodies = re.findall(r'^define [^\n]*@' + probe +
                            r'\(\)[^\n]*\{\n(.*?)^\}', ir, re.M | re.S)
        if len(bodies) != 1:
            raise ValueError("missing or ambiguous witness storage probe")
        calls = [line.strip() for line in bodies[0].splitlines()
                 if '@neverd_hashable_probe' in line]
        call = re.fullmatch(
            r'call swiftcc void @neverd_hashable_probe\('
            r'ptr noalias nonnull %[A-Za-z_0-9.]+, '
            r'ptr nonnull @"([^"\n]+)", ptr nonnull @"([^"\n]+)"\)'
            r'(?: #[0-9]+)?', calls[0]) if len(calls) == 1 else None
        if not call or call[1] not in metadata:
            continue
        values = declarations.get(call[2], [])
        if len(values) == 1 and values[0] == 'external global ptr, align 8':
            result.add(call[2])
    return result


def conformance_storage(ir, probes, metadata):
    """Read descriptors used by compiler-generated lazy witness accessors.

    The named probe must call a zero-argument accessor and pass its result to
    the declared generic StringProtocol probe. The accessor must in turn pass
    direct descriptor and metadata globals to swift_getWitnessTable and cache
    the result with a release store. This extracts external storage identity;
    it grants no descriptor or witness-table layout.
    """
    if len(ir) > 16 * 1024 * 1024:
        raise ValueError("conformance storage IR exceeds the input budget")
    declarations = {}
    for name, value in re.findall(r'^@"([^"\n]+)" = ([^\n]+)$', ir, re.M):
        declarations.setdefault(name, []).append(value)
    runtime = re.findall(r'^declare ([^\n]*@swift_getWitnessTable[^\n]*)$',
                         ir, re.M)
    if len(runtime) != 1 or not re.fullmatch(
            r'ptr @swift_getWitnessTable\(ptr, ptr, ptr\)'
            r'(?: local_unnamed_addr)?(?: #[0-9]+)?', runtime[0]):
        raise ValueError("witness accessor has an unexpected runtime ABI")
    generic = re.findall(
        r'^declare ([^\n]*@neverd_string_protocol_probe[^\n]*)$', ir, re.M)
    if len(generic) != 1 or not re.fullmatch(
            r'swiftcc void @neverd_string_protocol_probe\(ptr noalias, ptr, ptr\)'
            r'(?: local_unnamed_addr)?(?: #[0-9]+)?', generic[0]):
        raise ValueError("StringProtocol probe has an unexpected call ABI")

    result = set()
    for probe in probes:
        if not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", probe):
            raise ValueError("invalid probe identifier")
        bodies = re.findall(r'^define [^\n]*@' + probe +
                            r'\(\)[^\n]*\{\n(.*?)^\}', ir, re.M | re.S)
        if len(bodies) != 1:
            raise ValueError("missing or ambiguous conformance storage probe")
        getter_calls = re.findall(
            r'^\s*(%[A-Za-z_0-9.]+) = (?:tail )?call ptr '
            r'@"([^"\n]+)"\(\)(?: #[0-9]+)?$', bodies[0], re.M)
        generic_calls = re.findall(
            r'^\s*call swiftcc void @neverd_string_protocol_probe\('
            r'ptr noalias nonnull %[A-Za-z_0-9.]+, '
            r'ptr nonnull @"([^"\n]+)", ptr (%[A-Za-z_0-9.]+)\)'
            r'(?: #[0-9]+)?$', bodies[0], re.M)
        if len(getter_calls) != 1 or len(generic_calls) != 1 or \
                generic_calls[0][0] not in metadata or \
                generic_calls[0][1] != getter_calls[0][0]:
            continue
        accessors = re.findall(
            r'^define [^\n]*@"' + re.escape(getter_calls[0][1]) +
            r'"\(\)[^\n]*\{\n(.*?)^\}', ir, re.M | re.S)
        if len(accessors) != 1:
            continue
        witness_calls = re.findall(
            r'^\s*(%[A-Za-z_0-9.]+) = tail call ptr '
            r'@swift_getWitnessTable\(ptr nonnull @"([^"\n]+)", '
            r'ptr nonnull @"([^"\n]+)", ptr undef\)(?: #[0-9]+)?$',
            accessors[0], re.M)
        if len(witness_calls) != 1 or witness_calls[0][2] != generic_calls[0][0]:
            continue
        stores = re.findall(
            r'^\s*store atomic ptr ' + re.escape(witness_calls[0][0]) +
            r', ptr @"([^"\n]+)" release, align 8$', accessors[0], re.M)
        loads = re.findall(
            r'^\s*%[A-Za-z_0-9.]+ = load ptr, ptr @"([^"\n]+)", align 8$',
            accessors[0], re.M)
        descriptor = witness_calls[0][1]
        values = declarations.get(descriptor, [])
        if len(stores) == 1 and stores[0] in loads and len(values) == 1 and \
                values[0] == ('external global '
                              '%swift.protocol_conformance_descriptor, align 4'):
            result.add(descriptor)
    return result


def render(profiles, exports, version, compiler):
    if len(profiles) != 4 or len(exports) != 4:
        raise ValueError("all four compiler and export profiles are required")
    lines = ["// clang-format off",
             "// Generated by scripts/generate_swift_data_declarations.py.",
             f"// Compiler-derived external metadata/witness/conformance storage: MacOSX SDK {version}.",
             "// " + compiler.replace("\n", "; "),
             "// Target profiles: " + ", ".join(TARGETS),
             "// Direct non-TLS global addresses; no runtime layout or implementation."]
    for name in sorted(set.intersection(*profiles)):
        modules = [exports[i].get(name, set()) & exports[i + 1].get(name, set())
                   for i in (0, 2)]
        if all(modules):
            lines.append("{" + ", ".join(json.dumps(x) for x in
                         (name, *("|".join(sorted(m)) for m in modules))) + "},")
    return "\n".join(lines + ["// clang-format on", ""])


def run(command):
    return subprocess.run(command, check=True, capture_output=True, text=True,
                          timeout=180).stdout


def main():
    import yaml
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--swiftc', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    sdk = args.sdk.resolve(strict=True)
    probes = ['metadata_' + name for name in METADATA_TYPES]
    witnesses = ['witness_' + name for name in HASHABLE_TYPES]
    conformances = ['witness_StringProtocol']
    with tempfile.TemporaryDirectory(prefix='neverd-swift-data-') as work:
        source = Path(work) / 'metadata.swift'
        source.write_text('\n'.join(
            f'@_cdecl("{probe}") public func {probe}() -> UnsafeRawPointer {{ '
            f'unsafeBitCast({"Any" if name == "Any" else "Swift." + name}.self, '
            'to: UnsafeRawPointer.self) }'
            for probe, name in zip(probes, METADATA_TYPES)) + '\n' +
            '@_silgen_name("neverd_hashable_probe") '
            'func observe<T: Hashable>(_ value: T)\n' + '\n'.join(
                f'@_cdecl("{probe}") public func {probe}() {{ '
                f'observe({hashable_value(name)}) }}'
                for probe, name in zip(witnesses, HASHABLE_TYPES)) + '\n' +
            '@_silgen_name("neverd_string_protocol_probe") '
            'func observeStringProtocol<T: StringProtocol>(_ value: T)\n' +
            '@_cdecl("witness_StringProtocol") public func '
            'witness_StringProtocol() { observeStringProtocol("") }\n')
        profiles = []
        for index, target in enumerate(TARGETS):
            ir = Path(work) / f'{index}.ll'
            run([str(args.swiftc), '-O', '-parse-as-library', '-target', target,
                 '-sdk', str(sdk), '-emit-ir', str(source), '-o', str(ir)])
            text = ir.read_text()
            metadata = metadata_storage(text, probes)
            profiles.append(metadata |
                            witness_storage(text, witnesses, metadata) |
                            conformance_storage(text, conformances, metadata))
    class TBDLoader(yaml.SafeLoader):
        pass
    TBDLoader.add_constructor('!tapi-tbd', lambda loader, node:
                              loader.construct_mapping(node, deep=True))
    documents = list(yaml.load_all(
        (sdk / 'usr/lib/swift/libswiftCore.tbd').read_text(), Loader=TBDLoader))
    exports = [export_index(documents, target) for target in EXPORT_TARGETS]
    output = render(profiles, exports,
                    json.loads((sdk / 'SDKSettings.json').read_text())['Version'],
                    run([str(args.swiftc), '--version']).strip())
    count = sum(line.startswith('{') for line in output.splitlines())
    expected = len(METADATA_TYPES) + len(HASHABLE_TYPES) + len(conformances)
    if count != expected:
        parser.error('not all standard storage queries have complete evidence '
                     f'({count}/{expected})')
    if args.check:
        if args.output.read_text() != output:
            parser.error('generated Swift data catalog differs')
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output)
    print(f'verified {count} external metadata/witness storage identities')


if __name__ == '__main__':
    main()
