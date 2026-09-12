"""CI-only mutations of the SuppressLint round-trip acceptance contract."""
import unittest

from scripts import mobile_android_class_identity as identity
from scripts import test_mobile_android_suppress_lint as runner
from scripts.tests.test_mobile_android_class_identity import class_bytes


def fixture(rebuilt=False):
    def annotation(values):
        return [("RuntimeInvisibleAnnotations", [(identity.SUPPRESS_LINT_TYPE,
                 [("value", "[", [("s", value) for value in values])])])]
    # The synthetic pool helper is UTF-8 only. Use its typed facts to retain
    # isolated surrogate/null data after parsing the ordinary fixture shape.
    methods = [("<init>", "(I)V", 1, True), ("plus", "(I)I", 1, True), ("twice", "(I)I", 9, True)]
    attrs = {"class": annotation(["ClassIssue", "", "ClassIssue"]),
             ("field", "value"): annotation([]),
             ("method", "<init>", "(I)V"): annotation(["ConstructorIssue"]),
             ("method", "plus", "(I)I"): annotation(["PrivateApi"]),
             ("method", "twice", "(I)I"): annotation([])}
    signatures = {}
    if rebuilt:
        methods.append(("__neverdThrow", identity.THROW_HELPER_PROTOTYPE, 0xA, True))
        signatures[("method", "__neverdThrow", identity.THROW_HELPER_PROTOTYPE)] = [identity.THROW_HELPER_SIGNATURE]
    row = identity.ClassFile(class_bytes(runner.OWNER, methods, fields=[("value", 1, None)],
                                         class_access=0x21, extra_attributes=attrs, signatures=signatures),
                             platform_suppress_lint=True).facts()
    row["methods"][runner.OWNER + "->plus(I)I"]["runtime_invisible_annotations"] = runner.annotation(
        ["PrivateApi", 'line\n"\\', "", "\0", "\ud800"])
    return {runner.OWNER: row}


class SuppressLintAcceptanceTests(unittest.TestCase):
    def test_exact_original_and_rebuilt_sites_agree(self):
        original = runner.declarations(fixture())
        self.assertEqual(len(original), 5)
        self.assertEqual(original, runner.declarations(fixture(True), rebuilt=True))

    def test_dropped_empty_annotation_or_changed_order_is_not_success(self):
        for owner in ("class", "field", "method"):
            classes = fixture()
            row = classes[runner.OWNER]
            if owner == "field":
                row = row["fields"][runner.OWNER + "->value:I"]
            elif owner == "method":
                row = row["methods"][runner.OWNER + "->plus(I)I"]
            with self.subTest(owner=owner):
                row["runtime_invisible_annotations"] = []
                with self.assertRaisesRegex(RuntimeError, "annotation changed"):
                    runner.declarations(classes)
        classes = fixture()
        value = classes[runner.OWNER]["methods"][runner.OWNER + "->plus(I)I"]
        value["runtime_invisible_annotations"][0]["elements"][0]["values"].reverse()
        with self.assertRaises(RuntimeError):
            runner.declarations(classes)

    def test_helper_annotation_and_missing_original_method_are_rejected(self):
        classes = fixture(True)
        helper = runner.OWNER + "->__neverdThrow" + identity.THROW_HELPER_PROTOTYPE
        classes[runner.OWNER]["methods"][helper]["runtime_invisible_annotations"] = runner.annotation([])
        with self.assertRaisesRegex(RuntimeError, "helper"):
            runner.declarations(classes, rebuilt=True)
        classes = fixture(True)
        del classes[runner.OWNER]["methods"][runner.OWNER + "->plus(I)I"]
        with self.assertRaisesRegex(RuntimeError, "inventory"):
            runner.declarations(classes, rebuilt=True)

    def test_annotation_identity_does_not_hide_changed_declaration_scope(self):
        mutations = (("major", 61, "Java 8"), ("minor", 65535, "Java 8"),
                     ("name", "Lfixture/Other;", "class declaration"),
                     ("signature", "<T:Ljava/lang/Object;>Ljava/lang/Object;", "class declaration"),
                     ("enclosing_method", {"owner": "Lfixture/Outer;", "name": "make",
                                           "prototype": "()V"}, "nested scope"),
                     ("inner_class", {"inner": runner.OWNER, "outer": "Lfixture/Outer;",
                                      "name": "LintFixture", "access": 9}, "nested scope"))
        for rebuilt in (False, True):
            for key, value, diagnostic in mutations:
                with self.subTest(rebuilt=rebuilt, key=key):
                    classes = fixture(rebuilt)
                    classes[runner.OWNER][key] = value
                    with self.assertRaisesRegex(RuntimeError, diagnostic):
                        runner.declarations(classes, rebuilt=rebuilt)
            with self.subTest(rebuilt=rebuilt, key="field_signature"):
                classes = fixture(rebuilt)
                classes[runner.OWNER]["fields"][runner.OWNER + "->value:I"]["signature"] = "TT;"
                with self.assertRaisesRegex(RuntimeError, "field changed"):
                    runner.declarations(classes, rebuilt=rebuilt)

    def test_partial_or_duplicate_behavior_output_cannot_pass(self):
        for text in ("", "field:0=0\n", "field:0=0\nfield:0=0\n", "field:0=1\n"):
            with self.subTest(text=text), self.assertRaises(RuntimeError):
                runner.behavior(text)


if __name__ == "__main__":
    unittest.main()
