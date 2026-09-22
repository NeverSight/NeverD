import copy
import unittest

from scripts.generate_swift_metadata_declarations import (
    common_nominals, metadata_calls, nominal_types, render,
)


IDENTITY = 's:10Foundation3URLV'
ACCESSOR = '$s10Foundation3URLVMa'
MODULE = '/System/Library/Frameworks/Foundation.framework/Foundation'


def graph():
    return {'module': {'name': 'Foundation'}, 'symbols': [{
        'kind': {'identifier': 'swift.struct'},
        'identifier': {'interfaceLanguage': 'swift', 'precise': IDENTITY},
        'pathComponents': ['URL'], 'accessLevel': 'public',
    }]}


def query(name=ACCESSOR):
    return ('%swift.metadata_response = type { ptr, i64 }\n'
            f'declare swiftcc %swift.metadata_response @"{name}"(i64) #0\n'
            f'  %0 = tail call swiftcc %swift.metadata_response @"{name}"(i64 0) #1\n')


class SwiftMetadataDeclarationTests(unittest.TestCase):
    def test_only_public_root_nongeneric_nominals_supply_identities(self):
        self.assertEqual(nominal_types(graph()), {'URL': IDENTITY})
        concurrency = graph()
        concurrency['module']['name'] = '_Concurrency'
        concurrency['symbols'][0]['identifier']['precise'] = 's:ScM'
        concurrency['symbols'][0]['pathComponents'] = ['MainActor']
        self.assertEqual(nominal_types(concurrency, '_Concurrency'),
                         {'MainActor': 's:ScM'})
        for key, value in [
            ('kind', {'identifier': 'swift.protocol'}),
            ('identifier', {'interfaceLanguage': 'objc', 'precise': IDENTITY}),
            ('identifier', {'interfaceLanguage': 'swift', 'precise': 'c:URL'}),
            ('pathComponents', ['Outer', 'URL']), ('pathComponents', ['bad.name']),
            ('accessLevel', 'internal'),
            ('swiftGenerics', {'parameters': [{'name': 'T'}]}),
        ]:
            with self.subTest(key=key, value=value):
                document = graph()
                document['symbols'][0][key] = value
                self.assertEqual(nominal_types(document), {})

    def test_wrong_or_ambiguous_graphs_cannot_publish_partial_facts(self):
        document = graph()
        alternative = copy.deepcopy(document['symbols'][0])
        alternative['identifier']['precise'] = 's:4Test3URLV'
        document['symbols'].append(alternative)
        for bad in ({}, {'module': {'name': 'Swift'}}, document,
                    {'module': {'name': 'Foundation'}, 'symbols': {}}):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                nominal_types(bad)

    def test_all_profiles_must_agree_on_nominal_identity(self):
        profiles = [nominal_types(graph()) for _ in range(4)]
        self.assertEqual(common_nominals(profiles), ['URL'])
        profiles[2]['URL'] = 's:4Test3URLV'
        self.assertEqual(common_nominals(profiles), [])
        profiles[2] = {}
        self.assertEqual(common_nominals(profiles), [])
        with self.assertRaises(ValueError):
            common_nominals(profiles[:3])

    def test_accessor_role_and_compiled_call_and_declaration_are_all_required(self):
        self.assertEqual(metadata_calls(query(), [IDENTITY]), {ACCESSOR})
        self.assertEqual(metadata_calls(query(), []), set())
        self.assertEqual(metadata_calls(query('$s4Test3URLVMa'), [IDENTITY]), set())
        self.assertEqual(metadata_calls(query(ACCESSOR + 'suffix'), [IDENTITY]), set())
        self.assertEqual(metadata_calls(query(), [IDENTITY + '\nMa']), set())
        self.assertEqual(metadata_calls(query() + query('$s4Test3URLVMa'), [IDENTITY]),
                         {ACCESSOR})

    def test_unsupported_abi_cannot_be_inferred_from_an_accessor_name(self):
        for invalid in [query().replace('swiftcc', 'ccc'),
                        query().replace('{ ptr, i64 }', '{ ptr, i32 }'),
                        query().replace('(i64) #0', '(i64, ptr) #0'),
                        query().replace('(i64) #0', '(i32) #0'),
                        query().replace('declare swiftcc', 'declare'),
                        query().split('  %0')[0],
                        query().replace('call swiftcc', 'call'),
                        query().replace('(i64 0)', '(i64 0, ptr null)')]:
            with self.subTest(invalid=invalid):
                self.assertEqual(metadata_calls(invalid, [IDENTITY]), set())
        with self.assertRaises(ValueError):
            metadata_calls(' ' * (16 * 1024 * 1024 + 1), [IDENTITY])

    def test_both_architectures_and_platforms_require_matching_sdk_exports(self):
        profiles = [{ACCESSOR} for _ in range(4)]
        exports = [{ACCESSOR: {MODULE}} for _ in range(4)]
        output = render(profiles, exports, 'test', 'compiler')
        self.assertIn('{"' + ACCESSOR + '",', output)
        self.assertEqual(output, render(profiles, exports, 'test', 'compiler'))
        for index in range(4):
            changed = copy.deepcopy(exports)
            changed[index][ACCESSOR] = {'/Other.framework/Other'}
            self.assertNotIn('{"' + ACCESSOR + '",',
                             render(profiles, changed, 'test', 'compiler'))
            changed = copy.deepcopy(profiles)
            changed[index] = set()
            self.assertNotIn('{"' + ACCESSOR + '",',
                             render(changed, exports, 'test', 'compiler'))
        with self.assertRaises(ValueError):
            render(profiles[:3], exports, 'test', 'compiler')
        with self.assertRaises(ValueError):
            render(profiles, exports[:3], 'test', 'compiler')


if __name__ == '__main__':
    unittest.main()
