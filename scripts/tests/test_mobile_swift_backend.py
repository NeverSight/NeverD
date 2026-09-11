"""The real Swift runner must reject missing or partial callable coverage."""
from __future__ import annotations

import json
import importlib.util
from collections import Counter
from copy import deepcopy
from contextlib import redirect_stderr, redirect_stdout
import errno
import io
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
# unittest discovery imports this test under the runner's basename too.
# Load the actual acceptance script by path so it cannot resolve to this file.
spec = importlib.util.spec_from_file_location(
    "neverd_swift_acceptance_runner", Path(__file__).resolve().parents[1] / "test_mobile_swift_backend.py")
backend = importlib.util.module_from_spec(spec)
spec.loader.exec_module(backend)


class SwiftBackendAcceptanceTests(unittest.TestCase):
    EMPTY_NATIVE_ENTRY = 0x40a0
    EMPTY_INITIALIZER_ENTRY = 0x40b0

    def empty_entries(self):
        return {backend.EMPTY_SYMBOL: self.EMPTY_NATIVE_ENTRY,
                backend.EMPTY_INITIALIZER_SYMBOL: self.EMPTY_INITIALIZER_ENTRY}

    def empty_nm(self, entries=None):
        return ''.join(f'{entry:016x} t {symbol}\n'
                       for symbol, entry in (self.empty_entries() if entries is None else entries).items())

    def compiler_roles(self):
        manifest = backend.FIXTURE.with_suffix('.callables.json')
        return json.loads(manifest.read_text())['callables']

    def reports(self):
        rows = []
        for index, key in enumerate(sorted(backend.DECLARATIONS)):
            kind, context, name, declaration = key
            rows.append({'entry': hex(0x1000 + index * 16), 'mangled_symbol': f'_$s4Demo{index}F',
                         'context_kind': kind, 'context_name': context, 'name': name,
                         'declaration_kind': declaration, 'classification': 'callable',
                         'node_kind': 'Function' if declaration == 'function' else 'Constructor' if kind == 'class' else 'Allocator',
                         'status': 'recovered', 'source_representation': 'native-method-body'})
        for index, expected in enumerate(self.compiler_roles()):
            row = {**expected, 'entry': hex(0x4000 + index * 16),
                   'classification': 'callable', 'status': 'recovered'}
            if row['source_representation'] == 'compiler-generated-from-type':
                row['compiler_projection_evidence'] = ['Native fixture entry and compiler role verified.']
            rows.append(row)
        return rows

    def write_reports(self, output, rows, *, lying_count=False, counts=None,
                      demangler=None, demangle_logs=False, symbols=(), inventory_rows=None,
                      architecture='arm64', source_units=None, types=None, source_text=None):
        (output / 'metadata').mkdir(parents=True)
        (output / 'sources').mkdir()
        (output / 'sources/swift.swift').write_text(backend.EMPTY_SOURCE if source_text is None else source_text)
        recovered = sum(row['status'] == 'recovered' for row in rows)
        compiler = sum(row['status'] == 'recovered'
                       and row.get('source_representation') == 'compiler-generated-from-type' for row in rows)
        status = 'recovered' if recovered == len(rows) else 'partial'
        empty = [row for row in rows if row.get('mangled_symbol') in self.empty_entries()]
        if source_units is None:
            source_units = [] if not empty else [{
                'kind': 'type', 'module': backend.MODULE, 'name': 'Empty',
                'method_entries': [row['entry'] for row in empty],
                'method_identities': [{'entry': row['entry'], 'mangled_symbol': row['mangled_symbol']}
                                      for row in empty],
            }]
        if types is None:
            types = [{'module': backend.MODULE, 'kind': 'struct', 'name': 'Empty',
                      'status': 'recovered', 'reason': '', 'size': 0, 'alignment': 1, 'fields': []}]
        coverage = {'schema_version': 1, 'methods': rows, 'non_method_symbols': list(symbols),
                    'source_units': source_units, 'types': types,
                    'method_count': len(rows) + int(lying_count), 'recovered_method_count': recovered,
                    'unrecovered_method_count': len(rows) - recovered, 'symbol_count': len(rows) + len(symbols),
                    'source_body_method_count': recovered - compiler, 'compiler_projection_method_count': compiler,
                    'unclassified_symbol_count': 0, 'status': status, 'coverage_status': status}
        coverage.update(counts or {})
        report = {'status': 'success', 'platform': 'ios', 'architecture': architecture, 'swift_method_recovery': coverage}
        (output / 'report.json').write_text(json.dumps(report))
        (output / 'metadata/swift-methods.json').write_text(json.dumps(coverage))
        inventory = {'methods': rows if inventory_rows is None else inventory_rows, 'symbols': list(symbols),
                     'demangler': demangler if demangler is not None else
                     {'name': 'llvm-swift-demangle', 'execution': 'builtin', 'version': '6.3.3'}}
        if demangle_logs:
            inventory['logs'] = ['swift-demangle-0000.log']
        (output / 'metadata/swift-signatures.json').write_text(json.dumps(inventory))

    def check(self, rows, *, original=None, symbols=(), empty_entries=None, **options):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            self.write_reports(output, rows, symbols=symbols, **options)
            nm = '\n'.join(row['mangled_symbol'] for row in ([*rows, *symbols] if original is None else original))
            with patch.object(backend, 'run', return_value=nm):
                return backend.validate_coverage(output, output / 'original', 'arm64',
                                                 empty_entries=self.empty_entries() if empty_entries is None else empty_entries)

    def verify_matrix(self, work, *, arch='all', fixups='both', unavailable=None, error_number=errno.ENOEXEC):
        arguments = SimpleNamespace(arch=arch, fixups=fixups, setup_only=False,
                                    neverd=work / 'neverd', module_cache=None, timeout=1)
        rows = self.reports()
        oracle = '\n'.join(f'{key}={value}' for key, value in backend.expected_results().items())
        executed = []
        compiler = str((work / 'test-tools' / 'swiftc').resolve())
        demangler = str((work / 'test-tools' / 'apple-swift-demangle').resolve())

        def tool(argv, **options):
            # Replace only the external tool boundary. Report validation, the
            # mathematical oracle, and the complete runner flow stay real.
            if argv[0] == compiler:
                if argv[1:] == ['--version']:
                    return 'Mock Apple Swift compiler version for command-boundary tests\n'
                output = Path(argv[argv.index('-o') + 1])
                output.write_bytes(struct.pack('<8I', 0xFEEDFACF, 0, 0, 0, 0, 0, 0, 0))
                return ''
            if argv[0] == str(arguments.neverd.resolve()):
                output = Path(argv[argv.index('-o') + 1])
                architecture = next(arg.removeprefix('--arch=') for arg in argv if arg.startswith('--arch='))
                self.write_reports(output, rows, architecture=architecture)
                return ''
            if argv[0] == '/usr/bin/nm':
                if '-j' in argv:
                    return '\n'.join(row['mangled_symbol'] for row in rows)
                return '\n'.join(f'{int(row["entry"], 16):016x} T {row["mangled_symbol"]}' for row in rows)
            if argv == ['/usr/bin/xcrun', '--find', 'swift-demangle']:
                return demangler + '\n'
            if argv == [demangler, '--compact', backend.EMPTY_SYMBOL]:
                return 'type metadata accessor for SwiftBehavior.Empty\n'
            if argv == [demangler, '--compact', backend.EMPTY_INITIALIZER_SYMBOL]:
                return 'SwiftBehavior.Empty.init() -> SwiftBehavior.Empty\n'
            executable = Path(argv[0])
            label = executable.parent.name
            if executable.name not in ('original', 'rebuilt'):
                raise AssertionError(f'Unexpected external command: {argv!r}')
            if label.startswith(f'{unavailable}-'):
                raise OSError(error_number, 'unsupported executable architecture')
            executed.append((label, executable.name))
            return oracle

        stdout = io.StringIO()
        with patch.object(backend.sys, 'platform', 'darwin'), \
                patch.object(backend.platform, 'machine', return_value='arm64'), \
                patch.object(backend.shutil, 'which', return_value=compiler), \
                patch.object(backend, 'run', side_effect=tool), \
                redirect_stdout(stdout), redirect_stderr(io.StringIO()):
            backend.verify(arguments, work)
        return executed, stdout.getvalue()

    def test_requested_matrix_cannot_pass_when_an_architecture_cannot_execute(self):
        for error_number in (errno.ENOEXEC, 86):
            with self.subTest(error_number=error_number), tempfile.TemporaryDirectory() as temporary:
                work = Path(temporary)
                with self.assertRaisesRegex(RuntimeError, 'x86_64-classic.*cannot execute'):
                    self.verify_matrix(work, unavailable='x86_64', error_number=error_number)
                for fixup in ('classic', 'default'):
                    failed = work / f'x86_64-{fixup}'
                    self.assertTrue((failed / 'original').is_file())
                    self.assertTrue((failed / 'libSwiftBehavior.dylib').is_file())
                    self.assertIn('cannot execute', (failed / 'failure.txt').read_text())

    def test_only_the_explicitly_requested_matrix_is_required(self):
        for arch in ('all', 'arm64', 'x86_64'):
            for fixups in ('both', 'classic', 'default'):
                with self.subTest(arch=arch, fixups=fixups), tempfile.TemporaryDirectory() as temporary:
                    executed, stdout = self.verify_matrix(Path(temporary), arch=arch, fixups=fixups)
                    architectures = ('arm64', 'x86_64') if arch == 'all' else (arch,)
                    variants = ('classic', 'default') if fixups == 'both' else (fixups,)
                    expected = Counter((f'{architecture}-{fixup}', program)
                                       for architecture in architectures for fixup in variants
                                       for program in ('original', 'rebuilt'))
                    self.assertEqual(Counter(executed), expected)
                    self.assertIn(f'Verified {len(architectures) * len(variants)} recovered variants', stdout)
                    self.assertNotIn('SKIP', stdout)

    def test_explicit_unavailable_architecture_has_a_failure_diagnostic(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            with self.assertRaisesRegex(RuntimeError, '0/1 completed.*\n.*x86_64-default.*cannot execute'):
                self.verify_matrix(work, arch='x86_64', fixups='default', unavailable='x86_64')
            self.assertTrue((work / 'x86_64-default/original').is_file())

    def test_complete_declared_and_callable_inventory_passes(self):
        coverage = self.check(self.reports())
        self.assertEqual(coverage['method_count'], 34)
        self.assertEqual(coverage['source_body_method_count'], 25)
        self.assertEqual(coverage['compiler_projection_method_count'], 9)
        old = [row for row in coverage['methods'] if row['mangled_symbol'] not in self.empty_entries()]
        self.assertEqual(len(old), 32)
        self.assertEqual(sum(row['declaration_kind'] != 'runtime' for row in old), 25)
        self.assertEqual(sum(row['declaration_kind'] == 'runtime' for row in old), 7)

    def test_required_accessors_and_compiler_callables_cannot_be_reclassified_as_metadata(self):
        for expected in self.compiler_roles():
            with self.subTest(symbol=expected['mangled_symbol']):
                original = self.reports()
                rows = [row for row in original if row['mangled_symbol'] != expected['mangled_symbol']]
                metadata = next(dict(row) for row in original if row['mangled_symbol'] == expected['mangled_symbol'])
                metadata['classification'] = 'metadata'
                with self.assertRaisesRegex(RuntimeError, 'required fixture callable'):
                    self.check(rows, symbols=[metadata], original=original)

    def test_getter_and_setter_roles_cannot_be_swapped_with_counts_unchanged(self):
        rows = self.reports()
        getter = next(row for row in rows if row['context_name'] == 'Counter' and row['declaration_kind'] == 'getter')
        setter = next(row for row in rows if row['context_name'] == 'Counter' and row['declaration_kind'] == 'setter')
        for field in ('node_kind', 'declaration_kind'):
            getter[field], setter[field] = setter[field], getter[field]
        with self.assertRaisesRegex(RuntimeError, 'required fixture callable.*role'):
            self.check(rows)

    def test_required_callable_omission_cannot_shrink_the_denominator(self):
        for expected in self.compiler_roles():
            with self.subTest(symbol=expected['mangled_symbol']):
                original = self.reports()
                rows = [row for row in original if row['mangled_symbol'] != expected['mangled_symbol']]
                with self.assertRaisesRegex(RuntimeError, 'symbol inventory is incomplete'):
                    self.check(rows, original=original)

    def test_changed_original_inventory_requires_explicit_fixture_review(self):
        for expected in self.compiler_roles():
            with self.subTest(symbol=expected['mangled_symbol']):
                rows = [row for row in self.reports() if row['mangled_symbol'] != expected['mangled_symbol']]
                # Even if the compiler itself stops emitting a required role,
                # the smaller nm inventory is not silently a complete pass.
                with self.assertRaisesRegex(RuntimeError, 'required fixture callable.*original'):
                    self.check(rows)

    def test_callable_roles_are_checked_in_each_report(self):
        for report in ('coverage', 'signature inventory'):
            for field, value in (('node_kind', 'Function'), ('context_kind', 'class'),
                                 ('context_name', 'Calculator'), ('name', 'bias'),
                                 ('declaration_kind', 'function')):
                with self.subTest(report=report, field=field):
                    rows = self.reports()
                    inventory = deepcopy(rows)
                    changed = rows if report == 'coverage' else inventory
                    setter = next(row for row in changed if row['declaration_kind'] == 'setter')
                    setter[field] = value
                    with self.assertRaisesRegex(RuntimeError, 'required fixture callable.*role'):
                        self.check(rows, inventory_rows=inventory)

    def test_runtime_projection_kind_cannot_be_replaced_with_another_nonempty_kind(self):
        rows = self.reports()
        deallocator = next(row for row in rows if row['node_kind'] == 'Deallocator')
        deallocator['compiler_projection_kind'] = 'trivial_destructor'
        with self.assertRaisesRegex(RuntimeError, 'required fixture callable.*compiler_projection_kind'):
            self.check(rows)

    def test_distinct_callable_roles_may_share_a_native_entry(self):
        rows = self.reports()
        getter = next(row for row in rows if row['context_name'] == 'Counter' and row['declaration_kind'] == 'getter')
        resume = next(row for row in rows if row['node_kind'] == 'CoroutineContinuation')
        resume['entry'] = getter['entry']
        # The real optimized corpus aliases these symbols; identity is not VA alone.
        coverage = self.check(rows)
        self.assertEqual(coverage['method_count'], 34)
        self.assertEqual(coverage['compiler_projection_method_count'], 9)

    def test_real_type_metadata_stays_outside_the_callable_denominator(self):
        metadata = {'entry': '0x9000', 'mangled_symbol': '_$s13SwiftBehavior7CounterVN',
                    'classification': 'metadata', 'node_kind': 'TypeMetadata'}
        coverage = self.check(self.reports(), symbols=[metadata])
        self.assertEqual(coverage['method_count'], 34)
        self.assertEqual(coverage['symbol_count'], 35)

    def test_missing_or_external_demangler_cannot_pass(self):
        for demangler in ({}, {'name': 'llvm-swift-demangle', 'execution': 'external', 'version': '6.3.3'},
                          {'name': 'llvm-swift-demangle', 'execution': 'builtin', 'version': '0.0.0'},
                          {'name': 'llvm-swift-demangle', 'execution': 'builtin', 'version': 123}):
            with self.subTest(demangler=demangler), self.assertRaisesRegex(RuntimeError, 'builtin LLVM demangler'):
                self.check(self.reports(), demangler=demangler)

    def test_external_demangler_logs_cannot_pass(self):
        with self.assertRaisesRegex(RuntimeError, 'external demangler/toolchain logs'):
            self.check(self.reports(), demangle_logs=True)

    def test_compiler_callable_is_not_ignored_when_user_methods_pass(self):
        rows = self.reports()
        rows.append({'entry': '0x5000', 'mangled_symbol': '_$s4DemoGetterF', 'node_kind': 'Getter',
                     'classification': 'callable', 'status': 'unrecovered', 'reason': 'unbound getter body'})
        with self.assertRaisesRegex(RuntimeError, 'Getter.*unbound getter body'):
            self.check(rows)

    def test_removing_symbol_from_both_reports_cannot_hide_it(self):
        original = self.reports()
        with self.assertRaisesRegex(RuntimeError, 'symbol inventory is incomplete'):
            self.check(original[:-1], original=original)

    def test_declared_initializer_identity_must_remain_visible(self):
        rows = self.reports()
        for row in rows:
            if (row['context_kind'] == 'struct' and row['context_name'] == 'Counter'
                    and row['declaration_kind'] == 'initializer'):
                row.pop('name')
        with self.assertRaisesRegex(RuntimeError, 'missing declared method.*Counter.*init'):
            self.check(rows)

    def test_counts_must_match_actual_rows(self):
        with self.assertRaisesRegex(RuntimeError, 'aggregate counts'):
            self.check(self.reports(), lying_count=True)

    def test_compiler_projection_must_retain_evidence_and_separate_count(self):
        rows = self.reports()
        compiler = {'entry': '0x5000', 'mangled_symbol': '_$s4DemoMetadataF',
                    'declaration_kind': 'runtime', 'node_kind': 'TypeMetadataAccessFunction',
                    'classification': 'callable', 'status': 'recovered',
                    'source_representation': 'compiler-generated-from-type',
                    'compiler_projection_kind': 'type-metadata-accessor',
                    'compiler_projection_evidence': ['Native metadata identity and return shape verified.']}
        rows.append(compiler)
        self.assertEqual(self.check(rows)['compiler_projection_method_count'], 10)
        with self.assertRaisesRegex(RuntimeError, 'aggregate counts'):
            self.check(rows, counts={'compiler_projection_method_count': 0,
                                     'source_body_method_count': len(rows)})
        compiler.pop('compiler_projection_evidence')
        with self.assertRaisesRegex(RuntimeError, 'projection evidence'):
            self.check(rows)

    def test_ordinary_method_cannot_be_reported_as_compiler_projection(self):
        rows = self.reports()
        rows[0]['source_representation'] = 'compiler-generated-from-type'
        with self.assertRaisesRegex(RuntimeError, 'native body provenance'):
            self.check(rows)

    def test_oracle_has_fixed_behavior_count_and_wrapping_edges(self):
        expected = backend.expected_results()
        self.assertEqual(len(expected), 858)
        layout = {key: value for key, value in expected.items() if key.startswith('empty-')}
        self.assertEqual(layout, {'empty-size:0': 0, 'empty-alignment:0': 1, 'empty-stride:0': 1})
        self.assertEqual(len(expected) - len(layout), 855)
        self.assertEqual(expected['scalar:0'], 0)
        self.assertEqual(expected['struct-adjust:0'], 0)
        self.assertEqual(expected['pointerAfter:0'], 2**31 - 1)

    def test_empty_callables_retain_distinct_observed_provenance(self):
        manifest = json.loads(backend.FIXTURE.with_suffix('.callables.json').read_text())
        self.assertEqual(len(manifest['callables'][:-2]), 10)
        self.assertIn('Apple Swift 6.1.2', manifest['provenance'])
        self.assertIn('not direct execution', manifest['additional_expectation_provenance'])
        accessor, initializer = manifest['callables'][-2:]
        self.assertEqual(accessor['mangled_symbol'], backend.EMPTY_SYMBOL)
        self.assertEqual(accessor['provenance_kind'], 'compiled-original-nm-and-apple-demangle')
        self.assertEqual(initializer['mangled_symbol'], backend.EMPTY_INITIALIZER_SYMBOL)
        self.assertEqual(initializer['provenance_kind'], 'compiled-original-nm-pending-apple-role')

    def test_empty_accessor_address_cannot_be_substituted_in_both_reports(self):
        for symbol in self.empty_entries():
            with self.subTest(symbol=symbol):
                rows = self.reports()
                next(row for row in rows if row['mangled_symbol'] == symbol)['entry'] = '0x7000'
                with self.assertRaisesRegex(RuntimeError, 'Empty callable identity'):
                    self.check(rows)

    def test_empty_cannot_gain_an_ordinary_body_with_self_consistent_counts(self):
        rows = self.reports()
        rows.append({'entry': '0x7000', 'mangled_symbol': backend.EMPTY_PREFIX + '4fakeSiyF',
                     'module': backend.MODULE, 'context_kind': 'struct', 'context_name': 'Empty',
                     'name': 'fake', 'declaration_kind': 'function', 'node_kind': 'Function',
                     'classification': 'callable', 'status': 'recovered',
                     'source_representation': 'native-method-body'})
        with self.assertRaisesRegex(RuntimeError, 'no ordinary body'):
            self.check(rows)

    def test_empty_source_unit_requires_unique_exact_owner_and_identity(self):
        identities = [{'entry': hex(entry), 'mangled_symbol': symbol}
                      for symbol, entry in self.empty_entries().items()]
        identity = identities[0]
        original = {'kind': 'type', 'module': backend.MODULE, 'name': 'Empty',
                    'method_entries': [item['entry'] for item in identities], 'method_identities': identities}
        cases = [[], [original, original]]
        for key, value in (('kind', 'function'), ('module', 'Other'), ('name', 'Other'),
                           ('method_entries', ['0x7000']), ('method_identities', []),
                           ('method_identities', [identity, identity]),
                           ('method_identities', identities[:1]),
                           ('method_identities', identities[1:]),
                           ('method_identities', [{'entry': identity['entry'], 'mangled_symbol': 'substituted'}])):
            changed = deepcopy(original)
            changed[key] = value
            cases.append([changed])
        cases.append([original, {'kind': 'type', 'module': 'Other', 'name': 'Other',
                                 'method_entries': [identity['entry']], 'method_identities': [identity]}])
        for units in cases:
            with self.subTest(units=units), self.assertRaisesRegex(RuntimeError, 'exact nominal source unit'):
                self.check(self.reports(), source_units=units)

    def test_empty_layout_metadata_cannot_be_missing_or_changed(self):
        original = {'module': backend.MODULE, 'kind': 'struct', 'name': 'Empty',
                    'status': 'recovered', 'reason': '', 'size': 0, 'alignment': 1, 'fields': []}
        cases = [[], [original, original]]
        for key, value in (('module', 'Other'), ('kind', 'class'), ('status', 'unrecovered'),
                           ('reason', 'missing VWT'), ('size', 1), ('size', False),
                           ('alignment', 8), ('alignment', True),
                           ('fields', [{'name': 'fake'}])):
            changed = deepcopy(original)
            changed[key] = value
            cases.append([changed])
        for types in cases:
            with self.subTest(types=types), self.assertRaisesRegex(RuntimeError, 'exact recovered storage metadata'):
                self.check(self.reports(), types=types)

    def record_empty(self, work, nm, demangled='type metadata accessor for SwiftBehavior.Empty\n',
                     init_demangled='SwiftBehavior.Empty.init() -> SwiftBehavior.Empty\n',
                     locator=None, mutate_library=False):
        library = work / 'original.dylib'
        library.write_bytes(b'owned command-boundary input, not compiled Swift')
        calls = []
        compiler = str((work / 'test-tools' / 'swiftc').resolve())
        demangler = str((work / 'test-tools' / 'apple-swift-demangle').resolve())

        def tool(argv, **options):
            calls.append(argv)
            if argv == ['/usr/bin/nm', '-a', '-n', str(library)]:
                return nm
            if argv == ['/usr/bin/xcrun', '--find', 'swift-demangle']:
                return demangler + '\n' if locator is None else locator
            if argv == [demangler, '--compact', backend.EMPTY_SYMBOL]:
                return demangled
            if argv == [demangler, '--compact', backend.EMPTY_INITIALIZER_SYMBOL]:
                return init_demangled
            if argv == [compiler, '--version']:
                if mutate_library:
                    library.write_bytes(b'changed after the symbol and role evidence')
                return 'Mock Apple Swift version\n'
            raise AssertionError(f'Unexpected external command: {argv!r}')

        with patch.object(backend, 'run', side_effect=tool):
            entries = backend.record_empty_callables(library, compiler, work, 1)
        return entries, calls

    def test_compiled_empty_oracle_records_external_commands_and_actual_input_hash(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            text = self.empty_nm()
            entries, calls = self.record_empty(work, text)
            self.assertEqual(entries, self.empty_entries())
            self.assertEqual(len(calls), 5)
            self.assertEqual((work / 'original-symbols.txt').read_text(), text)
            receipt = json.loads((work / 'original-empty-callable.json').read_text())
            self.assertEqual(receipt['schema_version'], 2)
            self.assertEqual({row['mangled_symbol']: int(row['entry'], 16) for row in receipt['callables']},
                             self.empty_entries())
            self.assertEqual(len(receipt['callables']), 2)
            self.assertEqual([row['demangled'] for row in receipt['callables']],
                             ['type metadata accessor for SwiftBehavior.Empty',
                              'SwiftBehavior.Empty.init() -> SwiftBehavior.Empty'])
            self.assertEqual(receipt['library_sha256'],
                             backend.hashlib.sha256((work / 'original.dylib').read_bytes()).hexdigest())
            self.assertEqual(receipt['source_sha256'], backend.hashlib.sha256(backend.FIXTURE.read_bytes()).hexdigest())
            self.assertEqual(receipt['nm_argv'], calls[0])
            self.assertEqual([row['demangle_argv'] for row in receipt['callables']], calls[2:4])
            self.assertEqual(receipt['compiler'], calls[4][0])
            self.assertEqual((work / 'original-empty-demangle.txt').read_text(),
                             'type metadata accessor for SwiftBehavior.Empty\n')
            self.assertEqual((work / 'original-empty-initializer-demangle.txt').read_text(),
                             'SwiftBehavior.Empty.init() -> SwiftBehavior.Empty\n')

    def test_empty_oracle_rejects_missing_duplicate_undefined_and_nontext_symbols(self):
        good = self.empty_nm()
        for symbol, entry in self.empty_entries().items():
            line = f'{entry:016x} t {symbol}\n'
            for replacement in ('', line + line, f'                 U {symbol}\n',
                                line.replace(' t ', ' D '), f'not-hex t {symbol}\n',
                                f'0000000000000000 T {symbol}\n', f'10000000000000000 t {symbol}\n'):
                text = good.replace(line, replacement)
                with self.subTest(symbol=symbol, text=text), tempfile.TemporaryDirectory() as temporary:
                    work = Path(temporary)
                    with self.assertRaisesRegex(RuntimeError, 'Empty callable'):
                        self.record_empty(work, text)
                    self.assertEqual((work / 'original-symbols.txt').read_text(), text)
                    self.assertFalse((work / 'original-empty-callable.json').exists())

    def test_empty_oracle_rejects_independent_demangler_role_disagreement(self):
        for changed in ({'demangled': 'type metadata for SwiftBehavior.Empty\n'},
                        {'init_demangled': 'SwiftBehavior.Empty.other() -> SwiftBehavior.Empty\n'}):
            with self.subTest(changed=changed), tempfile.TemporaryDirectory() as temporary:
                work = Path(temporary)
                with self.assertRaisesRegex(RuntimeError, 'Apple demangler disagrees'):
                    self.record_empty(work, self.empty_nm(), **changed)
                self.assertTrue((work / 'original-empty-demangle.txt').is_file())
                if 'init_demangled' in changed:
                    self.assertTrue((work / 'original-empty-initializer-demangle.txt').is_file())
                self.assertFalse((work / 'original-empty-callable.json').exists())

    def test_empty_initializer_requires_its_exact_projection_and_evidence(self):
        for field, value, diagnostic in (
                ('source_representation', 'native-method-body', 'incorrect role'),
                ('compiler_projection_kind', 'type_metadata_accessor', 'incorrect role'),
                ('compiler_projection_evidence', [], 'projection evidence'),
                ('compiler_projection_evidence', [' '], 'projection evidence')):
            with self.subTest(field=field, value=value):
                rows = self.reports()
                next(row for row in rows if row['mangled_symbol'] == backend.EMPTY_INITIALIZER_SYMBOL)[field] = value
                with self.assertRaisesRegex(RuntimeError, diagnostic):
                    self.check(rows)
        with self.assertRaisesRegex(RuntimeError, 'aggregate counts'):
            self.check(self.reports(), counts={'source_body_method_count': 26,
                                               'compiler_projection_method_count': 8})

    def test_empty_callable_roles_remain_exact_in_each_report(self):
        for symbol in self.empty_entries():
            for label in ('coverage', 'signature inventory'):
                changes = [('module', 'Other'), ('node_kind', 'Function'),
                           ('context_kind', 'class'), ('context_name', 'Counter'),
                           ('name', 'other'), ('declaration_kind', 'function')]
                if symbol == backend.EMPTY_INITIALIZER_SYMBOL:
                    changes += [('parameters', [{'kind': 'int', 'name': 'Int'}]),
                                ('labels', ['value']), ('is_static', True), ('is_static', 0),
                                ('is_mutating', True), ('is_mutating', 0), ('is_mutating', 1),
                                ('is_mutating_known', False), ('is_mutating_known', 0), ('is_mutating_known', 1),
                                ('return_type', {'kind': 'void', 'name': 'Void'})]
                for field, value in changes:
                    with self.subTest(symbol=symbol, label=label, field=field):
                        rows = self.reports()
                        inventory = deepcopy(rows)
                        target = rows if label == 'coverage' else inventory
                        next(row for row in target if row['mangled_symbol'] == symbol)[field] = value
                        with self.assertRaisesRegex(RuntimeError, 'required fixture callable.*role'):
                            self.check(rows, inventory_rows=inventory)

    def test_empty_source_preserves_both_aliased_callable_identities(self):
        entries = {symbol: 0x6000 for symbol in self.empty_entries()}
        rows = self.reports()
        for row in rows:
            if row['mangled_symbol'] in entries:
                row['entry'] = '0x6000'
        coverage = self.check(rows, empty_entries=entries)
        unit = coverage['source_units'][0]
        self.assertEqual(unit['method_entries'], ['0x6000', '0x6000'])
        self.assertEqual(len(unit['method_identities']), 2)
        with tempfile.TemporaryDirectory() as temporary:
            actual, _ = self.record_empty(Path(temporary), self.empty_nm(entries))
            self.assertEqual(actual, entries)
        unit['method_entries'] = ['0x6000']
        with self.assertRaisesRegex(RuntimeError, 'exact nominal source unit'):
            self.check(rows, empty_entries=entries, source_units=[unit])

    def test_empty_source_cannot_gain_explicit_or_unreported_members(self):
        for source in ('struct `Empty` {\n  init() {}\n}\n',
                       'struct `Empty` {\n  func fake() {}\n}\n', '',
                       backend.EMPTY_SOURCE + backend.EMPTY_SOURCE,
                       backend.EMPTY_SOURCE + '\nextension Empty { init(unreported: Int) {} }\n',
                       backend.EMPTY_SOURCE + '\nextension `Empty` { func unreported() {} }\n',
                       backend.EMPTY_SOURCE + '\nextension SwiftBehavior.Empty { init(unreported: Int) {} }\n',
                       backend.EMPTY_SOURCE + '\nstruct Empty {}\n',
                       backend.EMPTY_SOURCE + '\nstruct /* alternate */ `Empty`\n{\n}\n'):
            with self.subTest(source=source), self.assertRaisesRegex(RuntimeError, 'canonical nominal-only source'):
                self.check(self.reports(), source_text=source)

    def test_empty_oracle_rejects_input_mutation_during_evidence(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            with self.assertRaisesRegex(RuntimeError, 'input changed during collection'):
                self.record_empty(work, self.empty_nm(), mutate_library=True)
            self.assertTrue((work / 'original-empty-initializer-demangle.txt').is_file())
            self.assertFalse((work / 'original-empty-callable.json').exists())

    def test_empty_oracle_rejects_invalid_locator_before_demangling(self):
        for locator in ('relative-demangler\n', '/rooted-without-drive\nsecond-line\n'):
            with self.subTest(locator=locator), tempfile.TemporaryDirectory() as temporary:
                work = Path(temporary)
                with self.assertRaisesRegex(RuntimeError, 'locator is invalid'):
                    self.record_empty(work, self.empty_nm(), locator=locator)
                self.assertFalse((work / 'original-empty-demangle.txt').exists())
                self.assertFalse((work / 'original-empty-callable.json').exists())


if __name__ == '__main__':
    unittest.main()
