import copy
import unittest

from scripts.generate_swift_data_declarations import metadata_storage, render, witness_storage


MODULE = '/usr/lib/swift/libswiftCore.dylib'
NAME = '$sSSN'
IR = ('@"$sSSN" = external global %swift.type, align 8\n'
      'define nonnull ptr @metadata_String() #0 {\nentry:\n'
      '  ret ptr @"$sSSN"\n}\n')
WITNESS = '$sSSSHsWP'
WITNESS_IR = ('@"$sSSSHsWP" = external global ptr, align 8\n'
              'declare swiftcc void @neverd_hashable_probe(ptr noalias, ptr, ptr) local_unnamed_addr #0\n'
              'define void @witness_String() #0 {\nentry:\n'
              '  %0 = alloca %TSS, align 8\n'
              '  call swiftcc void @neverd_hashable_probe(ptr noalias nonnull %0, '
              'ptr nonnull @"$sSSN", ptr nonnull @"$sSSSHsWP") #2\n'
              '  ret void\n}\n')


class SwiftDataDeclarationTests(unittest.TestCase):
    def test_only_direct_witness_arguments_supply_storage_identity(self):
        self.assertEqual(witness_storage(WITNESS_IR, ['witness_String'], {NAME}), {WITNESS})
        for invalid in [
            WITNESS_IR.replace('external global', 'external thread_local global'),
            WITNESS_IR.replace('external global', 'global'),
            WITNESS_IR.replace('global ptr', 'global i64'),
            WITNESS_IR.replace('align 8', 'align 4'),
            WITNESS_IR.replace('ptr nonnull @"$sSSSHsWP"', 'ptr nonnull %loaded'),
            WITNESS_IR.replace('ptr nonnull @"$sSSSHsWP"',
                               'ptr nonnull getelementptr (i8, ptr @"$sSSSHsWP", i64 8)'),
            WITNESS_IR.replace('ptr nonnull @"$sSSN"', 'ptr nonnull @"unknown"'),
            WITNESS_IR.replace('call swiftcc', 'call'),
            WITNESS_IR.replace('call swiftcc', 'tail call swiftcc'),
            '@"$sSSSHsWP" = external global ptr, align 8\n' + WITNESS_IR,
        ]:
            with self.subTest(invalid=invalid):
                self.assertEqual(witness_storage(invalid, ['witness_String'], {NAME}), set())
        self.assertEqual(witness_storage(WITNESS_IR, ['witness_String'], set()), set())

    def test_witness_probe_requires_unique_definition_and_exact_generic_abi(self):
        for invalid in [
            '', WITNESS_IR + WITNESS_IR, ' ' * (16 * 1024 * 1024 + 1),
            WITNESS_IR.replace('declare swiftcc', 'declare'),
            WITNESS_IR.replace('(ptr noalias, ptr, ptr)', '(ptr noalias, ptr)'),
            WITNESS_IR.replace('@witness_String()', '@another_probe()'),
        ]:
            with self.subTest(invalid=invalid[:80]), self.assertRaises(ValueError):
                witness_storage(invalid, ['witness_String'], {NAME})
        with self.assertRaises(ValueError):
            witness_storage(WITNESS_IR, ['witness.*'], {NAME})

    def test_only_direct_non_tls_external_storage_queries_supply_facts(self):
        self.assertEqual(metadata_storage(IR, ['metadata_String']), {NAME})
        for invalid in [
            IR.replace('external global', 'external thread_local global'),
            IR.replace('external global', 'global'),
            IR.replace('%swift.type', 'ptr'),
            IR.replace('align 8', 'align 4'),
            IR.replace('ret ptr @"$sSSN"', 'ret ptr null'),
            IR.replace('ret ptr @"$sSSN"', '%x = call ptr @get()\n  ret ptr %x'),
            IR.replace('ret ptr @"$sSSN"',
                       'ret ptr getelementptr (i8, ptr @"$sSSN", i64 8)'),
            '@"$sSSN" = external global %swift.type, align 8\n' + IR,
        ]:
            with self.subTest(invalid=invalid):
                self.assertEqual(metadata_storage(invalid, ['metadata_String']), set())

    def test_missing_ambiguous_and_oversized_queries_fail(self):
        for invalid in ['', IR + IR, ' ' * (16 * 1024 * 1024 + 1)]:
            with self.subTest(size=len(invalid)), self.assertRaises(ValueError):
                metadata_storage(invalid, ['metadata_String'])
        with self.assertRaises(ValueError):
            metadata_storage(IR, ['metadata.*'])

    def test_every_compiler_and_export_profile_must_agree(self):
        profiles = [{NAME} for _ in range(4)]
        exports = [{NAME: {MODULE}} for _ in range(4)]
        self.assertIn('{"' + NAME + '",', render(profiles, exports, 'test', 'compiler'))
        for index in range(4):
            changed = copy.deepcopy(profiles)
            changed[index] = set()
            self.assertNotIn('{"' + NAME + '",',
                             render(changed, exports, 'test', 'compiler'))
            changed = copy.deepcopy(exports)
            changed[index][NAME] = {'/tmp/impostor.dylib'}
            self.assertNotIn('{"' + NAME + '",',
                             render(profiles, changed, 'test', 'compiler'))
        with self.assertRaises(ValueError):
            render(profiles[:3], exports, 'test', 'compiler')
        with self.assertRaises(ValueError):
            render(profiles, exports[:3], 'test', 'compiler')


if __name__ == '__main__':
    unittest.main()
