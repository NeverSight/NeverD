"""Objective-C instance declarations and distinct category implementations."""
from __future__ import annotations

import copy
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from scripts.tests.test_mobile_objc_source import fixture
from mobile.common import MobileError
from mobile.macho import objc_header, objc_ivar_layout
from mobile.objc_source import render_objc_sources


def layouts():
    # These offsets come from the self-owned ObjCCalls.m arm64/x86_64 corpus.
    base = {"name": "NDCallBase", "superclass": None, "root_class": True,
            "instance_start": 0, "instance_size": 32, "ivar_status": "recovered", "methods": [],
            "ivars": [{"name": "isa", "type_encoding": "#", "offset": 0, "size": 8, "alignment": 8},
                      {"name": "_bias", "type_encoding": "i", "offset": 8, "size": 4, "alignment": 4},
                      {"name": "_wide", "type_encoding": "q", "offset": 16, "size": 8, "alignment": 8},
                      {"name": "_peer", "type_encoding": "@", "offset": 24, "size": 8, "alignment": 8}]}
    child = {"name": "NDCallChild", "superclass": "NDCallBase", "root_class": False,
             "instance_start": 32, "instance_size": 36, "ivar_status": "recovered", "methods": [],
             "ivars": [{"name": "_extra", "type_encoding": "i", "offset": 32, "size": 4, "alignment": 4}]}
    return {"classes": [child, base], "pointer_size": 8}


def category_fixture(owner="Calculator", local=True):
    report, metadata = fixture()
    native = report["methods"][0]
    native.update(class_name=owner, category_name="NDRecoveredExtras", category_address="0x3000")
    runtime = metadata["classes"][0]["methods"][0]
    runtime.update(category_name="NDRecoveredExtras", category_address="0x3000")
    metadata["classes"][0]["name"] = owner
    metadata["categories"] = [{"name": "NDRecoveredExtras", "class_name": owner,
                               "address": "0x3000", "methods": [copy.deepcopy(runtime)]}]
    if not local:
        metadata["classes"] = []
    return report, metadata


