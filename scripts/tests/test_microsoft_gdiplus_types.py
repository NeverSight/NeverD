import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import microsoft_gdiplus_types as gdiplus


class GDIPlusProjection(unittest.TestCase):
    def test_x86_c_linkage_and_reference_inventory(self):
        def routine(name, decorated, parameter):
            return {"kind": "FunctionDecl", "name": name, "mangledName": decorated,
                    "inner": [{"kind": "ParmVarDecl", "type": {"qualType": parameter}}]}

        routines = [routine("GdipCreatePath", "_GdipCreatePath@8", "GpPath **"),
                    routine("GdipCreateEffect", "_GdipCreateEffect@20", "GUID"),
                    routine("GdiplusStartup", "_GdiplusStartup@12", "ULONG_PTR *"),
                    routine("GdipGetMetafileHeaderFromWmf", "_GdipGetMetafileHeaderFromWmf", "HANDLE"),
                    routine("GdipMeasureCharacterRanges", "_GdipMeasureCharacterRanges@32", "const RectF &"),
                    routine("GdipCppHelper", "?GdipCppHelper@@YAHXZ", "int")]
        root = {"kind": "NamespaceDecl", "name": "Gdiplus",
                "inner": [{"kind": "LinkageSpecDecl", "inner": routines}]}
        _, functions, _ = gdiplus.declarations(Path("input.cpp"), ["clang"], Path("."),
                                               lambda command, log: json.dumps(root))
        self.assertNotIn("GdipCppHelper", functions)
        self.assertIn("GdipGetMetafileHeaderFromWmf", gdiplus.supported(functions))
        self.assertEqual(set(functions) - set(gdiplus.supported(functions)),
                         {"GdipMeasureCharacterRanges"})

    def test_enum_values_come_from_compiler_constants(self):
        first = {"kind": "EnumConstantDecl", "name": "First", "inner": [
            {"kind": "ImplicitCastExpr", "inner": [{"kind": "ConstantExpr", "value": "-2"}]}]}
        next_value = {"kind": "EnumConstantDecl", "name": "Next"}
        self.assertEqual(list(gdiplus.enum_members({"inner": [first, next_value]})),
                         [("First", -2), ("Next", -1)])
        first["inner"] = [{"kind": "UnknownExpr"}]
        with self.assertRaisesRegex(ValueError, "unevaluated"):
            list(gdiplus.enum_members({"inner": [first]}))

    def test_library_requires_the_original_decorated_symbol(self):
        def member(name, decorated):
            return (f"File: gdiplus.dll\nFormat: COFF-import-file-i386\nType: code\n"
                    f"Export name: {name}\nSymbol: __imp_{decorated}\nSymbol: {decorated}\n")

        symbols = {"GdiplusStartup": "_GdiplusStartup@12",
                   "GdipCreatePath": "_GdipCreatePath@8",
                   "GdipGetMetafileHeaderFromWmf": "_GdipGetMetafileHeaderFromWmf@12"}
        text = "".join(member(name, decorated) for name, decorated in symbols.items())
        functions = {name: {"mangledName": decorated} for name, decorated in symbols.items()}
        functions["GdipFontCollectionEnumerable"] = {"mangledName": "_GdipFontCollectionEnumerable@12"}
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            library = directory / "gdiplus.lib"
            library.write_bytes(b"fixture")
            inputs = {"architecture": "x86", "library": str(library),
                      "sha256": gdiplus.wdk.digest(library), "command": ["llvm-readobj"]}
            self.assertEqual(set(gdiplus.imported_functions(
                functions, inputs, directory, lambda cmd, log: text)), set(symbols))
            functions["GdipGetMetafileHeaderFromWmf"]["mangledName"] = "_GdipGetMetafileHeaderFromWmf"
            with self.assertRaisesRegex(ValueError, "compiler linkage"):
                gdiplus.imported_functions(functions, inputs, directory, lambda cmd, log: text)

    def test_opaque_pointer_keeps_its_type_and_x86_convention(self):
        projection = gdiplus.CProjection({"GpPath": {"kind": "CXXRecordDecl"}})
        routine = {"type": {"qualType": "int (GpPath *)"}, "inner": [
            {"kind": "ParmVarDecl", "name": "path", "type": {"qualType": "GpPath *"}}]}
        self.assertEqual(projection.function("GdipExample", routine),
                         "int __cdecl GdipExample(Gdiplus_GpPath * path);")
        self.assertEqual(projection.incomplete, ["GpPath"])
        self.assertEqual(projection.definitions["GpPath"], "typedef struct Gdiplus_GpPath Gdiplus_GpPath;")
        routine["type"]["qualType"] += " __attribute__((stdcall))"
        self.assertIn(" __stdcall ", projection.function("GdipExample", routine))


if __name__ == "__main__":
    unittest.main()
