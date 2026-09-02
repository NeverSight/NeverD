from __future__ import annotations

import ctypes
import unittest

from scripts.check_python_plugin_sdk import (
    C_API_HEADER,
    check_concolic_abi,
    check_sanitizer_abi,
    parse_c_api_header,
)


class ABIInventoryTests(unittest.TestCase):
    def test_sanitizer_header_and_python_contract_do_not_drift(self) -> None:
        errors: list[str] = []
        check_sanitizer_abi(errors)
        self.assertEqual(errors, [])

    def test_concolic_header_and_python_contract_do_not_drift(self) -> None:
        errors: list[str] = []
        check_concolic_abi(errors)
        self.assertEqual(errors, [])

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
        self.assertIs(neverd_plugin.LLVMOptimizationLevel, abi.LLVMOptimizationLevel)
        self.assertIs(
            neverd_plugin.TranslationObjectFormat,
            abi.TranslationObjectFormat,
        )
        self.assertIs(neverd_plugin.TranslationErrorCode, abi.TranslationErrorCode)
        self.assertIs(
            neverd_plugin.TranslationSemanticStop,
            abi.TranslationSemanticStop,
        )
        self.assertIs(
            neverd_plugin.TranslationProofStatus,
            abi.TranslationProofStatus,
        )
        self.assertIs(neverd_plugin.SanitizeStrategy, abi.SanitizeStrategy)
        self.assertIs(neverd_plugin.SanitizeStatus, abi.SanitizeStatus)
        self.assertIs(
            neverd_plugin.SanitizePublicationOutcome,
            abi.SanitizePublicationOutcome,
        )
        self.assertIs(
            neverd_plugin.SanitizePublicationNamespace,
            abi.SanitizePublicationNamespace,
        )
        self.assertIs(
            neverd_plugin.SanitizePublicationGuarantee,
            abi.SanitizePublicationGuarantee,
        )
        self.assertIs(
            neverd_plugin.SanitizePublicationOperandBinding,
            abi.SanitizePublicationOperandBinding,
        )

        self.assertEqual([member.value for member in abi.ProofStatus], [0, 1, 2, 3, 4])
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
        self.assertEqual(
            [member.value for member in abi.TranslationObjectFormat], [0, 1, 2]
        )
        self.assertEqual(
            [member.value for member in abi.TranslationErrorCode],
            list(range(15)),
        )
        self.assertEqual(
            [member.value for member in abi.TranslationSemanticStop],
            [0, 1, 2, 3],
        )
        self.assertEqual(
            [member.value for member in abi.TranslationProofStatus],
            [0, 1, 2, 3, 4],
        )
        self.assertEqual([member.value for member in abi.SanitizeStrategy], [0, 1])
        self.assertEqual(
            [member.value for member in abi.SanitizeStatus],
            list(range(21)),
        )
        self.assertEqual(
            [member.value for member in abi.SanitizePublicationOutcome],
            [0, 1, 2, 3],
        )
        self.assertEqual(
            [member.value for member in abi.SanitizePublicationNamespace],
            [0, 1, 2],
        )
        self.assertEqual(
            [member.value for member in abi.SanitizePublicationOperandBinding],
            [0, 1, 2],
        )
        self.assertEqual(
            {member.name: member.value for member in abi.SanitizePublicationGuarantee},
            {
                "NAMESPACE_ATOMIC": 1,
                "DESTINATION_CREATE_EXCLUSIVE": 2,
                "COMPARE_AND_SWAP": 4,
                "CRASH_DURABLE": 8,
            },
        )

    def test_sanitizer_structs_mirror_size_gated_append_only_layouts(self) -> None:
        from neverd_plugin import abi

        self.assertEqual(
            [name for name, _ctype in abi.NeverDSanitizeOptionsV1._fields_],
            [
                "struct_size",
                "strategy",
                "max_paths",
                "max_steps",
                "max_loop",
                "solver_conflicts",
                "max_call_depth",
                "max_summary_iterations",
            ],
        )
        self.assertEqual(
            [name for name, _ctype in abi.NeverDSanitizeResultV1._fields_],
            [
                "struct_size",
                "ok",
                "status",
                "plan_version",
                "findings",
                "guarded_sites",
                "guarded_functions",
                "unsupported_sites",
                "patched_functions",
                "code_size",
                "trampoline_count",
                "publication_outcome",
                "publication_receipt_version",
                "publication_receipt_complete",
                "publication_namespace_disposition",
                "publication_guarantee_flags",
                "publication_operand_binding",
            ],
        )
        if ctypes.sizeof(ctypes.c_void_p) != 8:
            self.skipTest("the published Python SDK supports 64-bit hosts")

        self.assertEqual(ctypes.sizeof(abi.NeverDSanitizeOptionsV1), 40)
        self.assertEqual(ctypes.sizeof(abi.NeverDSanitizeResultV1), 104)
        self.assertIs(
            dict(abi.NeverDSanitizeResultV1._fields_)["status"],
            ctypes.c_uint32,
        )
        for field, expected in {
            "struct_size": 0,
            "strategy": 8,
            "max_paths": 12,
            "max_steps": 16,
            "max_loop": 20,
            "solver_conflicts": 24,
            "max_call_depth": 32,
            "max_summary_iterations": 36,
        }.items():
            with self.subTest(struct="options", field=field):
                self.assertEqual(
                    getattr(abi.NeverDSanitizeOptionsV1, field).offset,
                    expected,
                )
        for field, expected in {
            "struct_size": 0,
            "ok": 8,
            "status": 12,
            "plan_version": 16,
            "findings": 24,
            "guarded_sites": 32,
            "guarded_functions": 40,
            "unsupported_sites": 48,
            "patched_functions": 56,
            "code_size": 64,
            "trampoline_count": 72,
            "publication_outcome": 80,
            "publication_receipt_version": 84,
            "publication_receipt_complete": 88,
            "publication_namespace_disposition": 92,
            "publication_guarantee_flags": 96,
            "publication_operand_binding": 100,
        }.items():
            with self.subTest(struct="result", field=field):
                self.assertEqual(
                    getattr(abi.NeverDSanitizeResultV1, field).offset,
                    expected,
                )

        # These are the C ABI's documented short-struct prefixes.  Keeping the
        # offsets explicit makes append-only drift visible to Python callers.
        self.assertEqual(ctypes.sizeof(ctypes.c_size_t), 8)
        self.assertEqual(
            abi.NeverDSanitizeResultV1.status.offset + ctypes.sizeof(ctypes.c_uint32),
            16,
        )

        spec = abi.FUNCTION_SPECS["neverd_session_sanitize"]
        self.assertEqual(
            spec.c_arguments,
            (
                "neverd_session_t",
                "const char *",
                "const neverd_sanitize_options_v1 *",
                "neverd_sanitize_result_v1 *",
            ),
        )
        self.assertIs(spec.ownership, abi.Ownership.VALUE)
        self.assertIs(
            abi.FUNCTION_SPECS["neverd_sanitize_status_name"].ownership,
            abi.Ownership.BORROWED_STRING,
        )
        self.assertIs(
            abi.FUNCTION_SPECS["neverd_sanitize_status_name"].argtypes[0],
            ctypes.c_uint32,
        )
        version_spec = abi.FUNCTION_SPECS["neverd_sanitize_publication_abi_version"]
        self.assertEqual(version_spec.c_arguments, ())
        self.assertIs(version_spec.restype, ctypes.c_uint32)

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

    def test_safety_options_mirror_the_versioned_c_layout(self) -> None:
        from neverd_plugin import abi

        self.assertIn("NeverDSafetyOptions", abi.__all__)

        self.assertEqual(
            [name for name, _ctype in abi.NeverDSafetyOptions._fields_],
            [
                "struct_size",
                "max_paths",
                "max_steps",
                "max_loop",
                "solver_conflicts",
                "sinks_path",
                "sources_path",
                "max_call_depth",
                "max_summary_iterations",
            ],
        )
        if ctypes.sizeof(ctypes.c_void_p) == 8:
            self.assertEqual(ctypes.sizeof(abi.NeverDSafetyOptions), 56)
            self.assertEqual(abi.NeverDSafetyOptions.solver_conflicts.offset, 24)
            self.assertEqual(abi.NeverDSafetyOptions.sinks_path.offset, 32)
            self.assertEqual(abi.NeverDSafetyOptions.max_call_depth.offset, 48)
            self.assertEqual(
                abi.NeverDSafetyOptions.max_summary_iterations.offset,
                52,
            )


    def test_lowir_concolic_structs_mirror_the_v1_c_layout(self) -> None:
        from neverd_plugin import abi

        self.assertEqual(
            [name for name, _ctype in abi.NeverDLowIRConcolicRegisterSeedV1._fields_],
            ["offset", "value", "bytes", "reserved"],
        )
        self.assertEqual(
            [name for name, _ctype in abi.NeverDLowIRConcolicOptionsV1._fields_],
            [
                "struct_size",
                "register_seeds",
                "register_seed_count",
                "max_steps",
                "max_block_visits",
                "max_loop_iterations",
                "max_flip_attempts",
                "max_candidates",
                "reserved",
                "solver_max_conflicts",
                "solver_max_propagations",
                "solver_max_watch_visits",
                "solver_max_gates",
            ],
        )
        if ctypes.sizeof(ctypes.c_void_p) != 8:
            self.skipTest("the published Python SDK supports 64-bit hosts")

        self.assertEqual(ctypes.sizeof(abi.NeverDLowIRConcolicRegisterSeedV1), 24)
        self.assertEqual(ctypes.alignment(abi.NeverDLowIRConcolicRegisterSeedV1), 8)
        self.assertEqual(ctypes.sizeof(abi.NeverDLowIRConcolicOptionsV1), 80)
        self.assertEqual(ctypes.alignment(abi.NeverDLowIRConcolicOptionsV1), 8)
        for field, expected in {
            "offset": 0,
            "value": 8,
            "bytes": 16,
            "reserved": 20,
        }.items():
            with self.subTest(struct="seed", field=field):
                self.assertEqual(
                    getattr(abi.NeverDLowIRConcolicRegisterSeedV1, field).offset,
                    expected,
                )
        for field, expected in {
            "struct_size": 0,
            "register_seeds": 8,
            "register_seed_count": 16,
            "max_steps": 24,
            "max_block_visits": 28,
            "max_loop_iterations": 32,
            "max_flip_attempts": 36,
            "max_candidates": 40,
            "reserved": 44,
            "solver_max_conflicts": 48,
            "solver_max_propagations": 56,
            "solver_max_watch_visits": 64,
            "solver_max_gates": 72,
        }.items():
            with self.subTest(struct="options", field=field):
                self.assertEqual(
                    getattr(abi.NeverDLowIRConcolicOptionsV1, field).offset,
                    expected,
                )

        spec = abi.FUNCTION_SPECS["neverd_lowir_concolic_json_v1"]
        self.assertEqual(
            spec.c_arguments,
            (
                "neverd_session_t",
                "neverd_va_t",
                "const neverd_lowir_concolic_options_v1 *",
            ),
        )
        self.assertIs(spec.ownership, abi.Ownership.OWNED_STRING)


    def test_every_exported_c_function_has_a_python_signature(self) -> None:
        from neverd_plugin import abi

        declarations = parse_c_api_header(C_API_HEADER)
        self.assertEqual(set(abi.FUNCTION_SPECS), set(declarations))
        for name, (result, arguments) in declarations.items():
            with self.subTest(name=name):
                spec = abi.FUNCTION_SPECS[name]
                self.assertEqual(spec.c_result, result)
                self.assertEqual(spec.c_arguments, arguments)

    def test_translation_structs_mirror_the_append_only_c_layouts(self) -> None:
        from neverd_plugin import abi

        self.assertIs(
            dict(abi.NeverDTranslateObjectRequestV1._fields_)["object_format"],
            ctypes.c_uint32,
        )
        result_fields = dict(abi.NeverDTranslateObjectResultV1._fields_)
        for name in ("error_code", "object_format", "semantic_stop", "semantic_proof"):
            with self.subTest(name=name):
                self.assertIs(result_fields[name], ctypes.c_uint32)

        self.assertEqual(
            [name for name, _ctype in abi.NeverDTranslateObjectRequestV1._fields_],
            [
                "struct_size",
                "guest_bytes",
                "guest_bytes_size",
                "entry_pc",
                "executable_generation",
                "object_format",
                "reserved",
            ],
        )
        self.assertEqual(
            [name for name, _ctype in abi.NeverDTranslateObjectResultV1._fields_],
            [
                "struct_size",
                "ok",
                "error_code",
                "error_message",
                "object_bytes",
                "object_size",
                "object_format",
                "guest_entry_pc",
                "guest_instruction_count",
                "guest_byte_count",
                "executable_generation",
                "block_ir_symbol",
                "block_object_symbol",
                "host_triple",
                "host_cpu",
                "host_target_identity",
                "runtime_registry_identity",
                "request_cache_key",
                "artifact_cache_key",
                "translation_cache_identity",
                "semantic_changed",
                "semantic_rewrites",
                "semantic_search_work",
                "semantic_proof_queries",
                "semantic_proof_conflicts",
                "semantic_proof_propagations",
                "semantic_proof_watch_visits",
                "semantic_function_pass_invocations",
                "semantic_max_rounds",
                "semantic_stop",
                "semantic_proof",
                "llvm_optimization_pipeline_ran",
                "object_cache_identity_version",
                "object_pipeline_schema_version",
            ],
        )

    def test_translation_struct_layout_is_frozen_on_supported_64_bit_abi(
        self,
    ) -> None:
        from neverd_plugin import abi

        if ctypes.sizeof(ctypes.c_void_p) != 8:
            self.skipTest("the published Python SDK supports 64-bit hosts")

        request_offsets = {
            "struct_size": 0,
            "guest_bytes": 8,
            "guest_bytes_size": 16,
            "entry_pc": 24,
            "executable_generation": 32,
            "object_format": 40,
            "reserved": 44,
        }
        result_offsets = {
            "struct_size": 0,
            "ok": 8,
            "error_code": 12,
            "error_message": 16,
            "object_bytes": 24,
            "object_size": 32,
            "object_format": 40,
            "guest_entry_pc": 48,
            "guest_instruction_count": 56,
            "guest_byte_count": 64,
            "executable_generation": 72,
            "block_ir_symbol": 80,
            "block_object_symbol": 88,
            "host_triple": 96,
            "host_cpu": 104,
            "host_target_identity": 112,
            "runtime_registry_identity": 120,
            "request_cache_key": 128,
            "artifact_cache_key": 136,
            "translation_cache_identity": 144,
            "semantic_changed": 152,
            "semantic_rewrites": 160,
            "semantic_search_work": 168,
            "semantic_proof_queries": 176,
            "semantic_proof_conflicts": 184,
            "semantic_proof_propagations": 192,
            "semantic_proof_watch_visits": 200,
            "semantic_function_pass_invocations": 208,
            "semantic_max_rounds": 216,
            "semantic_stop": 220,
            "semantic_proof": 224,
            "llvm_optimization_pipeline_ran": 228,
            "object_cache_identity_version": 232,
            "object_pipeline_schema_version": 236,
        }

        self.assertEqual(ctypes.sizeof(abi.NeverDTranslateObjectRequestV1), 48)
        self.assertEqual(ctypes.alignment(abi.NeverDTranslateObjectRequestV1), 8)
        self.assertEqual(ctypes.sizeof(abi.NeverDTranslateObjectResultV1), 240)
        self.assertEqual(ctypes.alignment(abi.NeverDTranslateObjectResultV1), 8)
        for field, expected in request_offsets.items():
            with self.subTest(struct="request", field=field):
                self.assertEqual(
                    getattr(abi.NeverDTranslateObjectRequestV1, field).offset,
                    expected,
                )
        for field, expected in result_offsets.items():
            with self.subTest(struct="result", field=field):
                self.assertEqual(
                    getattr(abi.NeverDTranslateObjectResultV1, field).offset,
                    expected,
                )


if __name__ == "__main__":
    unittest.main()
