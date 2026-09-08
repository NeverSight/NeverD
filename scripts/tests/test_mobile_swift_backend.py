"""The real Swift runner must reject missing or partial callable coverage."""
from __future__ import annotations

import json
import importlib.util
from pathlib import Path
import sys
import tempfile
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
    def reports(self):
        rows = []
        for index, key in enumerate(sorted(backend.DECLARATIONS)):
            kind, context, name, declaration = key
            rows.append({'entry': hex(0x1000 + index * 16), 'mangled_symbol': f'_$s4Demo{index}F',
                         'context_kind': kind, 'context_name': context, 'name': name,
                         'declaration_kind': declaration, 'classification': 'callable',
                         'node_kind': 'Function' if declaration == 'function' else 'Constructor' if kind == 'class' else 'Allocator',
                         'status': 'recovered', 'source_representation': 'native-method-body'})
        return rows

    def check(self, rows, *, original=None, lying_count=False, counts=None):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            (output / 'metadata').mkdir()
            recovered = sum(row['status'] == 'recovered' for row in rows)
            compiler = sum(row['status'] == 'recovered' and row.get('declaration_kind') == 'runtime' for row in rows)
            status = 'recovered' if recovered == len(rows) else 'partial'
            coverage = {'schema_version': 1, 'methods': rows, 'non_method_symbols': [],
                        'method_count': len(rows) + int(lying_count), 'recovered_method_count': recovered,
                        'unrecovered_method_count': len(rows) - recovered, 'symbol_count': len(rows),
                        'source_body_method_count': recovered - compiler, 'compiler_projection_method_count': compiler,
                        'unclassified_symbol_count': 0, 'status': status, 'coverage_status': status}
            coverage.update(counts or {})
            report = {'status': 'success', 'platform': 'ios', 'architecture': 'arm64', 'swift_method_recovery': coverage}
            (output / 'report.json').write_text(json.dumps(report))
            (output / 'metadata/swift-methods.json').write_text(json.dumps(coverage))
            (output / 'metadata/swift-signatures.json').write_text(json.dumps({'methods': rows, 'symbols': []}))
            nm = '\n'.join(row['mangled_symbol'] for row in (rows if original is None else original))
            with patch.object(backend, 'run', return_value=nm):
                return backend.validate_coverage(output, output / 'original', 'arm64')

    def test_complete_declared_and_callable_inventory_passes(self):
        self.assertEqual(self.check(self.reports())['method_count'], len(backend.DECLARATIONS))

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
            if row['context_kind'] == 'struct' and row['declaration_kind'] == 'initializer':
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
        self.assertEqual(self.check(rows)['compiler_projection_method_count'], 1)
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
        self.assertEqual(len(expected), 855)
        self.assertEqual(expected['scalar:0'], 0)
        self.assertEqual(expected['struct-adjust:0'], 0)
        self.assertEqual(expected['pointerAfter:0'], 2**31 - 1)


if __name__ == '__main__':
    unittest.main()
