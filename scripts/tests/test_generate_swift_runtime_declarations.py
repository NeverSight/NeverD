import unittest

from scripts.generate_swift_runtime_declarations import declarations, render


def record(name='swift_allocate', module='Swift', cc='C_CC',
           returns='RETURNS(RefCountedPtrTy)', args='ARGS(TypeMetadataPtrTy, SizeTy)',
           availability='AlwaysAvailable', attrs='ATTRS(NoUnwind)'):
    return (f'FUNCTION(Allocate, {module}, {name}, {cc}, {availability}, '
            f'{returns}, {args}, {attrs}, EFFECT(RuntimeEffect::Allocating), '
            'UNKNOWN_MEMEFFECTS)\n')


class SwiftRuntimeDeclarationTests(unittest.TestCase):
    def test_fixed_pointer_size_facts_are_deterministic(self):
        source = record() + record('swift_free', returns='RETURNS(VoidTy)',
                                    args='ARGS(RefCountedPtrTy, SizeTy, SizeTy)')
        facts = declarations(source)
        self.assertEqual(facts, {'swift_allocate': ('ppz', False, False),
                                 'swift_free': ('vpzz', False, False)})
        self.assertEqual(render(facts, '0' * 64),
                         render(declarations(source + source), '0' * 64))

    def test_unsupported_abi_representations_and_module_cannot_bind(self):
        cases = [dict(cc='SwiftTailCC'), dict(cc='SwiftDirectRR_CC'),
                 dict(module='objc2'), dict(module='stdlib'),
                 dict(availability='SwiftRuntime53'), dict(args='NO_ARGS'),
                 dict(args='ARGS(Int16Ty)'), dict(args='ARGS(ObjCBoolTy)'),
                 dict(args='ARGS(UnknownPtrTy)'), dict(args='ARGS(VoidTy)'),
                 dict(args='ARGS(TypeMetadataPtrTy->getPointerTo())'),
                 dict(returns='RETURNS(RefCountedPtrTy, OpaquePtrTy)'),
                 dict(returns='RETURNS(Int1Ty)'), dict(returns='RETURNS()'),
                 dict(args='ARGS(' + ','.join(['PtrTy'] * 17) + ')')]
        for case in cases:
            with self.subTest(case=case):
                self.assertEqual(declarations(record(**case)), {})
                self.assertEqual(declarations(record() + record(**case)), {})

    def test_c_integer_words_keep_exact_carriers(self):
        source = record(returns='RETURNS(Int32Ty)', args='ARGS(PtrTy, Int32Ty)')
        self.assertEqual(declarations(source), {'swift_allocate': ('upu', False, False)})
        self.assertEqual(declarations(source + record()), {})
        self.assertEqual(declarations(record() + source), {})
        self.assertEqual(declarations(record(cc='SwiftCC', returns='RETURNS(Int32Ty)')), {})
        self.assertEqual(declarations(record(cc='SwiftCC', args='ARGS(Int32Ty)')), {})

    def test_boolean_result_needs_explicit_zero_extension(self):
        source = record(returns='RETURNS(Int1Ty)', args='ARGS(PtrTy)',
                        attrs='ATTRS(ZExt, NoUnwind)')
        self.assertEqual(declarations(source), {'swift_allocate': ('bp', False, False)})
        for attrs in ('ATTRS(NoUnwind)', 'ATTRS(SExt)', 'ATTRS(SExt, ZExt)', 'NO_ATTRS'):
            invalid = record(returns='RETURNS(Int1Ty)', attrs=attrs)
            self.assertEqual(declarations(invalid), {})
            self.assertEqual(declarations(source + invalid), {})
            self.assertEqual(declarations(invalid + source), {})

    def test_return_extension_cannot_authenticate_boolean_parameters(self):
        for cc in ('C_CC', 'SwiftCC'):
            for attrs in ('ATTRS(NoUnwind)', 'ATTRS(ZExt)', 'ATTRS(SExt)'):
                self.assertEqual(declarations(record(cc=cc, args='ARGS(Int1Ty)', attrs=attrs)), {})

    def test_macro_bodies_comments_and_conditional_declarations_supply_no_facts(self):
        hidden = [f'/* {record()} */', f'// {record()}',
                  '#define WRAPPER() \\\n' + record(),
                  '#if TARGET_A\n' + record() + '#else\n' + record() + '#endif\n',
                  '#ifndef A\n#ifdef B\n' + record() + '#endif\n#endif\n']
        for source in hidden:
            with self.subTest(source=source):
                self.assertEqual(declarations(source), {})
                self.assertEqual(declarations(source + record('swift_visible')),
                                 {'swift_visible': ('ppz', False, False)})

    def test_conflicting_alternatives_veto_in_both_orders(self):
        changed = record(returns='RETURNS(VoidTy)')
        for source in (record() + changed, changed + record()):
            self.assertEqual(declarations(source), {})

    def test_only_explicit_void_noreturn_is_preserved(self):
        self.assertEqual(declarations(record(attrs='ATTRS(NoUnwind, NoReturn)')), {})
        self.assertEqual(declarations(record(returns='RETURNS(VoidTy)',
                                              attrs='ATTRS(NoUnwind, NoReturn)')),
                         {'swift_allocate': ('vpz', True, False)})

    def test_fixed_swift_signatures_keep_their_calling_convention(self):
        for availability in ('AlwaysAvailable', 'SignedDescriptorAvailability',
                             'GetTypesInAbstractMetadataStateAvailability'):
            source = record(cc='SwiftCC', availability=availability)
            self.assertEqual(declarations(source),
                             {'swift_allocate': ('ppz', False, True)})
            self.assertEqual(declarations(source + record()), {})
            self.assertEqual(declarations(record() + source), {})
        self.assertEqual(declarations(record(cc='SwiftCC', returns='RETURNS(VoidTy)',
                                              attrs='ATTRS(NoReturn)')),
                         {'swift_allocate': ('vpz', True, True)})

    def test_custom_parameter_abi_cannot_be_hidden_by_a_scalar_record(self):
        for cc in ('C_CC', 'SwiftCC'):
            source = record(name='swift_willThrow', cc=cc)
            self.assertEqual(declarations(source), {})
            self.assertEqual(declarations(source + record()),
                             {'swift_allocate': ('ppz', False, False)})

    def test_swift_attributes_availability_and_register_budget_are_closed(self):
        for change in [dict(attrs='ATTRS(SwiftSelf)'),
                       dict(attrs='ATTRS(SwiftError)'), dict(attrs='NO_ATTRS'),
                       dict(availability='NewAvailability'),
                       dict(availability='DifferentiationAvailability'),
                       dict(args='ARGS(' + ','.join(['PtrTy'] * 9) + ')'),
                       dict(returns='RETURNS(PtrTy, PtrTy, PtrTy)')]:
            with self.subTest(change=change):
                invalid = record(cc='SwiftCC', **change)
                self.assertEqual(declarations(invalid), {})
                self.assertEqual(declarations(record(cc='SwiftCC') + invalid), {})
        self.assertTrue(declarations(record(cc='SwiftCC',
                                            args='ARGS(' + ','.join(['PtrTy'] * 8) + ')')))

    def test_swift_record_results_preserve_declared_member_order(self):
        for returns, encoding in [('RETURNS(RefCountedPtrTy, OpaquePtrTy)', '(pp)'),
                                  ('RETURNS(PtrTy, SizeTy)', '(pz)'),
                                  ('RETURNS(SizeTy, PtrTy)', '(zp)'),
                                  ('RETURNS(SizeTy, SizeTy)', '(zz)'),
                                  ('RETURNS(TypeMetadataResponseTy)', '(pz)'),
                                  ('RETURNS(TypeMetadataDependencyTy)', '(pz)')]:
            source = record(cc='SwiftCC', returns=returns)
            self.assertEqual(declarations(source),
                             {'swift_allocate': (encoding + 'pz', False, True)})
            self.assertEqual(declarations(source + source), declarations(source))
            self.assertEqual(declarations(source + record(cc='SwiftCC')), {})
            self.assertEqual(declarations(record(cc='SwiftCC') + source), {})

    def test_named_result_layout_cannot_become_an_argument_or_c_result(self):
        for change in [dict(args='ARGS(TypeMetadataResponseTy)'),
                       dict(args='ARGS(TypeMetadataDependencyTy)'),
                       dict(returns='RETURNS(TypeMetadataResponseTy, PtrTy)'),
                       dict(returns='RETURNS(TypeMetadataDependencyTy, PtrTy)'),
                       dict(returns='RETURNS(UnknownResponseTy)'),
                       dict(returns='RETURNS(PtrTy, Int32Ty)'),
                       dict(returns='RETURNS(VoidTy, PtrTy)'),
                       dict(returns='RETURNS(PtrTy, Int1Ty)'),
                       dict(returns='RETURNS(PtrTy, PtrTy)', attrs='ATTRS(NoReturn)')]:
            for cc in ['C_CC', 'SwiftCC']:
                with self.subTest(cc=cc, change=change):
                    self.assertEqual(declarations(record(cc=cc, **change)), {})
        self.assertEqual(declarations(record(returns='RETURNS(TypeMetadataResponseTy)')), {})
        self.assertEqual(declarations(record(returns='RETURNS(TypeMetadataDependencyTy)')), {})

    def test_swift_generic_enum_callbacks_use_only_exact_int32_abis(self):
        getter = record(name='swift_getEnumTagSinglePayloadGeneric', cc='SwiftCC',
                        returns='RETURNS(Int32Ty)',
                        args='ARGS(OpaquePtrTy, Int32Ty, TypeMetadataPtrTy, '
                             'PtrTy)')
        setter = record(name='swift_storeEnumTagSinglePayloadGeneric', cc='SwiftCC',
                        returns='RETURNS(VoidTy)',
                        args='ARGS(OpaquePtrTy, Int32Ty, Int32Ty, '
                             'TypeMetadataPtrTy, PtrTy)')
        self.assertEqual(declarations(getter + setter), {
            'swift_getEnumTagSinglePayloadGeneric': ('upupp', False, True),
            'swift_storeEnumTagSinglePayloadGeneric': ('vpuupp', False, True),
        })
        for invalid in [
            getter.replace('Int32Ty, TypeMetadataPtrTy', 'SizeTy, TypeMetadataPtrTy'),
            getter.replace('TypeMetadataPtrTy, PtrTy', 'TypeMetadataPtrTy, Int8PtrTy'),
            getter.replace('SwiftCC', 'C_CC'),
            getter.replace('swift_getEnumTagSinglePayloadGeneric', 'swift_other'),
            setter.replace('RETURNS(VoidTy)', 'RETURNS(Int32Ty)'),
            setter.replace('Int32Ty, Int32Ty, TypeMetadataPtrTy',
                           'Int32Ty, SizeTy, TypeMetadataPtrTy'),
        ]:
            with self.subTest(invalid=invalid[:100]):
                self.assertEqual(declarations(invalid), {})

    def test_incomplete_or_exhausted_input_cannot_publish_partial_facts(self):
        for source in ('FUNCTION(', record() + '\n#if A', '#endif',
                       '#define X \\', record(args='ARGS(' * 20 + 'PtrTy' + ')' * 20),
                       record() + ' ' * (2 * 1024 * 1024), 'FUNCTION(A,B)'):
            with self.subTest(source=source[:60]), self.assertRaises(ValueError):
                declarations(source)


if __name__ == '__main__':
    unittest.main()
