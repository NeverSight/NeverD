"""Independent class-file scope and complete-method oracle mutations (CI only)."""
from __future__ import annotations

import copy
import struct
import unittest

from scripts import mobile_android_class_identity as oracle


OUTER = "Lfixture/LocalClassBehavior;"


def u2(value):
    return struct.pack(">H", value)


def u4(value):
    return struct.pack(">I", value)


def class_bytes(owner, methods, *, enclosing=None, inner_name="Worker", inner_outer=None,
                inner_access=0x10, duplicate_inner=False, wrong_enclosing_tag=False, fields=(),
                signatures=None, field_descriptors=None, wrong_signature_tag=False):
    """Build bounded JVM attributes with real typed constant-pool references."""
    pool, cache = [], {}

    def entry(tag, payload):
        key = (tag, payload)
        if key not in cache:
            pool.append(bytes([tag]) + payload)
            cache[key] = len(pool)
        return cache[key]

    def utf(value):
        raw = value.encode("utf-8")
        return entry(1, u2(len(raw)) + raw)

    def cls(value):
        return entry(7, u2(utf(value[1:-1])))

    def attr(name, payload):
        return u2(utf(name)) + u4(len(payload)) + payload

    def signature_attrs(key):
        values = [] if signatures is None else signatures.get(key, [])
        return [attr("Signature", value if isinstance(value, bytes) else
                     u2(cls("Ljava/lang/Object;") if wrong_signature_tag else utf(value))) for value in values]

    this_class, superclass = cls(owner), cls("Ljava/lang/Object;")
    init_name_type = entry(12, u2(utf("<init>")) + u2(utf("()V")))
    super_init = entry(10, u2(superclass) + u2(init_name_type))
    field_rows = []
    for name, flags, constant in fields:
        attributes = [] if constant is None else [attr("ConstantValue", u2(entry(3, u4(constant & 0xffffffff))))]
        attributes += signature_attrs(("field", name))
        descriptor = "I" if field_descriptors is None else field_descriptors.get(name, "I")
        field_rows.append(u2(flags) + u2(utf(name)) + u2(utf(descriptor)) + u2(len(attributes)) + b"".join(attributes))
    members = []
    for name, prototype, flags, code in methods:
        attributes = []
        if code:
            if name == "<init>":
                instructions = b"\x2a\xb7" + u2(super_init) + b"\xb1"
            elif prototype.endswith("J"):
                instructions = b"\x09\xad"
            elif prototype.endswith("V"):
                instructions = b"\xb1"
            elif prototype.endswith(";") or prototype.rsplit(")", 1)[1].startswith("["):
                instructions = b"\x01\xb0"
            else:
                instructions = b"\x03\xac"
            payload = u2(2) + u2(8) + u4(len(instructions)) + instructions + u2(0) + u2(0)
            attributes.append(attr("Code", payload))
        attributes += signature_attrs(("method", name, prototype))
        members.append(u2(flags) + u2(utf(name)) + u2(utf(prototype))
                       + u2(len(attributes)) + b"".join(attributes))
    attributes = [attr("SourceFile", u2(utf("LocalClassBehavior.java")))] + signature_attrs("class")
    if enclosing:
        outer, name, prototype = enclosing
        name_type = entry(12, u2(utf(name)) + u2(utf(prototype)))
        if wrong_enclosing_tag:
            name_type = utf(name)
        attributes.append(attr("EnclosingMethod", u2(cls(outer)) + u2(name_type)))
        row = (u2(this_class) + u2(cls(inner_outer) if inner_outer else 0)
               + u2(utf(inner_name) if inner_name is not None else 0) + u2(inner_access))
        attributes.append(attr("InnerClasses", u2(2 if duplicate_inner else 1)
                               + row + (row if duplicate_inner else b"")))
    access = 0x30 if enclosing else 0x31
    return (b"\xca\xfe\xba\xbe" + u2(0) + u2(52) + u2(len(pool) + 1) + b"".join(pool)
            + u2(access) + u2(this_class) + u2(superclass) + u2(0) + u2(len(field_rows)) + b"".join(field_rows)
            + u2(len(members)) + b"".join(members) + u2(len(attributes)) + b"".join(attributes))


