from __future__ import annotations

import unittest

from scripts.check_python_plugin_sdk import C_API_HEADER, parse_c_api_header


class ABIInventoryTests(unittest.TestCase):
    def test_public_enums_are_the_exact_abi_definitions(self) -> None:
        import neverd_plugin
        from neverd_plugin import abi

        self.assertIs(neverd_plugin.PluginType, abi.PluginType)
        self.assertIs(neverd_plugin.EventType, abi.EventType)
        self.assertIs(neverd_plugin.OutputLanguage, abi.OutputLanguage)
        self.assertIs(neverd_plugin.ProofStatus, abi.ProofStatus)
        self.assertIs(neverd_plugin.SynthesisOutcome, abi.SynthesisOutcome)
        self.assertIs(neverd_plugin.OptimizationStop, abi.OptimizationStop)
        self.assertIs(neverd_plugin.OptimizationMode, abi.OptimizationMode)
        self.assertIs(
            neverd_plugin.LLVMOptimizationLevel, abi.LLVMOptimizationLevel
        )

        self.assertEqual(
            [member.value for member in abi.ProofStatus], [0, 1, 2, 3, 4]
        )
        self.assertEqual(
            [member.value for member in abi.SynthesisOutcome],
            [0, 1, 2, 3, 4, 5, 6],
        )
        self.assertEqual(
            [member.value for member in abi.OptimizationStop], [0, 1, 2, 3, 4]
        )
        self.assertEqual(
            [member.value for member in abi.LLVMOptimizationLevel],
            [0, 1, 2, 3, 4],
        )

    def test_semantic_structs_mirror_the_append_only_c_layouts(self) -> None:
        from neverd_plugin import abi

        self.assertEqual(
            [name for name, _ctype in abi.NeverDSimplifyOptions._fields_],
            [
                "struct_size",
                "width",
                "shallow",
                "max_atoms",
                "max_work",
                "verify_samples",
                "allow_growth",
                "exhaustive",
            ],
        )
        self.assertEqual(
            [name for name, _ctype in abi.NeverDSynthesizeOptions._fields_],
            [
                "struct_size",
                "width",
                "max_cost",
                "max_samples",
                "verify_samples",
                "max_work",
                "max_leaves",
                "max_constants",
                "stochastic_slots",
                "stochastic_restarts",
                "stochastic_iterations",
                "solver_max_conflicts",
                "solver_max_propagations",
                "solver_max_watch_visits",
                "exhaustive",
            ],
        )
        self.assertEqual(
            [name for name, _ctype in abi.NeverDOptimizeLLVMOptions._fields_],
            [
                "struct_size",
                "mode",
                "llvm_level",
                "max_rounds",
                "enable_synthesis",
                "synthesis_max_cost",
                "synthesis_max_samples",
                "synthesis_verify_samples",
                "synthesis_max_work",
                "synthesis_max_leaves",
                "synthesis_max_constants",
                "synthesis_stochastic_slots",
                "synthesis_stochastic_restarts",
                "synthesis_stochastic_iterations",
                "solver_max_conflicts",
                "solver_max_propagations",
                "solver_max_watch_visits",
                "exhaustive",
            ],
        )

    def test_every_exported_c_function_has_a_python_signature(self) -> None:
        from neverd_plugin import abi

        declarations = parse_c_api_header(C_API_HEADER)
        self.assertEqual(set(abi.FUNCTION_SPECS), set(declarations))
        for name, (result, arguments) in declarations.items():
            with self.subTest(name=name):
                spec = abi.FUNCTION_SPECS[name]
                self.assertEqual(spec.c_result, result)
                self.assertEqual(spec.c_arguments, arguments)


if __name__ == "__main__":
    unittest.main()
