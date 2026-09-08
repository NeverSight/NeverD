"""Verified Objective-C body projection without guessing native function names."""

from __future__ import annotations

import copy
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/neverd"))

from mobile.common import MobileError
from mobile.objc_source import render_objc_sources
from mobile.macho import _types, objc_header


def fixture(body: str = "return arg0 + arg1;") -> tuple[dict, dict]:
    method = {"selector": "add:to:", "class_method": False,
              "implementation": "0x1000", "type_encoding": "q32@0:8q16q24"}
    metadata = {"status": "recovered", "classes": [{"name": "Calculator", "root_class": True,
                "address": "0x2000", "superclass_address": "0x0", "superclass": None,
                "methods": [method]}]}
    native = {**method, "class_name": "Calculator", "status": "recovered", "return_type": "int64_t",
              "parameters": [{"name": "objc_self", "type": "void*"}, {"name": "objc_cmd", "type": "void*"},
                             {"name": "arg0", "type": "int64_t"}, {"name": "arg1", "type": "int64_t"}],
              "function_name": "neverd_objc_imp_1000",
              "source": "#include <stdint.h>\nint64_t neverd_objc_imp_1000(void* objc_self, void* objc_cmd, int64_t arg0, int64_t arg1) {\n" + body + "\n}\n"}
    return {"schema_version": 1, "pointer_size": 8, "methods": [native]}, metadata