def inventory(names=("9", "2", "41")):
    outer_methods = [("<init>", "()V", 2, True), ("first", "(II)I", 9, True),
                     ("second", "(II)I", 9, True), ("first", "(J)J", 9, True)]
    records = {OUTER: oracle.ClassFile(class_bytes(OUTER, outer_methods)).facts()}
    for suffix, (name, prototype) in zip(names, (("first", "(II)I"), ("second", "(II)I"), ("first", "(J)J"))):
        owner = "Lfixture/LocalClassBehavior$" + suffix + "Worker;"
        methods = [("<init>", "()V", 0, True), ("apply", prototype, 0, True)]
        records[owner] = oracle.ClassFile(class_bytes(owner, methods, enclosing=(OUTER, name, prototype))).facts()
    return records


def bindings(classes, inputs):
    result = []
    for owner, facts in oracle.local_classes(classes).items():
        enclosing, name, prototype, source_name = oracle.local_key(facts)
        parameters, returns = oracle.prototype_parts(prototype)
        result.append({"class": owner, "input": inputs[owner], "source_name": source_name,
                       "source_unit": "fixture/LocalClassBehavior.java", "binding_kind": "named-method-local",
                       "binary_name_status": "unverified", "enclosing_method": {
                           "identity": enclosing + "->" + name + prototype, "owner": enclosing,
                           "name": name, "prototype": prototype, "parameters": parameters, "returns": returns}})
    return {"class_source_bindings": result}


class GenericCompilerIdentityTests(unittest.TestCase):
    def setUp(self):
        self.owner = "Lfixture/GenericFixture;"
        self.method = self.owner + "->identity(Ljava/lang/Object;)Ljava/lang/Object;"
        self.field = self.owner + "->value:Ljava/lang/Object;"
        descriptor = "(Ljava/lang/Object;)Ljava/lang/Object;"
        before = oracle.ClassFile(class_bytes(self.owner,
            [("<init>", "()V", 1, True), ("identity", descriptor, 9, True)],
            fields=[("value", 1, None)], field_descriptors={"value": "Ljava/lang/Object;"},
            signatures={"class": ["<T:Ljava/lang/Object;>Ljava/lang/Object;"],
                        ("field", "value"): ["TT;"],
                        ("method", "identity", descriptor): ["<U:Ljava/lang/Object;>(TU;)TU;"]})).facts()
        self.original = {self.owner: before}
        self.rebuilt = copy.deepcopy(self.original)
        self.helper = self.owner + "->__neverdThrow" + oracle.THROW_HELPER_PROTOTYPE
        self.rebuilt[self.owner]["methods"][self.helper] = {
            "name": "__neverdThrow", "prototype": oracle.THROW_HELPER_PROTOTYPE,
            "access": 10, "code": True, "signature": oracle.THROW_HELPER_SIGNATURE}

    def test_exact_signatures_and_only_the_real_auxiliary_method_pass(self):
        result = oracle.match_generic_recompiled(self.original, self.rebuilt)
        self.assertEqual(result["original_method_count"], 2)
        self.assertEqual(result["signature_count"], {"classes": 1, "fields": 1, "methods": 1})
        self.assertEqual(result["generated_helper_count"], 1)

    def test_erasure_or_wrong_binding_at_every_scope_is_not_equivalent(self):
        for scope in ("class", "field", "method"):
            for signature in (None, "Ljava/lang/Object;", "<T:Ljava/lang/Number;>(TT;)TT;"):
                rebuilt = copy.deepcopy(self.rebuilt)
                target = rebuilt[self.owner]
                if scope == "field": target = target["fields"][self.field]
                elif scope == "method": target = target["methods"][self.method]
                target["signature"] = signature
                with self.subTest(scope=scope, signature=signature), self.assertRaisesRegex(RuntimeError, "declaration changed|Signature changed"):
                    oracle.match_generic_recompiled(self.original, rebuilt)

    def test_complete_original_identity_cannot_be_replaced_by_bridge_or_helper(self):
        for mutation in ("missing", "bridge", "access", "no-code", "extra", "new-class"):
            rebuilt = copy.deepcopy(self.rebuilt)
            methods = rebuilt[self.owner]["methods"]
            if mutation == "missing": del methods[self.method]
            elif mutation == "bridge": methods[self.method]["access"] |= 0x40 | 0x1000
            elif mutation == "access": methods[self.method]["access"] = 1
            elif mutation == "no-code": methods[self.method]["code"] = False
            elif mutation == "new-class": rebuilt["Lfixture/Extra;"] = copy.deepcopy(rebuilt[self.owner])
            else: methods[self.owner + "->__neverdThrowExtra()V"] = dict(methods[self.helper], name="__neverdThrowExtra", prototype="()V")
            with self.subTest(mutation=mutation), self.assertRaises(RuntimeError):
                oracle.match_generic_recompiled(self.original, rebuilt)

    def test_helper_must_have_exact_staticness_signature_and_identity(self):
        for mutation in ("missing", "static", "signature", "code"):
            rebuilt = copy.deepcopy(self.rebuilt)
            methods = rebuilt[self.owner]["methods"]
            if mutation == "missing": del methods[self.helper]
            elif mutation == "static": methods[self.helper]["access"] = 2
            elif mutation == "signature": methods[self.helper]["signature"] = None
            else: methods[self.helper]["code"] = False
            with self.subTest(mutation=mutation), self.assertRaisesRegex(RuntimeError, "helper"):
                oracle.match_generic_recompiled(self.original, rebuilt)


