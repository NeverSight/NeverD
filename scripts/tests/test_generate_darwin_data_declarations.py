import unittest
from types import SimpleNamespace
from scripts.generate_darwin_data_declarations import (
    DataDeclarations, LEGACY_LITERAL_PROBES, LITERAL_PROBES,
    legacy_literal_storage_declarations, literal_storage_declarations, render)


class DarwinDataDeclarationTests(unittest.TestCase):
    @staticmethod
    def literal_ir():
        return "\n".join(
            f"@storage_{index} = external global ptr #0\n"
            f"define nonnull ptr @{probe}() local_unnamed_addr #1 {{\n"
            f"entry:\n  ret ptr @storage_{index}\n}}"
            for index, probe in enumerate(LITERAL_PROBES))

    def test_compiler_literal_identities_still_require_platform_and_export_agreement(self):
        profile = literal_storage_declarations(self.literal_ir())
        self.assertEqual(profile, {f"storage_{i}": {"data"} for i in range(4)})
        negative = {**profile, "storage_1": {""}}
        missing = {name: kind for name, kind in profile.items() if name != "storage_2"}
        output, count = render([profile, negative, profile, missing],
                               [{"storage_0": {"A"}, "storage_1": {"A"},
                                 "storage_2": {"A"}}] * 2, "test", "test")
        self.assertEqual(count, 3)
        self.assertIn('{"storage_0", "A", "A", false, false}', output)
        self.assertIn('{"storage_1", "", "A", false, false}', output)
        self.assertIn('{"storage_2", "A", "", false, false}', output)
        self.assertNotIn('"storage_3"', output)

    def test_literal_facts_reject_non_data_returns_and_ambiguous_ir(self):
        ir = self.literal_ir()
        for before, after in (
                ("external global ptr", "external thread_local global ptr"),
                ("external global ptr", "external global i32"),
                ("external global ptr", "extern_weak global ptr"),
                ("external global ptr", "global ptr null"),
                ("ret ptr @storage_0", "ret ptr @undeclared"),
                ("ret ptr @storage_0", "ret ptr null"),
                ("ret ptr @storage_0", "%v = load ptr, ptr @storage_0\n  ret ptr %v"),
                ("ret ptr @storage_0", "%v = call ptr @callee()\n  ret ptr %v"),
                ("ret ptr @storage_0", "ret i64 ptrtoint (ptr @storage_0 to i64)"),
                ("@neverd_literal_array()", "@unrelated()")):
            with self.subTest(after=after), self.assertRaises(ValueError):
                literal_storage_declarations(ir.replace(before, after, 1))
        for duplicate in (ir, "\n@storage_0 = external global ptr #0"):
            with self.assertRaises(ValueError):
                literal_storage_declarations(ir + "\n" + duplicate)

    def test_legacy_empty_collection_loads_supply_storage_addresses(self):
        ir = "\n".join(
            f"@storage_{index} = external local_unnamed_addr global ptr\n"
            f"define ptr @{probe}() {{\n"
            f"  %value = load ptr, ptr @storage_{index}, align 8\n"
            f"  %retained = call ptr @llvm.objc.retain(ptr %value)\n"
            f"  ret ptr %value\n}}"
            for index, probe in enumerate(LEGACY_LITERAL_PROBES))
        self.assertEqual(legacy_literal_storage_declarations(ir),
                         {f"storage_{i}": {"data"} for i in range(2)})
        for before, after in (
                ("external local_unnamed_addr global ptr", "global ptr null"),
                ("load ptr, ptr @storage_0", "load ptr, ptr @missing"),
                ("ret ptr %value", "ret ptr null")):
            with self.subTest(after=after), self.assertRaises(ValueError):
                legacy_literal_storage_declarations(ir.replace(before, after, 1))

    def test_only_external_non_tls_variables_supply_storage_addresses(self):
        clang = DataDeclarations.__new__(DataDeclarations)
        clang.string = lambda value: value
        clang.clang_getCursorLinkage = lambda cursor: cursor.linkage
        clang.clang_Cursor_getMangling = lambda cursor: cursor.name
        clang.clang_getCursorTLSKind = lambda cursor: cursor.tls
        clang.clang_getCursorType = lambda cursor: cursor.type
        clang.clang_getCanonicalType = lambda value: value
        cursor = SimpleNamespace(kind=9, linkage=4, name="_value", tls=0,
                                 type=SimpleNamespace(kind=17))
        self.assertEqual(clang.declaration(cursor), ("value", "data"))
        cursor.type.kind = 109
        self.assertEqual(clang.declaration(cursor), ("value", "object"))
        cursor.type.kind = 17
        for tls in (1, 2):
            cursor.tls = tls
            self.assertEqual(clang.declaration(cursor), ("value", ""))
        for field, value in (("kind", 8), ("linkage", 2), ("name", "value")):
            changed = SimpleNamespace(**vars(cursor))
            setattr(changed, field, value)
            self.assertIsNone(clang.declaration(changed))

    def test_missing_conflicting_and_tls_profiles_cannot_bind(self):
        first = {"tls": {""}, "mixed": {"data", ""}, "absent": {"data"},
                 "conflict": {"data"}, "known": {"data"}}
        second = {"tls": {""}, "mixed": {"data", ""}, "conflict": {""},
                  "known": {"data"}}
        output, count = render([first, second, first, second],
                               [{name: {"A"} for name in first}] * 2, "test", "test")
        self.assertEqual(count, 4)
        self.assertNotIn('"absent"', output)
        for name in ("tls", "mixed", "conflict"):
            self.assertIn('{"' + name + '", "", "", false, false}', output)
        self.assertIn('{"known", "A", "A", false, false}', output)

    def test_exports_remain_architecture_specific_and_deterministic(self):
        profile = {"first": {"data"}, "second": {"data"}, "missing": {"data"}}
        exports = [{"first": {"B", "A"}}, {"second": {"C"}}]
        output, count = render([profile] * 4, exports, "test", "test")
        self.assertEqual(count, 2)
        self.assertIn('{"first", "A|B", "", false, false}', output)
        self.assertIn('{"second", "", "C", false, false}', output)
        self.assertNotIn('"missing"', output)
        reverse = dict(reversed(list(profile.items())))
        self.assertEqual(render([reverse] * 4, exports, "test", "test")[0], output)


if __name__ == "__main__":
    unittest.main()