class ObjCSourceTests(unittest.TestCase):
    def render(self, body: str = "return arg0 + arg1;") -> tuple[str, dict]:
        return render_objc_sources(*fixture(body))

    def test_branch_loop_and_return_are_real_native_body(self) -> None:
        body = "int64_t sum = 0;\nfor (int64_t i = 0; i < arg0; ++i) {\n if (i & 1) sum += arg1;\n}\nreturn sum;"
        source, coverage = self.render(body)
        self.assertEqual(coverage["status"], "recovered")
        self.assertEqual(coverage["recovered_method_count"], 1)
        self.assertIn("@implementation Calculator", source)
        implementation = source.split("@implementation Calculator", 1)[1]
        self.assertIn(body, implementation)
        self.assertIn("void* objc_self = (void*)self;", implementation)
        self.assertIn("void* objc_cmd = (void*)_cmd;", implementation)
        self.assertIn("int64_t arg0 = (int64_t)neverd_objc_argument_", implementation)

    def test_literals_comments_and_braces_are_unchanged(self) -> None:
        body = r'''const char *text = "} /* objc_self neverd_objc_imp_1000 { ";
char ch = '}';
/* } return 99; { objc_cmd */
// } neverd_objc_imp_1000
return arg0 + arg1;'''
        source, coverage = self.render(body)
        self.assertEqual(coverage["status"], "recovered")
        self.assertIn(body, source.split("@implementation Calculator", 1)[1])

    def test_exact_function_name_not_address_or_comment(self) -> None:
        report, metadata = fixture()
        report["methods"][0]["function_name"] = "other_name"
        report["methods"][0]["source"] += "// int64_t other_name(void* objc_self, void* objc_cmd) { return 42; }\n"
        source, coverage = render_objc_sources(report, metadata)
        self.assertEqual(coverage["status"], "unrecovered")
        self.assertNotIn("@implementation", source)

    def test_prototype_alone_is_not_a_body(self) -> None:
        report, metadata = fixture()
        report["methods"][0]["source"] = report["methods"][0]["source"].split(" {")[0] + ";\n"
        _, coverage = render_objc_sources(report, metadata)
        self.assertIn("exactly one definition", coverage["methods"][0]["reason"])

    def test_line_comment_continuation_cannot_hide_a_definition(self) -> None:
        report, metadata = fixture()
        native = report["methods"][0]
        native["source"] = "// continued comment \\\n" + native["source"].replace("#include <stdint.h>\n", "").replace("\n", " ") + "\n"
        source, coverage = render_objc_sources(report, metadata)
        self.assertEqual(coverage["status"], "unrecovered")
        self.assertNotIn("@implementation", source)

    def test_helper_name_does_not_rewrite_wide_literal_prefix(self) -> None:
        report, metadata = fixture('const void *text = L"literal";\nreturn L(arg0) + arg1;')
        native = report["methods"][0]
        native["source"] = "static inline int64_t L(int64_t value) { return value; }\n" + native["source"]
        source, coverage = render_objc_sources(report, metadata)
        self.assertEqual(coverage["status"], "recovered")
        self.assertIn('L"literal"', source)

    def test_helper_prototype_external_declaration_and_recursion_are_retained(self) -> None:
        report, metadata = fixture("if (arg0) return helper(arg0);\nreturn neverd_objc_imp_1000(objc_self, objc_cmd, 1, arg1);")
        prefix = "extern int external_value(void);\nstatic inline int64_t helper(int64_t value);\nstatic inline int64_t helper(int64_t value) { return value + external_value(); }\n"
        report["methods"][0]["source"] = prefix + report["methods"][0]["source"]
        source, coverage = render_objc_sources(report, metadata)
        self.assertEqual(coverage["status"], "recovered", coverage)
        self.assertIn("extern int external_value(void);", source)
        self.assertIn("static inline int64_t neverd_objc_", source)
        self.assertIn("_helper(int64_t value);", source)
        self.assertIn("_neverd_objc_imp_1000(objc_self, objc_cmd, 1, arg1)", source)

    def test_same_helper_name_in_different_methods_has_private_definitions(self) -> None:
        report, metadata = fixture("return helper(arg0) + arg1;")
        native = report["methods"][0]
        native["source"] = "static inline int64_t helper(int64_t value) { return value + 1; }\n" + native["source"]
        other = copy.deepcopy(native)
        other.update(selector="multiply:by:", implementation="0x1010", function_name="neverd_objc_imp_1010")
        other["source"] = other["source"].replace("neverd_objc_imp_1000", "neverd_objc_imp_1010").replace("value + 1", "value * 2")
        report["methods"].append(other)
        metadata["classes"][0]["methods"].append({key: other[key] for key in ("selector", "implementation", "class_method", "type_encoding")})
        source, coverage = render_objc_sources(report, metadata)
        self.assertEqual(coverage["recovered_method_count"], 2, coverage)
        definitions = [line for line in source.splitlines() if line.startswith("static inline")]
        self.assertEqual(len(definitions), 2)
        self.assertNotEqual(definitions[0].split("(")[0], definitions[1].split("(")[0])

    def test_block_object_encoding_preserves_pointer_abi_without_inventing_prototype(self) -> None:
        encoding = "q32@0:8@?16q24"
        self.assertEqual(_types(encoding), ["long long", "id", "SEL", "id", "long long"])
        self.assertIsNone(_types("q32@0:8@??16q24"))
        report, metadata = fixture("return ((int64_t (^)(int64_t))arg0)(arg1);")
        native = report["methods"][0]
        native["parameters"][2]["type"] = "void*"
        native["source"] = native["source"].replace("int64_t arg0,", "void* arg0,")
        native["type_encoding"] = encoding
        metadata["classes"][0]["methods"][0]["type_encoding"] = encoding
        source, coverage = render_objc_sources(report, metadata)
        self.assertEqual(coverage["status"], "recovered", coverage)
        self.assertIn("add:(id)", objc_header(metadata))
        self.assertIn("((int64_t (^)(int64_t))arg0)(arg1)", source)
        self.assertEqual(native["type_encoding"], encoding)
        self.assertTrue(any("original invoke prototype is not encoded" in text for text in coverage["limitations"]))
        native["status"] = "unrecovered"
        native["reason"] = "method calls a native or dynamic target without a source binding"
        _, coverage = render_objc_sources(report, metadata)
        self.assertEqual(coverage["status"], "unrecovered")

    def test_runtime_block_array_allowlist_is_exact(self) -> None:
        for declaration, accepted in [
            ("extern void *_NSConcreteStackBlock[];", True),
            ("extern void *_NSConcreteGlobalBlock[];", True),
            ("extern int *_NSConcreteStackBlock[];", False),
            ("extern void *_NSConcreteStackBlock[12];", False),
            ("extern void *_NSConcreteOtherBlock[];", False),
        ]:
            with self.subTest(declaration=declaration):
                report, metadata = fixture()
                report["methods"][0]["source"] = declaration + "\n" + report["methods"][0]["source"]
                _, coverage = render_objc_sources(report, metadata)
                self.assertEqual(coverage["status"] == "recovered", accepted)

    @staticmethod
    def shared_block_fixture() -> tuple[dict, dict]:
        report, metadata = fixture("return (int64_t)neverd_block_literal_2100_address();")
        helper = """
int32_t neverd_block_invoke_1100(void *block, int32_t value) { return value - 9; }
uintptr_t neverd_block_descriptor_2200_address(void) {
  extern void *_NSConcreteGlobalBlock[];
  struct descriptor { uint64_t reserved, size; const char *signature; };
  struct literal { void *isa; uint32_t flags, reserved; void (*invoke)(void); const struct descriptor *descriptor; };
  struct storage { struct descriptor descriptor; struct literal literal; };
  static const struct storage data = {{0, 32, "i12@?0i8"},
    {(void *)_NSConcreteGlobalBlock, 0x50000000, 0, (void (*)(void))neverd_block_invoke_1100, &data.descriptor}};
  return (uintptr_t)&data.descriptor;
}
uintptr_t neverd_block_literal_2100_address(void) { return neverd_block_descriptor_2200_address() + 24; }
"""
        native = report["methods"][0]
        native["class_method"] = True
        metadata["classes"][0]["methods"][0]["class_method"] = True
        native["source"] = "uintptr_t neverd_block_literal_2100_address(void);\n" + native["source"] + helper
        native["shared_block_functions"] = ["neverd_block_invoke_1100", "neverd_block_descriptor_2200_address", "neverd_block_literal_2100_address"]
        second = copy.deepcopy(native)
        second.update(selector="identity:to:", implementation="0x1010", function_name="neverd_objc_imp_1010")
        second["source"] = second["source"].replace("neverd_objc_imp_1000", "neverd_objc_imp_1010")
        report["methods"].append(second)
        runtime = copy.deepcopy(metadata["classes"][0]["methods"][0])
        runtime.update(selector="identity:to:", implementation="0x1010")
        metadata["classes"][0]["methods"].append(runtime)
        return report, metadata

    def test_shared_block_functions_have_one_definition_and_conflicts_reject_all_users(self) -> None:
        report, metadata = self.shared_block_fixture()
        source, coverage = render_objc_sources(report, metadata)
        self.assertEqual(coverage["recovered_method_count"], 2, coverage)
        self.assertEqual(source.count("static const struct storage data ="), 1)
        self.assertEqual(source.count("return value - 9;"), 1)
        report["methods"][1]["source"] = report["methods"][1]["source"].replace("return value - 9;", "return value - 8;")
        _, coverage = render_objc_sources(report, metadata)
        self.assertEqual(coverage["recovered_method_count"], 0)
        self.assertTrue(all("conflicting shared Block" in item["reason"] for item in coverage["methods"]))

    def test_shared_block_inventory_requires_actual_generated_definitions(self) -> None:
        for names in [["missing"], ["neverd_block_literal_999_address"], [42], "name", ["neverd_objc_imp_1000"],
                      ["neverd_block_invoke_1100", "neverd_block_invoke_1100"]]:
            with self.subTest(names=names):
                report, metadata = self.shared_block_fixture()
                report["methods"] = report["methods"][:1]
                metadata["classes"][0]["methods"] = metadata["classes"][0]["methods"][:1]
                report["methods"][0]["shared_block_functions"] = names
                _, coverage = render_objc_sources(report, metadata)
                self.assertEqual(coverage["status"], "unrecovered")

    @unittest.skipUnless(sys.platform == "darwin" and shutil.which("clang"), "requires Darwin Objective-C runtime and clang")
    def test_two_methods_share_one_real_global_block_and_execute_original_invoke(self) -> None:
        source, coverage = render_objc_sources(*self.shared_block_fixture())
        self.assertEqual(coverage["recovered_method_count"], 2, coverage)
        source += """
int main(void) {
  uintptr_t a = (uintptr_t)[Calculator add:1 to:2];
  uintptr_t b = (uintptr_t)[Calculator identity:3 to:4];
  int (^block)(int) = (int (^)(int))a;
  return a == b && block(31) == 22 && block(-8) == -17 ? 0 : 1;
}
"""
        with tempfile.TemporaryDirectory(prefix="neverd-objc-block-source-") as temporary:
            path = Path(temporary)
            (path / "source.m").write_text(source)
            built = subprocess.run(["clang", "-fblocks", "-x", "objective-c", str(path / "source.m"),
                                    "-framework", "Foundation", "-o", str(path / "verify")],
                                   capture_output=True, text=True, timeout=30, check=False)
            self.assertEqual(built.returncode, 0, built.stderr)
            executed = subprocess.run([str(path / "verify")], capture_output=True, text=True, timeout=10, check=False)
            self.assertEqual(executed.returncode, 0, executed.stderr)

    def test_unsigned_native_parameter_is_explicitly_rebound(self) -> None:
        report, metadata = fixture("return arg0 / arg1;")
        native = report["methods"][0]
        native["parameters"][2]["type"] = "uint64_t"
        native["source"] = native["source"].replace("int64_t arg0", "uint64_t arg0")
        source, coverage = render_objc_sources(report, metadata)
        self.assertEqual(coverage["status"], "recovered")
        self.assertIn("uint64_t arg0 = (uint64_t)", source)
        self.assertIn("add:(long long)", source)

    def test_legacy_long_encoding_is_32_bit_on_a_64_bit_target(self) -> None:
        report, metadata = fixture()
        native = report["methods"][0]
        native["type_encoding"] = metadata["classes"][0]["methods"][0]["type_encoding"] = "l24@0:8l16l20"
        native["return_type"] = "int32_t"
        native["parameters"][2]["type"] = native["parameters"][3]["type"] = "int32_t"
        native["source"] = native["source"].replace("int64_t", "int32_t")
        source, coverage = render_objc_sources(report, metadata)
        self.assertEqual(coverage["status"], "recovered", coverage)
        self.assertIn("- (int)add:(int)", source)

    def test_reported_type_cannot_inject_comments_into_bindings(self) -> None:
        report, metadata = fixture()
        native = report["methods"][0]
        native["parameters"][0]["type"] = "void*// comment\n"
        native["source"] = native["source"].replace("void* objc_self", "void*// comment\n objc_self")
        source, coverage = render_objc_sources(report, metadata)
        self.assertEqual(coverage["status"], "unrecovered")
        self.assertNotIn("@implementation", source)

    def test_target_function_attributes_are_not_silently_lost(self) -> None:
        report, metadata = fixture()
        native = report["methods"][0]
        native["source"] = native["source"].replace("int64_t neverd_objc_imp_", '__attribute__((target("sse4.2"))) int64_t neverd_objc_imp_')
        _, coverage = render_objc_sources(report, metadata)
        self.assertEqual(coverage["status"], "unrecovered")
        self.assertIn("attributes", coverage["methods"][0]["reason"])

    def test_class_method_has_plus_prefix(self) -> None:
        report, metadata = fixture()
        report["methods"][0]["class_method"] = True
        metadata["classes"][0]["methods"][0]["class_method"] = True
        source, coverage = render_objc_sources(report, metadata)
        self.assertEqual(coverage["status"], "recovered")
        self.assertIn("+ (long long)add:", source)

    def test_missing_empty_or_unbalanced_body_is_unrecovered(self) -> None:
        for body in ("", "/* no body */", ";", "if (arg0) { return arg1;", 'return "unterminated;'):
            with self.subTest(body=body):
                source, coverage = self.render(body)
                self.assertEqual(coverage["status"], "unrecovered")
                self.assertNotIn("@implementation", source)

    def test_bad_or_incomplete_native_signatures_are_unrecovered(self) -> None:
        for change in ("parameter_count", "parameter_name", "reported_type", "source_type", "float_abi", "aggregate_abi"):
            with self.subTest(change=change):
                report, metadata = fixture()
                native = report["methods"][0]
                if change == "parameter_count":
                    native["parameters"].pop()
                elif change == "parameter_name":
                    native["parameters"][0]["name"] = "self"
                elif change == "reported_type":
                    native["parameters"][2]["type"] = "int32_t"
                elif change == "source_type":
                    native["source"] = native["source"].replace("int64_t arg0", "int32_t arg0")
                elif change == "float_abi":
                    native["return_type"] = "double"
                else:
                    native["type_encoding"] = metadata["classes"][0]["methods"][0]["type_encoding"] = "{Pair=ii}32@0:8q16q24"
                source, coverage = render_objc_sources(report, metadata)
                self.assertEqual(coverage["status"], "unrecovered")
                self.assertNotIn("@implementation", source)

    def test_source_parameter_redeclaration_is_rejected_but_multiply_is_not(self) -> None:
        for name in ("arg0", "objc_self", "objc_cmd"):
            with self.subTest(name=name):
                _, coverage = self.render(f"int64_t {name};\nreturn 1;")
                self.assertEqual(coverage["status"], "unrecovered")
                self.assertIn("redeclares", coverage["methods"][0]["reason"])
        _, coverage = self.render("return arg0 * arg1;")
        self.assertEqual(coverage["status"], "recovered")

    def test_unsafe_identifiers_never_become_source(self) -> None:
        for class_name, selector in (("Calculator; injected", "add:to:"), ("Calculator", "add:x;:"), ("while", "add:to:")):
            report, metadata = fixture()
            report["methods"][0].update(class_name=class_name, selector=selector)
            metadata["classes"][0]["name"] = class_name
            metadata["classes"][0]["methods"][0]["selector"] = selector
            source, coverage = render_objc_sources(report, metadata)
            self.assertEqual(coverage["status"], "unrecovered")
            self.assertNotIn("@implementation", source)
            self.assertNotIn("injected", source)

    def test_mismatched_metadata_and_native_status_are_explicit(self) -> None:
        report, metadata = fixture()
        report["methods"][0]["implementation"] = "0x9999"
        _, coverage = render_objc_sources(report, metadata)
        self.assertIn("identity disagrees", coverage["methods"][0]["reason"])
        report, metadata = fixture()
        report["methods"][0].update(status="unrecovered", reason="unbound runtime dependency")
        _, coverage = render_objc_sources(report, metadata)
        self.assertEqual(coverage["methods"][0]["reason"], "unbound runtime dependency")

    def test_native_diagnostics_survive_recovered_and_unrecovered_methods(self) -> None:
        for status in ("recovered", "unrecovered"):
            with self.subTest(status=status):
                report, metadata = fixture()
                diagnostics = ["Runtime type hint is unverified.", "Variadic tails are not described by the encoding."]
                report["methods"][0].update(status=status, diagnostics=diagnostics, reason="unsupported native dependency")
                _, coverage = render_objc_sources(report, metadata)
                self.assertEqual(coverage["methods"][0]["diagnostics"], diagnostics)
                self.assertIsNot(coverage["methods"][0]["diagnostics"], diagnostics)

    def test_missing_diagnostics_remain_compatible(self) -> None:
        _, coverage = self.render()
        self.assertEqual(coverage["status"], "recovered")
        self.assertEqual(coverage["methods"][0]["diagnostics"], [])

    def test_invalid_native_diagnostics_explicitly_prevent_recovery(self) -> None:
        for diagnostics in (None, "warning", ["valid", 3], [{"warning": "invalid item"}]):
            with self.subTest(diagnostics=diagnostics):
                report, metadata = fixture()
                report["methods"][0]["diagnostics"] = diagnostics
                source, coverage = render_objc_sources(report, metadata)
                self.assertEqual(coverage["status"], "unrecovered")
                self.assertEqual(coverage["methods"][0]["diagnostics"], [])
                self.assertIn("diagnostics must be a list of strings", coverage["methods"][0]["reason"])
                self.assertNotIn("@implementation", source)

    def test_unmatched_native_method_diagnostics_are_preserved_and_validated(self) -> None:
        for diagnostics in (["Native record without runtime metadata"], [42]):
            with self.subTest(diagnostics=diagnostics):
                report, metadata = fixture()
                report["methods"][0].update(selector="unknown:method:", diagnostics=diagnostics)
                _, coverage = render_objc_sources(report, metadata)
                unmatched = next(method for method in coverage["methods"] if method["selector"] == "unknown:method:")
                self.assertEqual(unmatched["status"], "unrecovered")
                if isinstance(diagnostics[0], str):
                    self.assertEqual(unmatched["diagnostics"], diagnostics)
                else:
                    self.assertIn("diagnostics must be a list of strings", unmatched["reason"])

    def test_missing_methods_and_partial_metadata_prevent_full_coverage(self) -> None:
        report, metadata = fixture()
        missing = dict(metadata["classes"][0]["methods"][0], selector="missing:body:", implementation="0x1010")
        metadata["classes"][0]["methods"].append(missing)
        _, coverage = render_objc_sources(report, metadata)
        self.assertEqual(coverage["status"], "partial")
        self.assertEqual((coverage["recovered_method_count"], coverage["unrecovered_method_count"]), (1, 1))
        report, metadata = fixture()
        metadata["status"] = "partial"
        _, coverage = render_objc_sources(report, metadata)
        self.assertEqual(coverage["status"], "partial")

    def test_duplicate_inconsistent_reports_do_not_choose_arbitrarily(self) -> None:
        report, metadata = fixture()
        duplicate = copy.deepcopy(report["methods"][0])
        duplicate["source"] = duplicate["source"].replace("arg0 + arg1", "42")
        report["methods"].append(duplicate)
        source, coverage = render_objc_sources(report, metadata)
        self.assertEqual(coverage["status"], "unrecovered")
        self.assertNotIn("@implementation", source)

    def test_unexpected_native_method_is_not_silently_ignored(self) -> None:
        report, metadata = fixture()
        report["methods"][0]["selector"] = "unknown:method:"
        _, coverage = render_objc_sources(report, metadata)
        self.assertEqual(coverage["unrecovered_method_count"], 2)

    def test_conditional_definition_does_not_become_a_live_method(self) -> None:
        report, metadata = fixture()
        report["methods"][0]["source"] = "#if 0\n" + report["methods"][0]["source"] + "#endif\n"
        _, coverage = render_objc_sources(report, metadata)
        self.assertEqual(coverage["status"], "unrecovered")
        self.assertIn("preprocessor", coverage["methods"][0]["reason"])

    def test_conflicting_external_prototypes_reject_both_methods(self) -> None:
        report, metadata = fixture()
        first = report["methods"][0]
        first["source"] = "extern int external_value(void);\n" + first["source"]
        second = copy.deepcopy(first)
        second.update(selector="other:method:", implementation="0x1010", function_name="neverd_objc_imp_1010")
        second["source"] = second["source"].replace("neverd_objc_imp_1000", "neverd_objc_imp_1010").replace("extern int", "extern double")
        report["methods"].append(second)
        metadata["classes"][0]["methods"].append({key: second[key] for key in ("selector", "implementation", "class_method", "type_encoding")})
        source, coverage = render_objc_sources(report, metadata)
        self.assertEqual(coverage["unrecovered_method_count"], 2)
        self.assertNotIn("@implementation", source)

    def test_empty_report_is_no_methods_and_needs_no_pointer_size(self) -> None:
        source, coverage = render_objc_sources({"schema_version": 1, "methods": []}, {"status": "section-absent", "classes": []})
        self.assertEqual(coverage["status"], "no-methods")
        self.assertNotIn("@implementation", source)
        json.dumps(coverage)

    def test_schema_errors_are_actionable(self) -> None:
        with self.assertRaisesRegex(MobileError, "schema"):
            render_objc_sources({"schema_version": 2, "methods": []}, {"classes": []})

    @unittest.skipUnless(sys.platform == "darwin" and shutil.which("clang"), "requires Apple Clang and Objective-C runtime")
    def test_projected_class_method_compiles_and_runs(self) -> None:
        body = "int64_t sum = 0;\nfor (int64_t i = 0; i < arg0; ++i) { if (i & 1) sum += arg1; }\nreturn sum;"
        report, metadata = fixture(body)
        report["methods"][0]["class_method"] = True
        metadata["classes"][0]["methods"][0]["class_method"] = True
        source, coverage = render_objc_sources(report, metadata)
        self.assertEqual(coverage["status"], "recovered")
        source += "\nint main(void) { return [Calculator add:6 to:7] == 21 ? 0 : 1; }\n"
        with tempfile.TemporaryDirectory(prefix="neverd-objc-source-") as temporary:
            path = Path(temporary)
            (path / "recovered.m").write_text(source)
            compiled = subprocess.run(["clang", "-x", "objective-c", str(path / "recovered.m"),
                                       "-framework", "Foundation", "-o", str(path / "verify")],
                                      capture_output=True, text=True, timeout=30, check=False)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            executed = subprocess.run([str(path / "verify")], capture_output=True, text=True, timeout=10, check=False)
            self.assertEqual(executed.returncode, 0, executed.stderr)


if __name__ == "__main__":
    unittest.main()
