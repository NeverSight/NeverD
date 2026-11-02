from __future__ import annotations

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from scripts import check_python_plugin_sdk as audit
from scripts import generate_python_plugin_limits as limits


class PythonPluginSDKAuditTests(unittest.TestCase):
    def test_generated_limits_match_native_authorities(self) -> None:
        self.assertEqual(limits.check(), [])

    def test_native_limit_change_invalidates_generated_binding(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for relative in {path for path, _, _ in limits.BINDINGS}:
                destination = root / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text((limits.ROOT / relative).read_text())
            output = root / limits.OUTPUT
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(limits.render(root))
            native = root / "include/neverd/sdk/Limits.h"
            source, count = re.subn(
                r"(kMaxSourcePatchCompilerArguments\s*=\s*)256\b",
                r"\g<1>257",
                native.read_text(),
            )
            self.assertEqual(count, 1)
            native.write_text(source)
            self.assertTrue(limits.check(root))
            self.assertIn(
                "MAX_SOURCE_PATCH_COMPILER_ARGUMENTS: Final[int] = 257",
                limits.render(root),
            )
            output.write_text(limits.render(root))
            self.assertEqual(limits.check(root), [])

    def test_limit_generator_accepts_native_integer_arithmetic(self) -> None:
        self.assertEqual(limits.integer_expression("64ULL * 1024 * 1024"), 67108864)
        self.assertEqual(limits.integer_expression("(1u << 16) + 0x10UL"), 65552)
        self.assertEqual(limits.integer_expression("20'000'000"), 20000000)
        self.assertEqual(limits.integer_expression("uint64_t{1} << 26"), 67108864)
        self.assertEqual(limits.integer_expression("size_t(1) << 22"), 4194304)

    def test_model_default_is_distinct_from_factory_overrides(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "model.h").write_text(
                "struct Other { unsigned Max = 7; };\n"
                "struct Options {\n"
                "  unsigned Max = 64;\n"
                "  static Options small() {\n"
                "    Options Result; Result.Max = 1; return Result;\n"
                "  }\n"
                "};\n"
            )
            with mock.patch.object(
                limits, "BINDINGS", (("model.h", "Options::Max", "MAX"),)
            ):
                self.assertIn("MAX: Final[int] = 64", limits.render(root))

    def test_limit_generator_rejects_unaudited_expressions(self) -> None:
        for expression in ("other_limit", "max(1, 2)", "True", "1.5", "0"):
            with self.subTest(expression=expression), self.assertRaises(ValueError):
                limits.integer_expression(expression)

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
        # Dependency notice trees are copied verbatim. SDK package sources must
        # still use the explicit file lists below so caches cannot be shipped.
        directory_sources = re.findall(r'\bcopy_directory\s+("[^"]*"|\S+)', source)
        for directory in directory_sources:
            # Generated Windows types carry original dependency notices in
            # this exact subtree; their source/cache roots remain excluded.
            if directory == '"${NEVERD_WINDOWS_TYPES_DIRECTORY}/licenses"':
                continue
            self.assertRegex(directory, r'^"\$\{CMAKE_SOURCE_DIR\}/LICENSES/[^"\r\n]+"$')
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
            "_limits.py",
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

    def test_source_patch_dependency_layout_is_audited_with_its_pointer(self) -> None:
        import ctypes

        from neverd_plugin import abi

        errors: list[str] = []
        audit.check_source_patch_abi(errors)
        self.assertEqual(errors, [])

        class WrongDependency(ctypes.Structure):
            _fields_ = [
                ("library", ctypes.c_char_p),
                ("symbol", ctypes.c_char_p),
                ("kind", ctypes.c_uint64),
            ]

        with mock.patch.object(abi, "NeverDSourcePatchDependencyV1", WrongDependency):
            audit.check_source_patch_abi(errors)
        self.assertTrue(any(
            message.startswith("neverd_source_patch_dependency_v1 field/type mismatch")
            for message in errors
        ), errors)

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
