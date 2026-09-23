import copy
from pathlib import Path
import sys
import tempfile
import unittest

from scripts.generate_objc_receiver_declarations import (
    ReceiverDeclarations, common_methods, common_owner, common_parameter,
    method_profiles, owner_profiles, parameter_profiles, render,
)


def profile(name="Receiver", kind="class", category="", class_method=False,
            encoding="i16@0:8", parents=("NSObject",), protocols=(),
            return_class="", return_self=False, parameters=()):
    return {
        "owners": [(kind, name, category, parents, protocols)],
        "methods": [(kind, name, category, class_method, "value", encoding,
                     return_class, return_self, parameters)],
    }


class ObjCReceiverDeclarationTests(unittest.TestCase):
    def test_unrelated_owners_and_class_roles_keep_distinct_contracts(self):
        source = profile()
        for other in [profile(name="Other", encoding="@16@0:8"),
                      profile(class_method=True, encoding="d16@0:8"),
                      profile(kind="protocol", parents=(), encoding="q16@0:8")]:
            source["owners"] += other["owners"]
            source["methods"] += other["methods"]
        rows = method_profiles(source)
        self.assertEqual(len(rows), 4)
        self.assertEqual(common_methods(rows, rows, ("class", "Receiver", "", False, "value")),
                         [("i16@0:8", "", False)])
        self.assertEqual(common_methods(rows, rows, ("class", "Receiver", "", True, "value")),
                         [("d16@0:8", "", False)])

    def test_root_missing_and_conflicting_ancestry_are_distinct(self):
        identity = ("class", "Receiver", "")
        root = owner_profiles(profile(parents=()))
        inherited = owner_profiles(profile())
        self.assertEqual(common_owner(root, root, identity), "")
        self.assertIsNone(common_owner(root, {}, identity))
        self.assertEqual(common_owner(root, inherited, identity), "!")
        inherited[identity].add("C:Other")
        self.assertEqual(common_owner(inherited, inherited, identity), "!")

    def test_categories_preserve_the_class_and_adopted_protocol_scope(self):
        source = profile(kind="category", category="Extra", parents=(),
                         protocols=("Contract",))
        owners = owner_profiles(source)
        self.assertEqual(owners, {("category", "Receiver", "Extra"): {"P:Contract"}})
        self.assertIn(("category", "Receiver", "Extra", False, "value"),
                      method_profiles(source))

    def test_variadic_and_platform_conflicts_remain_negative_evidence(self):
        identity = ("class", "Receiver", "", False, "value")
        ordinary = method_profiles(profile())
        variadic = method_profiles(profile(encoding=""))
        floating = method_profiles(profile(encoding="d16@0:8"))
        self.assertEqual(common_methods(ordinary, {}, identity), [(None, "", False)])
        self.assertEqual(common_methods(ordinary, floating, identity), [("", "", False)])
        self.assertEqual(common_methods(variadic, variadic, identity), [("", "", False)])

    def test_object_type_disagreement_keeps_only_the_agreed_abi(self):
        identity = ("class", "Receiver", "", False, "value")
        first = method_profiles(profile(encoding="@16@0:8", return_class="First"))
        second = method_profiles(profile(encoding="@16@0:8", return_class="Second"))
        self.assertEqual(common_methods(first, first, identity), [("@16@0:8", "First", False)])
        self.assertEqual(common_methods(first, second, identity), [("@16@0:8", "", False)])
        instancetype = method_profiles(profile(encoding="@16@0:8", return_self=True))
        self.assertEqual(common_methods(instancetype, instancetype, identity),
                         [("@16@0:8", "", True)])

    def test_all_abi_alternatives_survive_deterministic_rendering(self):
        source = profile(encoding="q16@0:8")
        source["methods"] += profile(encoding="Q16@0:8")["methods"]
        reverse = copy.deepcopy(source)
        reverse["methods"].reverse()
        first = render([("F", "module", [source] * 4)], "test", "test")
        second = render([("F", "module", [reverse] * 4)], "test", "test")
        self.assertEqual(first, second)
        self.assertEqual(first.count("ND_OBJC_MEMBER("), 2)
        with self.assertRaises(ValueError):
            render([("F", "module", [source] * 3)], "test", "test")

    @unittest.skipUnless(sys.platform == "darwin", "Requires the macOS libclang runtime")
    def test_compiler_distinguishes_interfaces_forwards_and_category_owners(self):
        library = Path("/Library/Developer/CommandLineTools/usr/lib/libclang.dylib")
        if not library.is_file():
            self.skipTest("libclang is unavailable")
        with tempfile.TemporaryDirectory() as work:
            directory = Path(work)
            source = directory / "owners.m"
            source.write_text("""
@protocol Contract
- (long long)count;
@end
@class Parent, ForwardOnly;
__attribute__((objc_root_class))
@interface Parent <Contract>
+ (id)value;
- (int)value;
@end
@interface Child : Parent
@property(readonly) Child *peer;
@end
@interface Parent (Extra)
- (int)categoryValue;
@end
""")
            clang = ReceiverDeclarations(library, directory)
            for target in ("arm64-apple-macos15.0", "arm64-apple-ios18.0",
                           "x86_64-apple-macos15.0", "x86_64-apple-ios18.0-simulator"):
                facts = clang.extract_owned(source, directory, target)
                owners = owner_profiles(facts)
                self.assertEqual(owners[("class", "Parent", "")], {"P:Contract"})
                self.assertEqual(owners[("class", "Child", "")], {"C:Parent"})
                self.assertEqual(owners[("protocol", "Contract", "")], {""})
                self.assertEqual(owners[("category", "Parent", "Extra")], {""})
                self.assertNotIn(("class", "ForwardOnly", ""), owners)
                methods = method_profiles(facts)
                self.assertEqual(methods[("class", "Parent", "", False, "value")],
                                 {("i16@0:8", "", False, ())})
                self.assertEqual(methods[("class", "Parent", "", True, "value")],
                                 {("@16@0:8", "", False, ())})
                self.assertEqual(methods[("class", "Child", "", False, "peer")],
                                 {("@16@0:8", "Child", False, ())})

    def test_object_pointer_parameters_require_cross_profile_agreement(self):
        identity = ("class", "Receiver", "", False, "value", 2)
        error = parameter_profiles(profile(parameters=((2, "NSError"),)))
        other = parameter_profiles(profile(parameters=((2, "Other"),)))
        self.assertEqual(common_parameter(error, error, identity), "NSError")
        self.assertIsNone(common_parameter(error, other, identity))
        self.assertIsNone(common_parameter(error, {}, identity))
        rendered = render(
            [("F", "module", [profile(parameters=((2, "NSError"),))] * 4)],
            "test", "test")
        self.assertIn(
            'ND_OBJC_OUT_PARAMETER("F", "module", "class", "Receiver", "", false, "value", 2, "NSError", "NSError")',
            rendered)


if __name__ == "__main__":
    unittest.main()
