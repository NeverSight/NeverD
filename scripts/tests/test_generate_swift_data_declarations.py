import copy
import unittest

from scripts.generate_swift_data_declarations import metadata_storage, render


MODULE = '/usr/lib/swift/libswiftCore.dylib'
NAME = '$sSSN'
IR = ('@"$sSSN" = external global %swift.type, align 8\n'
      'define nonnull ptr @metadata_String() #0 {\nentry:\n'
      '  ret ptr @"$sSSN"\n}\n')


class SwiftDataDeclarationTests(unittest.TestCase):
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