class ObjCLayoutTests(unittest.TestCase):
    def test_known_root_and_subclass_fields_preserve_gap_and_superclass(self):
        source = objc_header(layouts())
        self.assertIn("__attribute__((objc_root_class))", source)
        self.assertIn("@interface NDCallChild : NDCallBase", source)
        self.assertLess(source.index("@interface NDCallBase"), source.index("@interface NDCallChild"))
        self.assertIn("Class isa;", source)
        self.assertIn("int _bias;", source)
        self.assertIn("unsigned char neverd_objc_padding_c[4];", source)
        self.assertIn("long long _wide;", source)
        self.assertIn("id _peer;", source)
        self.assertIn("int _extra;", source)

    def test_external_nsobject_identity_remains_the_superclass(self):
        metadata = {"classes": [{"name": "Child", "superclass": "NSObject", "root_class": False,
                                 "methods": [], "ivar_status": "recovered", "ivars": [],
                                 "instance_start": 8, "instance_size": 8}]}
        source = objc_header(metadata)
        self.assertIn("@interface Child : NSObject", source)
        self.assertNotIn("inheritance is omitted", source)

    def test_bad_type_overlap_alignment_inheritance_and_identifiers_are_rejected(self):
        for mutation in ("width", "alignment", "overlap", "start", "name", "type", "shadow", "cycle", "ancestor"):
            metadata = layouts()
            child, base = metadata["classes"]
            if mutation == "width": base["ivars"][1]["size"] = 8
            if mutation == "alignment": base["ivars"][1]["alignment"] = 8
            if mutation == "overlap": base["ivars"][1]["offset"] = 4
            if mutation == "start": child["instance_start"] = 24
            if mutation == "name": base["ivars"][1]["name"] = "x; int injected"
            if mutation == "type": base["ivars"][1]["type_encoding"] = "{Unknown=ii}"
            if mutation == "shadow": child["ivars"][0]["name"] = "_bias"
            if mutation == "cycle": base.update(superclass="NDCallChild", root_class=False, instance_start=36, instance_size=36, ivars=[])
            if mutation == "ancestor": base["superclass"] = []
            classes = {entry["name"]: entry for entry in metadata["classes"]}
            target = child if mutation in ("start", "shadow", "cycle", "ancestor") else base
            with self.subTest(mutation=mutation):
                declarations, error = objc_ivar_layout(target, classes)
                self.assertEqual(declarations, [])
                self.assertTrue(error)

    def test_external_unknown_superclass_cannot_back_an_ivar_layout(self):
        child = {"name": "Child", "superclass": "UnavailableParent", "root_class": False,
                 "ivar_status": "recovered", "ivars": [], "instance_start": 16, "instance_size": 16}
        declarations, error = objc_ivar_layout(child, {"Child": child})
        self.assertEqual(declarations, [])
        self.assertIn("Superclass", error)

    def test_layout_dependency_rejects_method_without_rejecting_unrelated_body(self):
        report, metadata = fixture()
        cls = metadata["classes"][0]
        cls.update(instance_start=0, instance_size=16, ivar_status="unresolved", ivars=[])
        report["methods"][0]["instance_layout_classes"] = ["Calculator"]
        source, coverage = render_objc_sources(report, metadata)
        self.assertEqual(coverage["status"], "unrecovered")
        self.assertIn("layout", coverage["methods"][0]["reason"])
        self.assertNotIn("@implementation", source)
        report["methods"][0]["instance_layout_classes"] = []
        _, coverage = render_objc_sources(report, metadata)
        self.assertEqual(coverage["status"], "recovered")

    def test_base_and_category_same_selector_have_distinct_implementations(self):
        report, metadata = fixture()
        base = report["methods"][0]
        category = copy.deepcopy(base)
        category.update(category_name="Extras", category_address="0x3000", implementation="0x1100",
                        function_name="neverd_objc_imp_1100")
        category["source"] = category["source"].replace("neverd_objc_imp_1000", "neverd_objc_imp_1100").replace("arg0 + arg1", "arg0 - arg1")
        report["methods"].append(category)
        metadata["classes"][0]["methods"].append({key: category[key] for key in
            ("selector", "class_method", "implementation", "type_encoding", "category_name", "category_address")})
        source, coverage = render_objc_sources(report, metadata)
        self.assertEqual(coverage["recovered_method_count"], 2, coverage)
        self.assertIn("@interface Calculator (Extras)", source)
        self.assertIn("@implementation Calculator\n", source)
        self.assertIn("@implementation Calculator (Extras)\n", source)
        self.assertIn("return arg0 + arg1;", source.split("@implementation Calculator\n", 1)[1].split("@end", 1)[0])
        self.assertIn("return arg0 - arg1;", source.split("@implementation Calculator (Extras)\n", 1)[1])
        self.assertEqual({row["category_name"] for row in coverage["methods"]}, {"", "Extras"})

    def test_mismatched_or_duplicate_category_identity_is_not_merged(self):
        report, metadata = fixture()
        report["methods"][0].update(category_name="Extras", category_address="0x3000")
        _, coverage = render_objc_sources(report, metadata)
        self.assertEqual(coverage["recovered_method_count"], 0)
        self.assertEqual(coverage["method_count"], 2)
        runtime = metadata["classes"][0]["methods"][0]
        runtime.update(category_name="Extras", category_address="0x3000")
        other = dict(runtime, implementation="0x1100", category_address="0x4000")
        metadata["classes"][0]["methods"].append(other)
        _, coverage = render_objc_sources(report, metadata)
        self.assertEqual(coverage["recovered_method_count"], 0)
        self.assertTrue(all("category" in row["reason"] for row in coverage["methods"]))

    def test_category_inventory_and_class_inventory_share_exactly_one_method(self):
        report, metadata = category_fixture()
        source, coverage = render_objc_sources(report, metadata)
        self.assertEqual(coverage["method_count"], 1)
        self.assertEqual(coverage["recovered_method_count"], 1, coverage)
        self.assertEqual(source.count("@implementation Calculator (NDRecoveredExtras)"), 1)
        self.assertEqual(source.count("@interface Calculator (NDRecoveredExtras)"), 1)
        # The merger must not mutate the caller's inventory.
        self.assertEqual(len(metadata["classes"][0]["methods"]), 1)
        metadata["classes"][0]["methods"] = []
        _, coverage = render_objc_sources(report, metadata)
        self.assertEqual(coverage["recovered_method_count"], 1, coverage)

    def test_disagreeing_duplicate_category_record_is_unrecovered(self):
        report, metadata = category_fixture()
        metadata["categories"][0]["methods"][0]["implementation"] = "0x1010"
        source, coverage = render_objc_sources(report, metadata)
        self.assertEqual(coverage["method_count"], 1)
        self.assertEqual(coverage["recovered_method_count"], 0)
        self.assertIn("duplicate runtime", coverage["methods"][0]["reason"])
        self.assertNotIn("@implementation", source)

    def test_external_foundation_category_keeps_real_class_declaration(self):
        source, coverage = render_objc_sources(*category_fixture("NSObject", local=False))
        self.assertEqual(coverage["recovered_method_count"], 1, coverage)
        self.assertIn("@class NSObject;", source)
        self.assertIn("@interface NSObject (NDRecoveredExtras)", source)
        self.assertIn("@implementation NSObject (NDRecoveredExtras)", source)
        self.assertNotIn("@interface NSObject\n", source)
        self.assertNotIn("objc_root_class", source)

    def test_external_unknown_category_has_explicit_missing_header_coverage(self):
        source, coverage = render_objc_sources(*category_fixture("UnavailableOwner", local=False))
        self.assertEqual(coverage["recovered_method_count"], 0)
        self.assertEqual(coverage["method_count"], 1)
        self.assertIn("unavailable class declaration", coverage["methods"][0]["reason"])
        self.assertIn("@class UnavailableOwner;", source)
        self.assertNotIn("@interface UnavailableOwner", source)
        self.assertNotIn("@implementation", source)

    def test_category_owner_and_method_identity_must_agree(self):
        for mutation in ("category_name", "category_address", "class_method"):
            report, metadata = category_fixture()
            metadata["categories"][0]["methods"][0][mutation] = "wrong"
            with self.subTest(mutation=mutation), self.assertRaisesRegex(MobileError, "identity"):
                render_objc_sources(report, metadata)

    @unittest.skipUnless(sys.platform == "darwin" and shutil.which("clang"), "requires Apple Clang and runtime")
    def test_external_foundation_category_compiles_and_executes(self):
        source, coverage = render_objc_sources(*category_fixture("NSObject", local=False))
        self.assertEqual(coverage["recovered_method_count"], 1, coverage)
        source += "\nint main(void) { @autoreleasepool { NSObject *value = [NSObject new]; return [value add:17 to:-9] != 8; } }\n"
        with tempfile.TemporaryDirectory(prefix="neverd-objc-category-") as directory:
            root = Path(directory)
            path, binary = root / "category.m", root / "check"
            path.write_text(source)
            compiled = subprocess.run(["clang", "-fno-objc-arc", str(path), "-framework", "Foundation", "-o", str(binary)],
                                      text=True, capture_output=True, timeout=30)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            ran = subprocess.run([str(binary)], text=True, capture_output=True, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stderr)

    @unittest.skipUnless(sys.platform == "darwin" and shutil.which("clang"), "requires Apple Clang and runtime")
    def test_compiled_root_subclass_ivar_offsets_match_native_corpus(self):
        source = objc_header(layouts()) + r'''
#include <objc/runtime.h>
#include <string.h>
@implementation NDCallBase
@end
@implementation NDCallChild
@end
int main(void) {
    Class base = objc_getClass("NDCallBase");
    Class child = objc_getClass("NDCallChild");
    if (!base || !child || class_getSuperclass(child) != base) return 1;
    const char *names[] = {"isa", "_bias", "_wide", "_peer", "_extra"};
    const ptrdiff_t offsets[] = {0, 8, 16, 24, 32};
    for (unsigned i = 0; i != 5; ++i) {
        Ivar field = class_getInstanceVariable(i == 4 ? child : base, names[i]);
        if (!field || ivar_getOffset(field) != offsets[i]) return 2;
    }
    if (class_getInstanceSize(base) < 32 || class_getInstanceSize(child) < 36) return 3;
    id object = class_createInstance(child, 0);
    if (!object || object_getClass(object) != child) return 4;
    object_dispose(object);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="neverd-objc-layout-") as directory:
            root = Path(directory)
            path, binary = root / "layout.m", root / "check"
            path.write_text(source)
            compiled = subprocess.run(["clang", "-fno-objc-arc", str(path), "-framework", "Foundation", "-o", str(binary)],
                                      text=True, capture_output=True, timeout=30)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            ran = subprocess.run([str(binary)], text=True, capture_output=True, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stderr)


if __name__ == "__main__":
    unittest.main()
