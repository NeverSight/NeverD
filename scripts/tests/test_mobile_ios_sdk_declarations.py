"""Guards for real SDK namespace evidence; these do not qualify recovery."""
import unittest

from scripts.collect_mobile_ios_sdk_declarations import declarations, macro_identifiers


class SDKDeclarationEvidenceTests(unittest.TestCase):
    def test_macros_preserve_object_and_function_names_without_evaluating_values(self):
        output = ("#define __APPLE__ 1\n#define __OBJC__ 1\n"
                  "#define SDK_EMPTY\n#define SDK_VALUE (1 + 2)\n"
                  "#define SDK_FUNCTION(x, y) ((x) + \\\n(y))\n")
        self.assertEqual(macro_identifiers(output),
                         ["SDK_EMPTY", "SDK_FUNCTION", "SDK_VALUE", "__APPLE__", "__OBJC__"])

    def test_missing_target_duplicate_unknown_or_truncated_macro_evidence_fails(self):
        target = "#define __APPLE__ 1\n#define __OBJC__ 1\n"
        for text in ("", "#define __APPLE__ 1\n", target + "#define __OBJC__ 1\n",
                     target + "unrecognized text\n", target + "#define 123BAD 1\n",
                     target + "#define SDK_VALUE \\", target + "\\",
                     target + "#define SDK\\u1234 1\n"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                macro_identifiers(text)

    def test_interface_names_are_not_all_subclass_evidence(self):
        ast = {"kind": "TranslationUnitDecl", "inner": [
            {"kind": "ObjCInterfaceDecl", "name": "RequiredSystemClass"},
            {"kind": "ObjCInterfaceDecl", "name": "ForwardOnly"},
            {"kind": "TypedefDecl", "name": "SDKValue"},
            {"kind": "FunctionDecl", "name": "SDKFunction"},
            {"kind": "EnumDecl", "name": "SeparateTag", "inner": [
                {"kind": "EnumConstantDecl", "name": "SDKEnumerator"}]},
            {"kind": "ObjCProtocolDecl", "name": "SeparateProtocol"}]}
        result = declarations(ast, "RequiredSystemClass")
        self.assertEqual(result["interface_declarations"], ["ForwardOnly", "RequiredSystemClass"])
        self.assertEqual(result["ordinary_identifiers"],
                         ["ForwardOnly", "RequiredSystemClass", "SDKEnumerator", "SDKFunction", "SDKValue"])
        self.assertNotIn("subclass_declaration_probe", result)
        with self.assertRaises(ValueError):
            declarations(ast, "MissingClass")


if __name__ == "__main__":
    unittest.main()
