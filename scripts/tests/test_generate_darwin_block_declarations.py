import json
from types import SimpleNamespace
import unittest

from scripts.generate_darwin_block_declarations import BlockDeclarations, block_lifetime, noescape_attribute, render


class DarwinBlockDeclarationTests(unittest.TestCase):
    def test_only_expanded_noescape_attributes_supply_lifetime_evidence(self):
        self.assertTrue(noescape_attribute('void (^block)(void) __attribute__((noescape))'))
        self.assertFalse(noescape_attribute('DISPATCH_NOESCAPE dispatch_block_t block'))
        self.assertFalse(noescape_attribute('__attribute__((annotate("__attribute__((noescape))")))'))

    def test_all_platforms_alternatives_and_exports_must_agree(self):
        fact = json.dumps(['v16^v0@?8', [[1, 'v8@?0', 'NonEscaping']]])
        profiles = [{'consume': {fact}}] * 4
        exports = [{'consume': {'/usr/lib/public.dylib'}}] * 2
        self.assertIn('{"consume", "v16^v0@?8", "v16^v0@?8", 1, "v8@?0",', render(profiles, exports, 'test', 'test'))
        for index in range(4):
            for bad in ({}, {'consume': {fact, ''}}, {'consume': {json.dumps(['v16^v0@?8', [[0, 'v8@?0', 'NonEscaping']]])}}):
                changed = list(profiles)
                changed[index] = bad
                self.assertNotIn('{"consume",', render(changed, exports, 'test', 'test'))
        self.assertNotIn('{"consume",', render(profiles, [{}, exports[0]], 'test', 'test'))

    def test_copy_lifetime_requires_an_exact_audited_contract(self):
        self.assertEqual(block_lifetime('dispatch_after', 'v24Q0@8@?16', 2,
                                        'v8@?0', ''), 'Copied')
        for parent, index, callback in [('v24Q0@8@?16', 1, 'v8@?0'),
                                        ('v24Q0@8@?16', 2, 'i8@?0'),
                                        ('v16@0@?8', 1, 'v8@?0')]:
            self.assertIsNone(block_lifetime('dispatch_after', parent, index,
                                              callback, ''))
        for name in ('dispatch_group_async', 'dispatch_group_notify'):
            self.assertEqual(block_lifetime(name, 'v24@0@8@?16', 2,
                                            'v8@?0', ''), 'Copied')
            self.assertIsNone(block_lifetime(name, 'v24@0@8@?16', 1,
                                              'v8@?0', ''))
            self.assertIsNone(block_lifetime(name, 'v24@0@8@?16', 2,
                                              'i8@?0', ''))
        self.assertEqual(block_lifetime('dispatch_source_set_event_handler',
                                        'v16@0@?8', 1, 'v8@?0', ''), 'Copied')
        self.assertIsNone(block_lifetime('dispatch_source_set_event_handler',
                                          'v16@0@?8', 0, 'v8@?0', ''))
        for name in ('dispatch_async', 'dispatch_barrier_async'):
            self.assertEqual(block_lifetime(name, 'v16@0@?8', 1, 'v8@?0', ''), 'Copied')
            for parent, index, callback in [('v16@0@?8', 0, 'v8@?0'),
                                            ('v16@0@?8', 1, 'i8@?0'),
                                            ('v24@0@?8Q16', 1, 'v8@?0')]:
                self.assertIsNone(block_lifetime(name, parent, index, callback, ''))
        self.assertIsNone(block_lifetime('unannotated', 'v16@0@?8', 1, 'v8@?0', ''))
        fact = json.dumps(['v16@0@?8', [[1, 'v8@?0', 'Copied']]])
        other = json.dumps(['v16@0@?8', [[1, 'v8@?0', 'NonEscaping']]])
        exports = [{'dispatch_async': {'/usr/lib/libSystem.B.dylib'}}] * 2
        profiles = [{'dispatch_async': {fact}}] * 4
        self.assertIn('Lifetime::Copied', render(profiles, exports, 'test', 'test'))
        for index in range(4):
            changed = list(profiles)
            changed[index] = {'dispatch_async': {other}}
            self.assertNotIn('{"dispatch_async",', render(changed, exports, 'test', 'test'))

    def test_callback_requires_a_fixed_c_block_prototype(self):
        clang = object.__new__(BlockDeclarations)
        function = SimpleNamespace(kind=111)
        clang.clang_getCanonicalType = lambda value: value
        clang.clang_getPointeeType = lambda value: function
        clang.clang_getNumArgTypes = lambda value: 0
        clang.clang_isFunctionTypeVariadic = lambda value: False
        clang.clang_getFunctionTypeCallingConv = lambda value: 1
        clang.clang_getResultType = lambda value: SimpleNamespace(kind=2)
        clang.clang_Type_getSizeOf = lambda value: 0
        block = SimpleNamespace(kind=102)
        self.assertEqual(clang.callback(block), 'v8@?0')
        self.assertIsNone(clang.callback(SimpleNamespace(kind=101)))
        clang.clang_isFunctionTypeVariadic = lambda value: True
        self.assertIsNone(clang.callback(block))
        clang.clang_isFunctionTypeVariadic = lambda value: False
        clang.clang_getFunctionTypeCallingConv = lambda value: 17
        self.assertIsNone(clang.callback(block))


if __name__ == '__main__':
    unittest.main()
