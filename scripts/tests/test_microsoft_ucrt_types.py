"""UCRT import names select signatures; they never supply ABI guesses."""

from pathlib import Path
import json
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import microsoft_ucrt_types as ucrt


def member(name, architecture="x86", kind="code", module="api-ms-win-crt-test.dll"):
    return (f"File: {module}\nFormat: {ucrt.IMPORT_FORMATS[architecture]}\n"
            f"Type: {kind}\nName type: exportas\nExport name: {name}\n"
            "Symbol: __imp__DifferentLinkerSymbol\nSymbol: _DifferentLinkerSymbol\n\n")


class MicrosoftUCRTTypes(unittest.TestCase):
    def test_compiler_abi_keeps_calling_convention_and_extension_attributes(self):
        ir = ('@neverd_ucrt_abi_0 = global ptr @"\\01_routine@4", align 4\n'
              'declare x86_stdcallcc zeroext i1 @"\\01_routine@4"(ptr noundef) #2\n')
        expected = {"routine": ('@"\\01_routine@4"', 'x86_stdcallcc zeroext i1', 'ptr noundef')}
        self.assertEqual(ucrt.bridge_abis(ir, ["routine"]), expected)
        self.assertNotEqual(ucrt.bridge_abis(ir.replace('x86_stdcallcc ', ''), ["routine"]), expected)
        self.assertNotEqual(ucrt.bridge_abis(ir.replace('zeroext ', ''), ["routine"]), expected)
        with self.assertRaisesRegex(ValueError, "unique compiler ABI"):
            ucrt.bridge_abis(ir.replace('declare', 'missing'), ["routine"])

    def test_compiler_abi_keeps_by_value_record_type_and_alignment(self):
        ir = ('@neverd_ucrt_abi_0 = global ptr @"\\01_routine@12", align 4\n'
              'declare x86_stdcallcc ptr @"\\01_routine@12"'
              '(ptr noundef, ptr noundef byval(%union.Value) align 4) #0\n')
        expected = {"routine": ('@"\\01_routine@12"', 'x86_stdcallcc ptr',
                                'ptr noundef, ptr noundef byval(%union.Value) align 4')}
        self.assertEqual(ucrt.bridge_abis(ir, ["routine"]), expected)
        for before, after in [('byval(%union.Value)', ''), ('align 4)', 'align 8)'),
                              ('%union.Value', '%struct.Other')]:
            self.assertNotEqual(ucrt.bridge_abis(ir.replace(before, after), ["routine"]), expected)
        with self.assertRaisesRegex(ValueError, "unique compiler ABI"):
            ucrt.bridge_abis(ir.replace('align 4)', 'align 4'), ["routine"])

    def test_dll_inventory_selects_executable_extent_and_records_other_exports(self):
        text = ("File: ucrtbase.dll\nFormat: COFF-ARM64\nSections [\n"
                "  Section {\n    VirtualAddress: 0x1000\n    VirtualSize: 0x20\n"
                "    RawDataSize: 512\n    IMAGE_SCN_MEM_EXECUTE\n  }\n"
                "  Section {\n    VirtualAddress: 0x2000\n    VirtualSize: 0x20\n  }\n]\n")
        for ordinal, (name, rva) in enumerate([
                ("known", 0x1000), ("_o_known", 0x101f), ("padding", 0x1020),
                ("data_or_forwarder", 0x2000), ("unmapped", 0x3000), ("", 0x1008)], 1):
            text += f"Export {{\n  Ordinal: {ordinal}\n  Name: {name}\n  RVA: {hex(rva)}\n}}\n"
        code, other = ucrt.dll_exports(text, "arm64")
        self.assertEqual([r["export"] for r in code], ["_o_known", "known"])
        self.assertEqual([r["export"] for r in other], ["data_or_forwarder", "padding", "unmapped"])
        self.assertEqual(ucrt.select_exports(code, ["known"]),
                         (["_o_known", "known"], {"_o_known": "known"}, []))
        hybrid = text.replace("Format: COFF-ARM64", "Format: COFF-ARM64X")
        hybrid += "HybridObject {\n  Format: COFF-ARM64EC\n  Export {\n    Name: EC_only\n  }\n}\n"
        self.assertEqual(ucrt.dll_exports(hybrid, "arm64"), (code, other))
        with self.assertRaisesRegex(ValueError, "architecture"):
            ucrt.dll_exports(text.replace("COFF-ARM64", "COFF-ARM64EC"), "arm64")
        with self.assertRaisesRegex(ValueError, "architecture"):
            ucrt.dll_exports(text, "x64")
        with self.assertRaisesRegex(ValueError, "one UCRT DLL"):
            ucrt.dll_exports(text + "File: another.dll\n", "arm64")
        with self.assertRaisesRegex(ValueError, "duplicate"):
            ucrt.dll_exports(text + "Export {\n  Ordinal: 99\n  Name: known\n  RVA: 0x1000\n}\n", "arm64")

    def test_c_bridge_preserves_zero_arguments_and_only_erases_empty_throw(self):
        self.assertEqual(ucrt.c_declaration('extern "C" void __cdecl routine() throw()'),
                         "void __cdecl routine(void);\n")
        self.assertEqual(ucrt.c_declaration("typedef void (__cdecl *handler)()"),
                         "typedef void (__cdecl *handler)(void);\n")
        self.assertEqual(ucrt.c_declaration("bool Owned"), "_Bool Owned;\n")
        self.assertIn("throw(int)", ucrt.c_declaration("void routine() throw(int)"))
        for suffix in ("noexcept", "noexcept(true)", "noexcept(false)"):
            self.assertEqual(ucrt.c_declaration("void routine() " + suffix),
                             "void routine(void);\n")
        self.assertIn("noexcept(condition)", ucrt.c_declaration("void routine() noexcept(condition)"))

    def test_ast_extraction_never_enters_classes_or_templates(self):
        ast = {"inner": [{"kind": "CXXRecordDecl", "inner": [{"kind": "FunctionDecl", "name": "hidden"}]},
                         {"kind": "LinkageSpecDecl", "inner": [{"kind": "FunctionDecl", "name": "visible"}]}]}
        self.assertEqual([node.get("name") for node in ucrt.global_declarations(ast)], [None, "visible"])
        source = b"int routine(void) { return 1; }"
        node = {"kind": "FunctionDecl", "range": {"begin": {"offset": 0},
                "end": {"offset": len(source) - 1, "tokLen": 1}}}
        with self.assertRaisesRegex(ValueError, "declarations only"):
            ucrt.ast_declaration(source, node)

    def test_explicit_exports_are_not_decorated_symbols(self):
        for arch in ucrt.IMPORT_FORMATS:
            text = member("_o_format", arch) + member("_o_format", arch)
            text += member("Data", arch, "data")
            text += member("WrongArchitecture", "x64" if arch != "x64" else "x86")
            self.assertEqual(ucrt.import_names(text, arch),
                             [{"module": "api-ms-win-crt-test.dll", "export": "_o_format"}])

    def test_arm64ec_is_not_native_arm64(self):
        text = member("Native", "arm64")
        text += member("EC", "arm64").replace("COFF-import-file-ARM64", "COFF-import-file-ARM64EC")
        self.assertEqual(ucrt.import_names(text, "arm64"),
                         [{"module": "api-ms-win-crt-test.dll", "export": "Native"}])

    def test_old_reader_or_wrong_architecture_cannot_certify_coverage(self):
        for text, message in [(member("Routine").replace("Export name: Routine\n", ""), "Export name"),
                              (member("Routine", "x64"), "no UCRT"),
                              (member("?Mangled@@"), "spelling"),
                              (member("Routine", module="unknown.obj"), "DLL identity")]:
            with self.assertRaisesRegex(ValueError, message):
                ucrt.import_names(text, "x86")

    def test_osmode_alias_requires_both_export_and_known_declaration(self):
        imports = [{"export": name} for name in
                   ["known", "_o_known", "_o_unknown", "_CIpow", "_o_existing"]]
        selected, aliases, missing = ucrt.select_exports(
            imports, ["known", "not_exported", "_o_existing", "__compiler_intrinsic"])
        self.assertEqual(selected, ["_o_existing", "_o_known", "known"])
        self.assertEqual(aliases, {"_o_known": "known"})
        self.assertEqual(missing, ["_CIpow", "_o_unknown"])

    def test_compatibility_only_selects_officially_unresolved_names(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "UCRT-inputs").mkdir()
            (root / "licenses").mkdir()
            source = root / "source.c"
            source.write_text("/* Synthetic test notice */\n"
                              "int CDECL missing(int x) { return x; }\n")
            license_file = root / "license.txt"
            license_file.write_text("Synthetic test license")
            inputs = root / "inputs.json"
            inputs.write_text(json.dumps({"compatibility": {
                "provider": "fixture", "revision": "fixture revision",
                "sources": [{"name": "missing", "file": source.name,
                             "url": "fixture", "sha256": "fixture"}],
                "license": {"file": license_file.name, "url": "fixture", "sha256": "fixture"}}}))
            runtime = {"arguments": ["fixture compiler"],
                       "metadata": {"unresolved_exports": ["_o_missing", "unreviewed"]}}
            def download(cache, filename, *_):
                return root / filename
            with patch.object(ucrt, "INPUTS", inputs), patch.object(ucrt.wdk, "download", download):
                provider = ucrt.prepare_compatibility(runtime, root, root)
                self.assertEqual(provider["prototypes"], ["_o_missing"])
                self.assertEqual(provider["metadata"]["unresolved_exports"], ["unreviewed"])
                self.assertNotIn("return x", provider["source"].read_text())
                self.assertIn("__typeof__(missing) _o_missing", provider["source"].read_text())
                runtime["metadata"]["unresolved_exports"] = ["unreviewed"]
                self.assertIsNone(ucrt.prepare_compatibility(runtime, root, root))


if __name__ == "__main__":
    unittest.main()
