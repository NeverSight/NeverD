"""Compiler-backed qualification requires a distinct, role-correct entry for every original."""
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from scripts.verify_mobile_android_recovery import class_names, verify_methods


OWNER = "Lfixture/Owner;"
ENTRY = OWNER + "->first()I"
OTHER = OWNER + "->second()I"


def inputs():
    methods = {}
    rows = []
    compiled = {}
    for name, identity in (("first", ENTRY), ("second", OTHER)):
        declaration = {"identity": identity, "class": OWNER, "input": "classes.dex",
                       "prototype": "()I", "name": name}
        methods[identity] = {**declaration, "role": "body", "access_flags": 1, "code_units": 2}
        rows.append({**declaration, "status": "source-projected", "projection_kind": "virtual-method-identifier",
                     "reason": "Source identifier", "instruction_count": 2})
        compiled[identity] = {"access": 1, "code": True}
    coverage = {"status": "partial", "method_count": 2, "methods": rows, "projected_method_count": 2,
                "recovered_method_count": 0, "declaration_only_method_count": 0, "unrecovered_method_count": 0}
    inventory = {"classes": {OWNER: {}}, "methods": methods, "body_count": 2, "native_count": 0, "abstract_count": 0}
    return coverage, inventory, {OWNER: {"name": OWNER, "methods": compiled}}


class VerifiedRecoveryTests(unittest.TestCase):
    def verify(self, coverage, inventory, classes):
        return verify_methods(coverage, inventory, classes, {}, {OWNER: OWNER}, Path("."))

    def test_source_projection_remains_visible_after_verification(self):
        coverage, inventory, classes = inputs()
        result = self.verify(coverage, inventory, classes)
        self.assertEqual(result["distinct_compiled_methods"], 2)
        self.assertEqual(coverage["status"], "recovered")
        self.assertEqual(coverage["source_projected_method_count"], 2)
        self.assertEqual(coverage["recovered_method_count"], 2)
        self.assertEqual({r["compiled_identity"] for r in coverage["methods"]}, {ENTRY, OTHER})
        self.assertTrue(all(row["source_status"] == "source-projected" for row in coverage["methods"]))
        self.assertTrue(all(row["dex_access_flags"] == row["compiled_access_flags"] == 1
                            for row in coverage["methods"]))

    def test_two_originals_cannot_claim_one_body(self):
        coverage, inventory, classes = inputs()
        coverage["methods"][1]["source_identity"] = ENTRY
        with self.assertRaisesRegex(RuntimeError, "share one compiled entry"):
            self.verify(coverage, inventory, classes)

    def test_code_and_static_roles_must_come_from_compiler(self):
        for change in ({"code": False}, {"access": 9}):
            with self.subTest(change=change):
                coverage, inventory, classes = inputs()
                classes[OWNER]["methods"][ENTRY].update(change)
                with self.assertRaises(RuntimeError):
                    self.verify(coverage, inventory, classes)

    def test_projected_native_and_abstract_declarations_keep_their_distinct_roles(self):
        coverage, inventory, classes = inputs()
        inventory.update(body_count=0, native_count=1, abstract_count=1)
        for row, (role, access) in zip(coverage["methods"], (("native", 0x101), ("abstract", 0x401))):
            identity = row["identity"]
            row["instruction_count"] = 0
            inventory["methods"][identity].update(role=role, access_flags=access, code_units=0)
            classes[OWNER]["methods"][identity].update(access=access, code=False)
        classes[OWNER]["methods"][ENTRY]["access"] = 0x401
        with self.assertRaisesRegex(RuntimeError, "static/native/abstract role"):
            self.verify(coverage, inventory, classes)
        classes[OWNER]["methods"][ENTRY]["access"] = 0x101
        self.verify(coverage, inventory, classes)
        self.assertEqual((coverage["recovered_method_count"], coverage["declaration_only_method_count"]), (0, 2))
        self.assertTrue(all(row["status"] == "declaration-only" for row in coverage["methods"]))

    def test_regenerated_bridge_cannot_be_an_ordinary_method(self):
        coverage, inventory, classes = inputs()
        coverage["methods"][0]["compiler_generated_bridge"] = True
        with self.assertRaisesRegex(RuntimeError, "Expected a compiler-generated bridge"):
            self.verify(coverage, inventory, classes)

    def test_inherited_bridge_uses_the_generated_helpers_source_name(self):
        typed = "()Ljava/lang/String;"
        erased = "()Ljava/lang/Object;"
        original = OWNER + "->value" + typed
        source = OWNER + "->returnSlot" + typed
        generated = OWNER + "->value" + erased
        compiled_bridge = OWNER + "->returnSlot" + erased
        declaration = {"class": OWNER, "identity": original, "input": "classes.dex", "name": "value",
                       "prototype": typed}
        row = {**declaration, "source_identity": source, "generated_bridges": [generated], "instruction_count": 0,
               "status": "source-projected", "projection_kind": "incompatible-inherited-return-slot",
               "reason": "Source return slot alias"}
        helper = {"bridge_target": original, "class": OWNER, "name": "returnSlot", "prototype": erased,
                  "kind": "compiler-covariant-interface-bridge"}
        coverage = {"methods": [row], "method_count": 1, "status": "partial", "recovered_method_count": 0,
                    "declaration_only_method_count": 0, "projected_method_count": 1, "unrecovered_method_count": 0,
                    "generated_source_helpers": [helper]}
        inventory = {"methods": {original: {**declaration, "role": "abstract", "access_flags": 0x401,
                                            "code_units": 0}}, "body_count": 0, "native_count": 0, "abstract_count": 1}
        methods = {source: {"access": 0x401, "code": False}, compiled_bridge: {"access": 0x1041, "code": True}}
        self.verify(coverage, inventory, {OWNER: {"name": OWNER, "methods": methods}})
        self.assertEqual(row["compiled_identity"], source)
        self.assertEqual(row["compiled_generated_bridges"], [compiled_bridge])

    def test_class_without_methods_still_needs_its_own_classfile(self):
        with TemporaryDirectory() as tmp:
            path = Path(tmp) / "names.tsv"
            path.write_text(OWNER.encode().hex() + "\tfixture.Owner\n")
            owners = [OWNER, "Lfixture/Empty;"]
            with self.assertRaisesRegex(RuntimeError, "Missing, duplicate or unexpected compiled class"):
                class_names({"original_classes": owners, "class_count": 2},
                            {"classes": dict.fromkeys(owners)}, {OWNER: {}}, path)


if __name__ == "__main__":
    unittest.main()