class CompilerClassFileTests(unittest.TestCase):
    def test_signature_attributes_keep_class_field_and_method_scopes(self):
        owner = "Lfixture/GenericFixture;"
        descriptor = "(Ljava/lang/Object;)Ljava/lang/Object;"
        signatures = {"class": ["<T:Ljava/lang/Object;>Ljava/lang/Object;"],
                      ("field", "value"): ["TT;"],
                      ("method", "identity", descriptor): ["<U:Ljava/lang/Object;>(TU;)TU;"]}
        parsed = oracle.ClassFile(class_bytes(owner, [("identity", descriptor, 9, True)],
            fields=[("value", 1, None)], field_descriptors={"value": "Ljava/lang/Object;"},
            signatures=signatures))
        facts = parsed.facts()
        self.assertEqual(facts["signature"], signatures["class"][0])
        self.assertEqual(facts["fields"][owner + "->value:Ljava/lang/Object;"]["signature"], "TT;")
        self.assertEqual(facts["methods"][owner + "->identity" + descriptor]["signature"],
                         "<U:Ljava/lang/Object;>(TU;)TU;")
        self.assertEqual(parsed.inventory(), (owner, {owner + "->identity" + descriptor: "body"}))

    def test_signature_payloads_are_unique_exact_u2_utf8_references_at_all_scopes(self):
        descriptor = "(Ljava/lang/Object;)Ljava/lang/Object;"
        for scope in ("class", ("field", "value"), ("method", "identity", descriptor)):
            for values in (["TT;", "TT;"], [b""], [b"\0"], [b"\0\1\0"], [b"\xff\xff"], [""], ["T\0;"]):
                with self.subTest(scope=scope, values=values), self.assertRaises(RuntimeError):
                    oracle.ClassFile(class_bytes("Lfixture/GenericFixture;", [("identity", descriptor, 9, True)],
                        fields=[("value", 1, None)], field_descriptors={"value": "Ljava/lang/Object;"},
                        signatures={scope: values})).facts()
            with self.subTest(scope=scope, wrong_tag=True), self.assertRaisesRegex(RuntimeError, "constant reference"):
                oracle.ClassFile(class_bytes("Lfixture/GenericFixture;", [("identity", descriptor, 9, True)],
                    fields=[("value", 1, None)], field_descriptors={"value": "Ljava/lang/Object;"},
                    signatures={scope: ["TT;"]}, wrong_signature_tag=True)).facts()

    def test_missing_signature_is_explicit_and_does_not_change_erased_inventory(self):
        facts = oracle.ClassFile(class_bytes(OUTER, [("get", "()I", 9, True)], fields=[("count", 1, None)])).facts()
        self.assertIsNone(facts["signature"])
        self.assertIsNone(facts["fields"][OUTER + "->count:I"]["signature"])
        self.assertIsNone(facts["methods"][OUTER + "->get()I"]["signature"])

    def test_real_attributes_keep_exact_overload_and_code_access_roles(self):
        classes = inventory()
        self.assertEqual(len(classes), 4)
        self.assertEqual(len(oracle.projected_methods(classes)), 9)
        keys = {oracle.local_key(row) for row in oracle.local_classes(classes).values()}
        self.assertEqual(keys, {(OUTER, "first", "(II)I", "Worker"),
                                (OUTER, "second", "(II)I", "Worker"),
                                (OUTER, "first", "(J)J", "Worker")})
        self.assertEqual(classes[OUTER]["methods"][OUTER + "-><init>()V"]["access"], 2)

    def test_enclosing_method_must_reference_name_and_type(self):
        data = class_bytes("Lfixture/LocalClassBehavior$1Worker;", [("<init>", "()V", 0, True)],
                           enclosing=(OUTER, "first", "(II)I"), wrong_enclosing_tag=True)
        with self.assertRaisesRegex(RuntimeError, "constant reference"):
            oracle.ClassFile(data).facts()

    def test_truncated_and_duplicate_identity_attributes_fail(self):
        for duplicate in (False, True):
            data = class_bytes("Lfixture/LocalClassBehavior$1Worker;", [("<init>", "()V", 0, True)],
                               enclosing=(OUTER, "first", "(II)I"), duplicate_inner=duplicate)
            with self.subTest(duplicate=duplicate), self.assertRaisesRegex(RuntimeError, "Truncated|Duplicate"):
                oracle.ClassFile(data if duplicate else data[:-1]).facts()

    def test_anonymous_and_member_classes_cannot_claim_named_local_scope(self):
        for arguments in ({"inner_name": None}, {"inner_outer": OUTER}, {"inner_access": 0x18}):
            data = class_bytes("Lfixture/LocalClassBehavior$1Worker;", [("<init>", "()V", 0, True)],
                               enclosing=(OUTER, "first", "(II)I"), **arguments)
            with self.subTest(arguments=arguments), self.assertRaisesRegex(RuntimeError, "anonymous, a member"):
                oracle.local_key(oracle.ClassFile(data).facts())

    def test_missing_code_cannot_be_disguised_as_an_ordinary_body(self):
        data = class_bytes(OUTER, [("first", "(II)I", 9, False)])
        with self.assertRaisesRegex(RuntimeError, "Code/access"):
            oracle.ClassFile(data).facts()

    def test_duplicate_real_constructor_is_rejected_in_the_compiler_inventory(self):
        data = class_bytes(OUTER, [("<init>", "()V", 2, True), ("<init>", "()V", 2, True)])
        with self.assertRaisesRegex(RuntimeError, "duplicate method"):
            oracle.ClassFile(data).facts()

    def test_static_constantvalue_is_independent_initializer_evidence(self):
        facts = oracle.ClassFile(class_bytes(OUTER, [("<init>", "()V", 2, True)],
                                             fields=[("seed", 0x19, 42)])).facts()
        self.assertEqual(facts["fields"][OUTER + "->seed:I"]["constant_value"], {"tag": 3, "value": "0000002a"})
        self.assertNotIn(OUTER + "-><clinit>()V", facts["methods"])


