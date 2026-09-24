import copy
import unittest

from scripts.generate_swift_data_declarations import (conformance_storage,
                                                        HASHABLE_TYPES,
                                                        METADATA_TYPES,
                                                        metadata_storage,
                                                        render,
                                                        witness_storage)


MODULE = '/usr/lib/swift/libswiftCore.dylib'
NAME = '$sSSN'
IR = ('@"$sSSN" = external global %swift.type, align 8\n'
      'define nonnull ptr @metadata_String() #0 {\nentry:\n'
      '  ret ptr @"$sSSN"\n}\n')
ANY = '$sypN'
ANY_IR = ('@"$sypN" = external global %swift.full_existential_type\n'
          'define nonnull ptr @metadata_Any() #0 {\nentry:\n'
          '  ret ptr getelementptr inbounds (i8, ptr @"$sypN", i64 8)\n}\n')
WITNESS = '$sSSSHsWP'
WITNESS_IR = ('@"$sSSSHsWP" = external global ptr, align 8\n'
              'declare swiftcc void @neverd_hashable_probe(ptr noalias, ptr, ptr) local_unnamed_addr #0\n'
              'define void @witness_String() #0 {\nentry:\n'
              '  %0 = alloca %TSS, align 8\n'
              '  call swiftcc void @neverd_hashable_probe(ptr noalias nonnull %0, '
              'ptr nonnull @"$sSSN", ptr nonnull @"$sSSSHsWP") #2\n'
              '  ret void\n}\n')
CONFORMANCE = '$sSSSysMc'
CONFORMANCE_IR = (
    '@"$sSSN" = external global %swift.type, align 8\n'
    '@"$sS2SSysWL" = linkonce_odr hidden local_unnamed_addr global ptr null, align 8\n'
    '@"$sSSSysMc" = external global %swift.protocol_conformance_descriptor, align 4\n'
    'declare swiftcc void @neverd_string_protocol_probe(ptr noalias, ptr, ptr) local_unnamed_addr #0\n'
    'declare ptr @swift_getWitnessTable(ptr, ptr, ptr) local_unnamed_addr #1\n'
    'define void @witness_StringProtocol() #0 {\nentry:\n'
    '  %0 = alloca i64, align 8\n'
    '  %1 = tail call ptr @"$sS2SSysWl"() #2\n'
    '  call swiftcc void @neverd_string_protocol_probe(ptr noalias nonnull %0, '
    'ptr nonnull @"$sSSN", ptr %1) #3\n'
    '  ret void\n}\n'
    'define linkonce_odr hidden ptr @"$sS2SSysWl"() local_unnamed_addr #2 {\nentry:\n'
    '  %0 = load ptr, ptr @"$sS2SSysWL", align 8\n'
    '  %1 = icmp eq ptr %0, null\n'
    '  br i1 %1, label %cacheIsNull, label %cont\n'
    'cacheIsNull:\n'
    '  %2 = tail call ptr @swift_getWitnessTable(ptr nonnull @"$sSSSysMc", '
    'ptr nonnull @"$sSSN", ptr undef) #4\n'
    '  store atomic ptr %2, ptr @"$sS2SSysWL" release, align 8\n'
    '  br label %cont\n'
    'cont:\n'
    '  %3 = phi ptr [ %0, %entry ], [ %2, %cacheIsNull ]\n'
    '  ret ptr %3\n}\n')


class SwiftDataDeclarationTests(unittest.TestCase):
    def test_metadata_only_types_do_not_invent_hashable_witnesses(self):
        self.assertIn('Any', METADATA_TYPES)
        self.assertNotIn('Any', HASHABLE_TYPES)
        self.assertIn('Substring', METADATA_TYPES)
        self.assertNotIn('Substring', HASHABLE_TYPES)
        self.assertEqual(set(METADATA_TYPES) - set(HASHABLE_TYPES),
                         {'Any', 'Substring'})
        self.assertEqual(metadata_storage(ANY_IR, ['metadata_Any']), {ANY})
        for invalid in [
            ANY_IR.replace('external global', 'external thread_local global'),
            ANY_IR.replace('%swift.full_existential_type', '%swift.type'),
            ANY_IR.replace('i64 8', 'i64 0'),
            ANY_IR.replace('getelementptr inbounds', 'getelementptr'),
        ]:
            with self.subTest(invalid=invalid):
                self.assertEqual(metadata_storage(
                    invalid, ['metadata_Any']), set())

    def test_only_lazy_witness_accessors_supply_conformance_identity(self):
        self.assertEqual(conformance_storage(
            CONFORMANCE_IR, ['witness_StringProtocol'], {NAME}),
            {CONFORMANCE})
        for invalid in [
            CONFORMANCE_IR.replace('external global %swift.protocol_conformance_descriptor',
                                   'external thread_local global %swift.protocol_conformance_descriptor'),
            CONFORMANCE_IR.replace('external global %swift.protocol_conformance_descriptor',
                                   'global %swift.protocol_conformance_descriptor'),
            CONFORMANCE_IR.replace('%swift.protocol_conformance_descriptor', 'ptr'),
            CONFORMANCE_IR.replace('align 4', 'align 8'),
            CONFORMANCE_IR.replace('ptr nonnull @"$sSSSysMc"', 'ptr %descriptor'),
            CONFORMANCE_IR.replace('ptr nonnull @"$sSSN"', 'ptr nonnull @"unknown"'),
            CONFORMANCE_IR.replace('ptr undef)', 'ptr null)'),
            CONFORMANCE_IR.replace('store atomic ptr %2', 'store ptr %2'),
            CONFORMANCE_IR.replace(' release, align 8', ', align 8'),
            CONFORMANCE_IR.replace('ptr %1) #3', 'ptr %other) #3'),
            '@"$sSSSysMc" = external global '
            '%swift.protocol_conformance_descriptor, align 4\n' + CONFORMANCE_IR,
        ]:
            with self.subTest(invalid=invalid):
                self.assertEqual(conformance_storage(
                    invalid, ['witness_StringProtocol'], {NAME}), set())

    def test_conformance_probe_requires_unique_definition_and_exact_abis(self):
        for invalid in [
            '', CONFORMANCE_IR + CONFORMANCE_IR,
            ' ' * (16 * 1024 * 1024 + 1),
            CONFORMANCE_IR.replace('declare ptr @swift_getWitnessTable',
                                   'declare i64 @swift_getWitnessTable'),
            CONFORMANCE_IR.replace('declare swiftcc void '
                                   '@neverd_string_protocol_probe',
                                   'declare void @neverd_string_protocol_probe'),
            CONFORMANCE_IR.replace('@witness_StringProtocol()',
                                   '@another_probe()'),
        ]:
            with self.subTest(invalid=invalid[:80]), self.assertRaises(ValueError):
                conformance_storage(invalid, ['witness_StringProtocol'], {NAME})
        with self.assertRaises(ValueError):
            conformance_storage(CONFORMANCE_IR, ['witness.*'], {NAME})

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
