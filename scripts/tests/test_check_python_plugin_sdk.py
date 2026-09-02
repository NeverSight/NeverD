from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from scripts import check_python_plugin_sdk as audit


class PythonPluginSDKAuditTests(unittest.TestCase):
    def test_audit_is_directly_runnable_without_pythonpath(self) -> None:
        environment = os.environ.copy()
        environment.pop("PYTHONPATH", None)
        result = subprocess.run(
            [
                sys.executable,
                str(audit.ROOT / "scripts" / "check_python_plugin_sdk.py"),
                "--without-workflows",
            ],
            cwd=Path(__file__).resolve().parents[2],
            env=environment,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("audit passed", result.stdout)

    def test_build_stages_an_installable_sdk_without_generated_caches(self) -> None:
        source = (audit.ROOT / "lib" / "sdk" / "CMakeLists.txt").read_text(
            encoding="utf-8"
        )
        self.assertNotIn("copy_directory", source)
        self.assertIn("$<TARGET_FILE_DIR:neverd_shared>/sdk", source)
        self.assertNotIn("${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/sdk", source)
        for staged_path in (
            '"${NEVERD_STAGED_SDK_DIRECTORY}"',
            '"${NEVERD_STAGED_SDK_DIRECTORY}/neverd/sdk"',
            '"${NEVERD_STAGED_SDK_DIRECTORY}/neverd"',
            '"${NEVERD_STAGED_SDK_DIRECTORY}/python"',
            '"${NEVERD_STAGED_SDK_DIRECTORY}/python/neverd_plugin"',
            '"${NEVERD_STAGED_SDK_DIRECTORY}/python/examples"',
        ):
            with self.subTest(staged_path=staged_path):
                self.assertIn(staged_path, source)
        for required in (
            "LICENSE",
            "README.md",
            "pyproject.toml",
            "py.typed",
            "analysis_report.py",
            "minimal.py",
            "semantic_optimizer.py",
        ):
            with self.subTest(required=required):
                self.assertIn(required, source)

        consumer = (
            audit.ROOT / "unittests" / "translate" / "CMakeLists.txt"
        ).read_text(encoding="utf-8")
        self.assertIn('"$<TARGET_FILE_DIR:neverd_shared>/sdk"', consumer)
        self.assertNotIn("${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/sdk", consumer)

    def test_header_parser_ignores_the_export_macro_definition(self) -> None:
        declarations = audit.parse_c_api(
            """
#define NEVERD_API __attribute__((visibility("default")))
NEVERD_API neverd_session_t neverd_session_create(void);
"""
        )
        self.assertEqual(
            declarations,
            {"neverd_session_create": ("neverd_session_t", ())},
        )

    def test_multiline_header_parser_preserves_types_and_order(self) -> None:
        declarations = audit.parse_c_api(
            """
NEVERD_API const char *neverd_name(neverd_session_t Session);
NEVERD_API int neverd_read(
    neverd_session_t Session, neverd_va_t Address,
    unsigned char *Buffer, int Size);
"""
        )
        self.assertEqual(
            declarations,
            {
                "neverd_name": ("const char *", ("neverd_session_t",)),
                "neverd_read": (
                    "int",
                    (
                        "neverd_session_t",
                        "neverd_va_t",
                        "unsigned char *",
                        "int",
                    ),
                ),
            },
        )

    def test_named_enum_parser_ignores_comments_and_preserves_values(self) -> None:
        entries = audit.parse_c_enum(
            """
typedef enum {
  NEVERD_SAMPLE_ZERO = 0,
  /* reserved = 8 */
  NEVERD_SAMPLE_NINE = 9,
} neverd_sample_t;
""",
            "neverd_sample_t",
        )
        self.assertEqual(
            entries,
            {"NEVERD_SAMPLE_ZERO": 0, "NEVERD_SAMPLE_NINE": 9},
        )

    def test_static_name_functions_are_explicitly_borrowed(self) -> None:
        from neverd_plugin.abi import Ownership

        self.assertIs(
            audit.expected_ownership("neverd_proof_status_name", "const char *"),
            Ownership.BORROWED_STRING,
        )
        self.assertIs(
            audit.expected_ownership("neverd_sanitize_status_name", "const char *"),
            Ownership.BORROWED_STRING,
        )
        self.assertIs(
            audit.expected_ownership("neverd_version", "const char *"),
            Ownership.OWNED_STRING,
        )

    def test_repository_abi_enum_and_versions_do_not_drift(self) -> None:
        errors: list[str] = []
        audit.check_abi(errors)
        audit.check_plugin_enums(errors)
        audit.check_output_languages(errors)
        audit.check_concolic_abi(errors)
        audit.check_versions(errors)
        self.assertEqual(errors, [])

    def test_concolic_seed_ceiling_cannot_drift_from_public_header(self) -> None:
        from neverd_plugin import api

        errors: list[str] = []
        original = api._LOWIR_CONCOLIC_MAX_REGISTER_SEEDS_V1
        api._LOWIR_CONCOLIC_MAX_REGISTER_SEEDS_V1 = original + 1
        try:
            audit.check_concolic_abi(errors)
        finally:
            api._LOWIR_CONCOLIC_MAX_REGISTER_SEEDS_V1 = original

        self.assertEqual(
            errors,
            ["concolic register-seed ceiling mismatch: header=4096, Python=4097"],
        )

    def test_concolic_v1_seed_ceiling_is_frozen_when_both_surfaces_drift(
        self,
    ) -> None:
        from neverd_plugin import api

        source = audit.SYMBOLIC_HEADER.read_text(encoding="utf-8")
        mutated = source.replace(
            "#define NEVERD_LOWIR_CONCOLIC_MAX_REGISTER_SEEDS_V1 4096u",
            "#define NEVERD_LOWIR_CONCOLIC_MAX_REGISTER_SEEDS_V1 4097u",
        )
        self.assertNotEqual(mutated, source)

        with tempfile.TemporaryDirectory() as directory:
            header = Path(directory) / "NeverDCAPISymbolic.h"
            header.write_text(mutated, encoding="utf-8")
            errors: list[str] = []
            with (
                mock.patch.object(audit, "SYMBOLIC_HEADER", header),
                mock.patch.object(
                    api,
                    "_LOWIR_CONCOLIC_MAX_REGISTER_SEEDS_V1",
                    4097,
                ),
            ):
                audit.check_concolic_abi(errors)

        self.assertEqual(
            errors,
            [
                "concolic v1 register-seed ceiling changed: "
                "expected=4096, header=4097, Python=4097"
            ],
        )

    def test_multilingual_documentation_is_complete_and_examples_match(self) -> None:
        errors: list[str] = []
        audit.check_documentation(errors)
        self.assertEqual(errors, [])


if __name__ == "__main__":
    unittest.main()