class LocalProjectionOracleTests(unittest.TestCase):
    def setUp(self):
        self.original = inventory()
        self.rebuilt = inventory(("5", "7", "1"))
        self.inputs = {owner: "classes2.dex" if owner != OUTER else "classes.dex" for owner in self.original}

    def check(self, helpers=None):
        return oracle.match_recompiled(self.original, self.rebuilt, [] if helpers is None else helpers)

    def test_mapping_uses_scope_not_numbers_or_enumeration_order(self):
        self.rebuilt = dict(reversed(list(self.rebuilt.items())))
        result = self.check()
        mapping = {row["original"]: row["recompiled"] for row in result["classes"]}
        self.assertEqual(mapping["Lfixture/LocalClassBehavior$9Worker;"], "Lfixture/LocalClassBehavior$5Worker;")
        self.assertEqual(mapping["Lfixture/LocalClassBehavior$41Worker;"], "Lfixture/LocalClassBehavior$1Worker;")
        self.assertEqual(result["original_method_count"], 10)
        self.assertFalse(result["binary_identity_equivalence"])
        self.assertFalse(result["all_measured_binary_names_equal"])

    def test_even_equal_names_do_not_attest_unrestricted_binary_equivalence(self):
        self.rebuilt = copy.deepcopy(self.original)
        result = self.check()
        self.assertTrue(result["all_measured_binary_names_equal"])
        self.assertFalse(result["binary_identity_equivalence"])

    def test_duplicate_or_wrong_enclosing_overload_cannot_select_first_match(self):
        local = next(iter(oracle.local_classes(self.rebuilt).values()))
        for name, prototype, message in (("first", "(J)J", "Ambiguous"), ("absent", "(II)I", "Missing")):
            with self.subTest(name=name, prototype=prototype):
                local["enclosing_method"].update(name=name, prototype=prototype)
                with self.assertRaisesRegex(RuntimeError, message): self.check()

    def test_swapped_overload_attributes_do_not_hide_wrong_original_signatures(self):
        locals_ = list(oracle.local_classes(self.rebuilt).values())
        locals_[0]["enclosing_method"], locals_[2]["enclosing_method"] = locals_[2]["enclosing_method"], locals_[0]["enclosing_method"]
        with self.assertRaisesRegex(RuntimeError, "original method or Code/access"):
            self.check()

    def test_missing_constructor_or_captured_constructor_arguments_fail(self):
        owner, facts = next(iter(oracle.local_classes(self.original).items()))
        ctor = facts["methods"].pop(owner + "-><init>()V")
        with self.assertRaisesRegex(RuntimeError, "constructor is missing or captures"):
            self.check()
        facts["methods"][owner + "-><init>(I)V"] = dict(ctor, prototype="(I)V")
        with self.assertRaisesRegex(RuntimeError, "constructor is missing or captures"):
            self.check()

    def test_missing_original_method_and_changed_code_or_flags_fail(self):
        identity = OUTER + "->first(II)I"
        for mutation in ("missing", "code", "access"):
            self.rebuilt = inventory(("5", "7", "1"))
            if mutation == "missing": del self.rebuilt[OUTER]["methods"][identity]
            elif mutation == "code": self.rebuilt[OUTER]["methods"][identity]["code"] = False
            else: self.rebuilt[OUTER]["methods"][identity]["access"] = 1
            with self.subTest(mutation=mutation), self.assertRaisesRegex(RuntimeError, "original method or Code/access"):
                self.check()

    def test_new_capture_field_inner_flags_and_non_java8_output_fail(self):
        for mutation in ("field", "flags", "major"):
            self.rebuilt = inventory(("5", "7", "1"))
            owner, local = next(iter(oracle.local_classes(self.rebuilt).items()))
            if mutation == "field":
                local["fields"][owner + "->capture:I"] = {"name": "capture", "descriptor": "I", "access": 0x1010}
            elif mutation == "flags": local["inner_class"]["access"] = 0
            else: local["major"] = 61
            with self.subTest(mutation=mutation), self.assertRaisesRegex(RuntimeError, "capture storage|InnerClasses access|Java 8"):
                self.check()

    def test_every_extra_method_requires_exact_typed_helper_identity(self):
        name, prototype = "__neverdThrow", "(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
        original_owner = next(iter(oracle.local_classes(self.original)))
        target = next(iter(oracle.local_classes(self.rebuilt)))
        self.rebuilt[target]["methods"][target + "->" + name + prototype] = {
            "name": name, "prototype": prototype, "access": 2, "code": True}
        helper = {"class": original_owner, "name": name, "prototype": prototype, "static": False,
                  "source_unit": "fixture/LocalClassBehavior.java", "kind": "throw-helper"}
        self.assertEqual(self.check([helper])["generated_helper_count"], 1)
        with self.assertRaisesRegex(RuntimeError, "exact extra methods"): self.check()
        with self.assertRaisesRegex(RuntimeError, "duplicates"): self.check([helper, helper])
        with self.assertRaisesRegex(RuntimeError, "wrong ABI"): self.check([dict(helper, static=True)])
        with self.assertRaisesRegex(RuntimeError, "source unit"): self.check([dict(helper, source_unit="sources/fixture/LocalClassBehavior.java")])

    def test_helper_prefix_cannot_exempt_an_unreported_method(self):
        self.rebuilt[OUTER]["methods"][OUTER + "->__neverdThrowExtra()I"] = {
            "name": "__neverdThrowExtra", "prototype": "()I", "access": 10, "code": True}
        with self.assertRaisesRegex(RuntimeError, "exact extra methods"):
            self.check()

    def test_generated_field_initializer_requires_original_constant_evidence_and_exact_access(self):
        field = {"name": "seed", "descriptor": "I", "access": 0x19,
                 "constant_value": {"tag": 3, "value": "0000002a"}}
        self.original[OUTER]["fields"][OUTER + "->seed:I"] = field
        self.rebuilt[OUTER]["fields"][OUTER + "->seed:I"] = dict(field, constant_value=None)
        initializer = {"name": "<clinit>", "prototype": "()V", "access": 8, "code": True}
        self.rebuilt[OUTER]["methods"][OUTER + "-><clinit>()V"] = initializer
        helper = {"class": OUTER, "name": "<clinit>", "prototype": "()V", "static": True,
                  "source_unit": "fixture/LocalClassBehavior.java", "kind": "field-initializer"}
        self.assertEqual(self.check([helper])["generated_helper_count"], 1)
        initializer["access"] = 10
        with self.assertRaisesRegex(RuntimeError, "Code/access role changed"): self.check([helper])
        initializer["access"] = 8
        field["constant_value"] = None
        with self.assertRaisesRegex(RuntimeError, "ConstantValue evidence"): self.check([helper])

    def test_field_initializer_cannot_reclassify_an_original_method(self):
        self.original[OUTER]["fields"][OUTER + "->seed:I"] = {
            "name": "seed", "descriptor": "I", "access": 0x19,
            "constant_value": {"tag": 3, "value": "0000002a"}}
        row = {"name": "<clinit>", "prototype": "()V", "access": 8, "code": True}
        self.original[OUTER]["methods"][OUTER + "-><clinit>()V"] = row
        self.rebuilt[OUTER]["methods"][OUTER + "-><clinit>()V"] = dict(row)
        helper = {"class": OUTER, "name": "<clinit>", "prototype": "()V", "static": True,
                  "source_unit": "fixture/LocalClassBehavior.java", "kind": "field-initializer"}
        with self.assertRaisesRegex(RuntimeError, "replaces an original initializer"):
            self.check([helper])

    def test_bindings_match_independent_input_and_exact_scope_without_guessed_name(self):
        coverage = bindings(self.original, self.inputs)
        oracle.validate_bindings(coverage, self.original, self.inputs)
        for mutation in ("scope", "input", "source", "duplicate", "guess", "verified"):
            altered = copy.deepcopy(coverage)
            row = altered["class_source_bindings"][0]
            if mutation == "scope": row["enclosing_method"]["prototype"] = "(J)J"
            elif mutation == "input": row["input"] = "classes.dex"
            elif mutation == "source": row["source_unit"] = "sources/fixture/LocalClassBehavior.java"
            elif mutation == "duplicate": altered["class_source_bindings"][1] = copy.deepcopy(row)
            elif mutation == "guess": row["generated_binary_name"] = row["class"]
            else: row["binary_name_status"] = "verified"
            with self.subTest(mutation=mutation), self.assertRaisesRegex(RuntimeError, "binding|scope"):
                oracle.validate_bindings(altered, self.original, self.inputs)


if __name__ == "__main__":
    unittest.main()
