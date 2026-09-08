"""Source declarations from real structured Swift demangler output."""
from __future__ import annotations

from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/neverd'))
from mobile.common import Limits, MobileError
from mobile.swift_signatures import parse_swift_signature, recover_swift_signatures

SYMBOL = '$s16RecoveredFixture6scalarys5Int64VAD_ADtF'
TREE = '''kind=Global
  kind=Function
    kind=Module, text="RecoveredFixture"
    kind=Identifier, text="scalar"
    kind=LabelList
    kind=Type
      kind=FunctionType
        kind=ArgumentTuple
          kind=Type
            kind=Tuple
              kind=TupleElement
                kind=Type
                  kind=Structure
                    kind=Module, text="Swift"
                    kind=Identifier, text="Int64"
              kind=TupleElement
                kind=Type
                  kind=Structure
                    kind=Module, text="Swift"
                    kind=Identifier, text="Int64"
        kind=ReturnType
          kind=Type
            kind=Structure
              kind=Module, text="Swift"
              kind=Identifier, text="Int64"
'''


class SwiftSignatureTests(unittest.TestCase):
    def parse(self, tree=TREE, **kwargs):
        return parse_swift_signature(SYMBOL, '0x820', tree, **kwargs)

    def test_real_scalar_tree_keeps_unused_parameter_positions(self):
        row = self.parse()
        self.assertEqual(row['status'], 'supported')
        self.assertEqual(row['classification'], 'callable')
        self.assertEqual(row['name'], 'scalar')
        self.assertEqual(row['labels'], ['_', '_'])
        self.assertEqual([parameter['name'] for parameter in row['parameters']], ['arg0', 'arg1'])
        self.assertEqual(row['return_type'], {'kind': 'integer', 'name': 'Int64', 'bits': 64, 'signed': True})

    def test_class_self_is_separate_from_explicit_parameters(self):
        tree = TREE.replace('    kind=Module, text="RecoveredFixture"',
                            '    kind=Class\n      kind=Module, text="RecoveredFixture"\n      kind=Identifier, text="Calculator"')
        row = self.parse(tree)
        self.assertEqual(row['status'], 'supported')
        self.assertEqual(row['context_kind'], 'class')
        self.assertEqual(row['context_name'], 'Calculator')
        self.assertEqual(len(row['parameters']), 2)

    def test_struct_instance_does_not_guess_mutating_or_expanded_self(self):
        tree = TREE.replace('    kind=Module, text="RecoveredFixture"',
                            '    kind=Structure\n      kind=Module, text="RecoveredFixture"\n      kind=Identifier, text="Counter"')
        row = self.parse(tree)
        self.assertEqual(row['classification'], 'callable')
        self.assertEqual(row['status'], 'unsupported')
        self.assertFalse(row['is_mutating_known'])
        self.assertTrue(row['requires_self_abi_proof'])
        self.assertIn('mutating convention', row['reason'])

    def test_initializing_constructor_preserves_exact_class_return_identity(self):
        tree = '''kind=Global
  kind=Constructor
    kind=Class
      kind=Module, text="RecoveredFixture"
      kind=Identifier, text="Calculator"
    kind=LabelList
    kind=Type
      kind=FunctionType
        kind=ArgumentTuple
          kind=Type
            kind=Structure
              kind=Module, text="Swift"
              kind=Identifier, text="Int64"
        kind=ReturnType
          kind=Type
            kind=Class
              kind=Module, text="RecoveredFixture"
              kind=Identifier, text="Calculator"
'''
        row = self.parse(tree)
        self.assertEqual(row['status'], 'supported')
        self.assertEqual(row['declaration_kind'], 'initializer')
        self.assertEqual(row['name'], 'init')
        self.assertEqual(row['return_type'], {'kind': 'pointer', 'name': 'UnsafeMutableRawPointer'})
        bad = tree.rsplit('text="Calculator"', 1)
        self.assertEqual(self.parse('text="OtherClass"'.join(bad))['status'], 'unsupported')
        allocated = self.parse(tree.replace('kind=Constructor', 'kind=Allocator'))
        self.assertEqual(allocated['status'], 'unsupported')
        self.assertEqual(allocated['runtime_source_kind'], 'allocating_initializer')
        self.assertTrue(allocated['requires_runtime_source_proof'])
        self.assertEqual(allocated['parameters'], row['parameters'])
        self.assertEqual(allocated['return_type'], row['return_type'])
        value_tree = tree.replace('kind=Constructor', 'kind=Allocator').replace('kind=Class', 'kind=Structure')
        value_row = self.parse(value_tree)
        self.assertEqual(value_row['status'], 'unsupported')
        self.assertTrue(value_row['requires_storage_abi_proof'])
        self.assertEqual(value_row['context_kind'], 'struct')
        self.assertEqual(value_row['return_type'], {'kind': 'nominal', 'module': 'RecoveredFixture', 'name': 'Calculator', 'context_kind': 'struct'})
        self.assertNotIn('requires_self_abi_proof', value_row)

    def test_named_and_underscore_labels_remain_exact(self):
        tree = TREE.replace('    kind=LabelList', '    kind=LabelList\n      kind=FirstElementMarker\n      kind=Identifier, text="count"')
        self.assertEqual(self.parse(tree)['labels'], ['_', 'count'])
        tree = tree.replace('\n      kind=FirstElementMarker', '')
        self.assertEqual(self.parse(tree)['status'], 'unsupported')

    def test_signature_signedness_is_not_guessed_from_native_registers(self):
        row = self.parse(TREE.replace('text="Int64"', 'text="UInt32"'))
        self.assertEqual(row['return_type'], {'kind': 'integer', 'name': 'UInt32', 'bits': 32, 'signed': False})

    def test_aggregate_async_and_inout_signatures_are_explicitly_unsupported(self):
        cases = [TREE.replace('text="Int64"', 'text="String"'),
                 TREE.replace('      kind=FunctionType', '      kind=FunctionType\n        kind=AsyncAnnotation'),
                 TREE.replace('kind=Structure', 'kind=InOut')]
        for tree in cases:
            with self.subTest(tree=tree):
                row = self.parse(tree)
                self.assertEqual(row['status'], 'unsupported')
                self.assertEqual(row['classification'], 'callable')
                self.assertTrue(row['reason'])

    def test_metadata_descriptor_is_classified_separately(self):
        row = self.parse('kind=Global\n  kind=NominalTypeDescriptor\n    kind=Type\n      kind=Structure\n        kind=Module, text="Demo"\n        kind=Identifier, text="Counter"\n')
        self.assertEqual(row['classification'], 'metadata')
        self.assertEqual(row['node_kind'], 'NominalTypeDescriptor')

    def test_getter_is_callable_but_not_falsely_supported_as_function(self):
        row = self.parse('kind=Global\n  kind=Getter\n    kind=Variable\n      kind=Module, text="Demo"\n')
        self.assertEqual(row['classification'], 'callable')
        self.assertEqual(row['status'], 'unsupported')

    def test_scalar_accessors_keep_property_identity_and_exact_value_type(self):
        tree = '''kind=Global
  kind=Getter
    kind=Variable
      kind=Class
        kind=Module, text="SwiftBehavior"
        kind=Identifier, text="Calculator"
      kind=Identifier, text="bias"
      kind=Type
        kind=Structure
          kind=Module, text="Swift"
          kind=Identifier, text="Int64"
'''
        getter = self.parse(tree)
        self.assertEqual(getter['status'], 'supported')
        self.assertEqual(getter['declaration_kind'], 'getter')
        self.assertEqual(getter['name'], 'bias')
        self.assertEqual(getter['parameters'], [])
        self.assertEqual(getter['return_type']['name'], 'Int64')
        self.assertEqual(getter['context_name'], 'Calculator')
        setter = self.parse(tree.replace('kind=Getter', 'kind=Setter'))
        self.assertEqual(setter['status'], 'supported')
        self.assertEqual(setter['declaration_kind'], 'setter')
        self.assertEqual(setter['labels'], ['_'])
        self.assertEqual(setter['parameters'], [{'name': 'arg0', 'type': getter['return_type']}])
        self.assertEqual(setter['return_type'], {'kind': 'void', 'name': 'Void'})
        self.assertFalse(setter['is_mutating'])
        for node in ('Getter', 'Setter'):
            value = self.parse(tree.replace('kind=Getter', 'kind=' + node).replace('kind=Class', 'kind=Structure'))
            self.assertEqual(value['status'], 'unsupported')
            self.assertEqual(value['node_kind'], node)
            self.assertTrue(value['requires_self_abi_proof'])
            self.assertFalse(value['is_mutating_known'])

    def test_accessor_shape_and_unsupported_storage_are_not_guessed(self):
        tree = '''kind=Global
  kind=Getter
    kind=Variable
      kind=Class
        kind=Module, text="Demo"
        kind=Identifier, text="Box"
      kind=Identifier, text="value"
      kind=Type
        kind=Structure
          kind=Module, text="Swift"
          kind=Identifier, text="Double"
'''
        for malformed in (
                tree.replace('kind=Variable', 'kind=Subscript'),
                tree.replace('text="value"', 'text="bad.name"'),
                tree.replace('text="Double"', 'text="String"'),
                tree.replace('      kind=Type\n', '      kind=InOut\n'),
                tree.replace('  kind=Getter', '  kind=ModifyAccessor'),
                'kind=Global\n  kind=Static\n' + '\n'.join('  ' + line for line in tree.splitlines()[1:]),
                tree.replace('      kind=Class\n        kind=Module, text="Demo"\n        kind=Identifier, text="Box"',
                             '      kind=Module, text="Demo"')):
            with self.subTest(tree=malformed):
                row = self.parse(malformed)
                self.assertEqual(row['status'], 'unsupported')
                self.assertTrue(row['reason'])

    def test_coroutine_resume_symbol_remains_in_callable_inventory(self):
        row = self.parse('kind=Global\n  kind=ModifyAccessor\n    kind=Variable\n  kind=Suffix, text=".resume.0"\n')
        self.assertEqual(row['classification'], 'callable')
        self.assertEqual(row['node_kind'], 'CoroutineContinuation')
        self.assertEqual(row['continuation_of'], 'ModifyAccessor')
        self.assertEqual(row['status'], 'unsupported')

    def test_runtime_entries_keep_context_without_claiming_body_recovery(self):
        nominal = '    kind=Class\n      kind=Module, text="Demo"\n      kind=Identifier, text="Box"\n'
        for node, kind in (('Destructor', 'destructor'), ('Deallocator', 'deallocator'),
                           ('TypeMetadataAccessFunction', 'type_metadata_accessor')):
            tree = 'kind=Global\n  kind=' + node + '\n' + nominal
            if node == 'TypeMetadataAccessFunction':
                tree = 'kind=Global\n  kind=' + node + '\n    kind=Type\n' + ''.join('  ' + line + '\n' for line in nominal.splitlines())
            row = self.parse(tree)
            self.assertEqual(row['classification'], 'callable')
            self.assertEqual(row['status'], 'unsupported')
            self.assertEqual(row['declaration_kind'], 'runtime')
            self.assertEqual(row['runtime_source_kind'], kind)
            self.assertEqual((row['module'], row['context_kind'], row['context_name']), ('Demo', 'class', 'Box'))
            self.assertTrue(row['requires_runtime_source_proof'])
            self.assertNotIn('source', row)

    def test_modify_and_resume_share_property_identity_without_a_guessed_frame_abi(self):
        tree = '''kind=Global
  kind=ModifyAccessor
    kind=Variable
      kind=Structure
        kind=Module, text="Demo"
        kind=Identifier, text="Counter"
      kind=Identifier, text="value"
      kind=Type
        kind=Structure
          kind=Module, text="Swift"
          kind=Identifier, text="Int64"
'''
        modify = self.parse(tree)
        resume = self.parse(tree + '  kind=Suffix, text=".resume.0"\n')
        for row, kind in ((modify, 'modify_accessor'), (resume, 'modify_resume')):
            self.assertEqual(row['status'], 'unsupported')
            self.assertEqual(row['runtime_source_kind'], kind)
            self.assertEqual(row['context_name'], 'Counter')
            self.assertEqual(row['name'], 'value')
            self.assertEqual(row['property_type']['name'], 'Int64')
            self.assertTrue(row['requires_runtime_source_proof'])
        self.assertEqual(resume['compiler_suffix'], '.resume.0')
        for invalid in (tree.replace('text="Int64"', 'text="String"'),
                        tree.replace('text="value"', 'text="unsafe.name"'),
                        tree + '  kind=Suffix, text=".other.0"\n'):
            self.assertNotIn('requires_runtime_source_proof', self.parse(invalid))

    def test_malformed_or_unsafe_trees_have_inventory_reasons(self):
        for tree in (TREE + TREE, TREE.replace('kind=Function', ' kind=Function', 1),
                     TREE.replace('text="scalar"', 'text="bad;name"'), TREE.replace('text="scalar"', 'text="scalar", text="other"')):
            with self.subTest(tree=tree):
                row = self.parse(tree)
                self.assertEqual(row['status'], 'unsupported')
                self.assertTrue(row['reason'])

    def test_invalid_pointer_size_is_not_a_supported_abi(self):
        self.assertEqual(self.parse(pointer_size=4)['status'], 'unsupported')
        self.assertEqual(self.parse(pointer_size=True)['status'], 'unsupported')

    def test_process_contract_separates_data_from_method_denominator(self):
        data_symbol = '$s4Demo7CounterVMn'
        symbols = [{'name': SYMBOL, 'address': '0x820'}, {'name': data_symbol, 'address': '0x1000'}]
        def backend(argv, log, timeout):
            self.assertEqual(argv, ['swift-demangle', '--expand', '--tree-only', SYMBOL, data_symbol])
            log.write_text('Demangling for ' + SYMBOL + '\n' + TREE + '\nDemangling for ' + data_symbol + '\nkind=Global\n  kind=NominalTypeDescriptor\n')
        with tempfile.TemporaryDirectory() as directory, patch('mobile.swift_signatures.run_tool', side_effect=backend):
            report = recover_swift_signatures(symbols, demangler='swift-demangle', log_directory=Path(directory), limits=Limits())
        self.assertEqual(report['method_count'], 1)
        self.assertEqual(report['supported_signature_count'], 1)
        self.assertEqual(len(report['symbols']), 1)
        self.assertEqual(report['symbols'][0]['classification'], 'metadata')

    def test_backend_identity_mismatch_is_a_domain_error(self):
        def backend(argv, log, timeout):
            log.write_text('Demangling for different\n' + TREE)
        with tempfile.TemporaryDirectory() as directory, patch('mobile.swift_signatures.run_tool', side_effect=backend):
            with self.assertRaisesRegex(MobileError, 'identity'):
                recover_swift_signatures([{'name': SYMBOL, 'address': '0x820'}], demangler='swift-demangle', log_directory=Path(directory), limits=Limits())


if __name__ == '__main__':
    unittest.main()
