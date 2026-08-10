from __future__ import annotations

import unittest

from scripts.check_python_plugin_sdk import parse_c_api

from scripts.check_python_plugin_sdk import C_API_HEADER


class ABIInventoryTests(unittest.TestCase):
    def test_public_enums_are_the_exact_abi_definitions(self) -> None:
        import neverd_plugin
        from neverd_plugin import abi

        self.assertIs(neverd_plugin.PluginType, abi.PluginType)
        self.assertIs(neverd_plugin.EventType, abi.EventType)
        self.assertIs(neverd_plugin.OutputLanguage, abi.OutputLanguage)

    def test_every_exported_c_function_has_a_python_signature(self) -> None:
        from neverd_plugin import abi

        declarations = parse_c_api(C_API_HEADER.read_text(encoding="utf-8"))
        self.assertEqual(set(abi.FUNCTION_SPECS), set(declarations))
        for name, (result, arguments) in declarations.items():
            with self.subTest(name=name):
                spec = abi.FUNCTION_SPECS[name]
                self.assertEqual(spec.c_result, result)
                self.assertEqual(spec.c_arguments, arguments)


if __name__ == "__main__":
    unittest.main()
