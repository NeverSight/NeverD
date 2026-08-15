from __future__ import annotations

import io
import json
import subprocess
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

from scripts import check_capabilities as capabilities


ROOT = Path(__file__).resolve().parents[2]


class CapabilitySchemaTests(unittest.TestCase):
    @staticmethod
    def capability(**overrides: object) -> dict[str, object]:
        row: dict[str, object] = {
            "id": "llvm.semantic.test-rewrite",
            "kind": "rewrite",
            "status": "experimental",
            "owner": "pass-ir",
            "targets": ["x86_64"],
            "docs": ["docs/roadmap/README.md"],
            "limitations": [],
            "tests": [],
            "public_surfaces": {
                "c": [],
                "python": [],
                "cli": [],
                "json": [],
            },
        }
        row.update(overrides)
        return row

    def test_supported_rewrite_requires_proof_evidence(self) -> None:
        manifest = {
            "schema": 1,
            "capabilities": [self.capability(status="supported")],
        }

        self.assertEqual(
            capabilities.validate_manifest(manifest, ROOT),
            [
                "missing poison_test",
                "missing proof_test",
                "missing unknown_test",
            ],
        )

    def test_manifest_must_be_an_object(self) -> None:
        self.assertEqual(
            capabilities.validate_manifest([], ROOT),
            ["manifest: expected an object"],
        )

    def test_status_vocabulary_is_closed(self) -> None:
        manifest = {
            "schema": 1,
            "capabilities": [self.capability(status="complete")],
        }

        self.assertEqual(
            capabilities.validate_manifest(manifest, ROOT),
            [
                "capabilities[0].status: expected one of experimental, "
                "supported, unsupported"
            ],
        )

    def test_schema_version_is_exactly_integer_one(self) -> None:
        for invalid in (True, 2, "1"):
            with self.subTest(schema=invalid):
                self.assertEqual(
                    capabilities.validate_manifest(
                        {"schema": invalid, "capabilities": []}, ROOT
                    ),
                    ["schema: expected integer 1"],
                )

    def test_manifest_rejects_unknown_top_level_fields(self) -> None:
        self.assertEqual(
            capabilities.validate_manifest(
                {"schema": 1, "capabilities": [], "description": "drift"},
                ROOT,
            ),
            ["unexpected top-level field: description"],
        )

    def test_manifest_requires_the_capability_collection(self) -> None:
        self.assertEqual(
            capabilities.validate_manifest({"schema": 1}, ROOT),
            ["missing top-level field: capabilities"],
        )

    def test_capability_collection_cannot_be_empty(self) -> None:
        self.assertEqual(
            capabilities.validate_manifest({"schema": 1, "capabilities": []}, ROOT),
            ["capabilities: expected at least one capability"],
        )

    def test_capability_collection_must_be_an_array(self) -> None:
        self.assertEqual(
            capabilities.validate_manifest({"schema": 1, "capabilities": {}}, ROOT),
            ["capabilities: expected an array"],
        )

    def test_each_capability_must_be_an_object(self) -> None:
        self.assertEqual(
            capabilities.validate_manifest(
                {"schema": 1, "capabilities": ["rewrite"]}, ROOT
            ),
            ["capabilities[0]: expected an object"],
        )

    def test_capability_rejects_unknown_fields(self) -> None:
        row = self.capability(proof_tests=[])
        self.assertEqual(
            capabilities.validate_manifest({"schema": 1, "capabilities": [row]}, ROOT),
            ["capabilities[0]: unexpected field proof_tests"],
        )

    def test_capability_requires_every_core_field(self) -> None:
        row = self.capability()
        del row["owner"]
        self.assertEqual(
            capabilities.validate_manifest({"schema": 1, "capabilities": [row]}, ROOT),
            ["capabilities[0]: missing owner"],
        )

    def test_required_strings_are_non_empty(self) -> None:
        for field in ("id", "kind", "owner"):
            with self.subTest(field=field):
                row = self.capability(**{field: "  "})
                self.assertEqual(
                    capabilities.validate_manifest(
                        {"schema": 1, "capabilities": [row]}, ROOT
                    ),
                    [f"capabilities[0].{field}: expected a non-empty string"],
                )

    def test_capability_arrays_have_array_type(self) -> None:
        row = self.capability(targets="x86_64")
        self.assertEqual(
            capabilities.validate_manifest({"schema": 1, "capabilities": [row]}, ROOT),
            ["capabilities[0].targets: expected an array"],
        )

    def test_capability_arrays_reject_duplicate_strings(self) -> None:
        row = self.capability(targets=["x86_64", "x86_64"])
        self.assertEqual(
            capabilities.validate_manifest({"schema": 1, "capabilities": [row]}, ROOT),
            ["capabilities[0].targets: duplicate entry x86_64"],
        )

    def test_capability_array_entries_are_non_empty_strings(self) -> None:
        row = self.capability(limitations=[" ", 7])
        self.assertEqual(
            capabilities.validate_manifest({"schema": 1, "capabilities": [row]}, ROOT),
            [
                "capabilities[0].limitations[0]: expected a non-empty string",
                "capabilities[0].limitations[1]: expected a non-empty string",
            ],
        )

    def test_capability_ids_are_unique(self) -> None:
        rows = [self.capability(), self.capability(owner="symbolic")]
        self.assertEqual(
            capabilities.validate_manifest({"schema": 1, "capabilities": rows}, ROOT),
            ["duplicate capability id: llvm.semantic.test-rewrite"],
        )

    def test_required_capability_ids_cannot_be_removed(self) -> None:
        manifest = {"schema": 1, "capabilities": [self.capability()]}
        self.assertEqual(
            capabilities.validate_manifest(
                manifest,
                ROOT,
                required_capability_ids={
                    "llvm.semantic.test-rewrite",
                    "runtime.translation-contract",
                },
            ),
            ["missing required capability id: runtime.translation-contract"],
        )

    def test_public_surfaces_must_be_an_object(self) -> None:
        row = self.capability(public_surfaces=[])
        self.assertEqual(
            capabilities.validate_manifest({"schema": 1, "capabilities": [row]}, ROOT),
            ["capabilities[0].public_surfaces: expected an object"],
        )

    def test_public_surface_names_are_closed_and_required(self) -> None:
        row = self.capability(
            public_surfaces={"c": [], "python": [], "cli": [], "rust": []}
        )
        self.assertEqual(
            capabilities.validate_manifest({"schema": 1, "capabilities": [row]}, ROOT),
            [
                "capabilities[0].public_surfaces: missing json",
                "capabilities[0].public_surfaces: unexpected field rust",
            ],
        )

    def test_public_surface_values_are_arrays(self) -> None:
        row = self.capability(
            public_surfaces={
                "c": "neverd_api",
                "python": [],
                "cli": [],
                "json": [],
            }
        )
        self.assertEqual(
            capabilities.validate_manifest({"schema": 1, "capabilities": [row]}, ROOT),
            ["capabilities[0].public_surfaces.c: expected an array"],
        )

    def test_public_surface_entries_are_unique(self) -> None:
        row = self.capability(
            public_surfaces={
                "c": ["neverd_api", "neverd_api"],
                "python": [],
                "cli": [],
                "json": [],
            }
        )
        self.assertEqual(
            capabilities.validate_manifest({"schema": 1, "capabilities": [row]}, ROOT),
            ["capabilities[0].public_surfaces.c: duplicate entry neverd_api"],
        )

    def test_public_surface_entries_are_non_empty_strings(self) -> None:
        row = self.capability(
            public_surfaces={
                "c": [],
                "python": [],
                "cli": [],
                "json": ["", 3],
            }
        )
        self.assertEqual(
            capabilities.validate_manifest({"schema": 1, "capabilities": [row]}, ROOT),
            [
                "capabilities[0].public_surfaces.json[0]: expected a non-empty string",
                "capabilities[0].public_surfaces.json[1]: expected a non-empty string",
            ],
        )

    def test_public_surfaces_must_have_authoritative_declarations(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "docs").mkdir()
            (root / "docs" / "roadmap.md").write_text("# Roadmap\n", encoding="utf-8")
            row = self.capability(
                docs=["docs/roadmap.md"],
                public_surfaces={
                    "c": ["missing_c_api"],
                    "python": ["missing_python_api"],
                    "cli": ["neverd missing-command"],
                    "json": ["missing_json_api"],
                },
            )

            self.assertEqual(
                capabilities.validate_manifest(
                    {"schema": 1, "capabilities": [row]}, root
                ),
                [
                    "capabilities[0].public_surfaces.c[0]: declaration not "
                    "found: missing_c_api",
                    "capabilities[0].public_surfaces.cli[0]: declaration not "
                    "found: neverd missing-command",
                    "capabilities[0].public_surfaces.json[0]: declaration not "
                    "found: missing_json_api",
                    "capabilities[0].public_surfaces.python[0]: declaration not "
                    "found: missing_python_api",
                ],
            )

    def test_tests_must_name_a_declared_cpp_or_python_test(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "docs").mkdir()
            (root / "docs" / "roadmap.md").write_text("# Roadmap\n", encoding="utf-8")
            row = self.capability(
                docs=["docs/roadmap.md"], tests=["Suite.DoesNotExist"]
            )

            self.assertEqual(
                capabilities.validate_manifest(
                    {"schema": 1, "capabilities": [row]}, root
                ),
                [
                    "capabilities[0].tests[0]: test declaration not found: "
                    "Suite.DoesNotExist"
                ],
            )

    def test_evidence_must_be_an_object(self) -> None:
        row = self.capability(proof_test=[])
        self.assertEqual(
            capabilities.validate_manifest({"schema": 1, "capabilities": [row]}, ROOT),
            ["capabilities[0].proof_test: expected an object"],
        )

    def test_evidence_fields_are_closed_and_required(self) -> None:
        row = self.capability(
            proof_test={"target": "T", "source": "s.cpp", "name": "S.N"}
        )
        self.assertEqual(
            capabilities.validate_manifest({"schema": 1, "capabilities": [row]}, ROOT),
            [
                "capabilities[0].proof_test: missing filter",
                "capabilities[0].proof_test: unexpected field name",
            ],
        )

    def test_evidence_values_are_non_empty_strings(self) -> None:
        row = self.capability(proof_test={"target": " ", "source": 4, "filter": ""})
        self.assertEqual(
            capabilities.validate_manifest({"schema": 1, "capabilities": [row]}, ROOT),
            [
                "capabilities[0].proof_test.filter: expected a non-empty string",
                "capabilities[0].proof_test.source: expected a non-empty string",
                "capabilities[0].proof_test.target: expected a non-empty string",
            ],
        )

    def test_evidence_rejects_a_missing_source(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "docs").mkdir()
            (root / "docs" / "roadmap.md").write_text("# Roadmap\n", encoding="utf-8")
            (root / "CMakeLists.txt").write_text(
                "add_neverd_unittest(ExistingTests test.cpp)\n",
                encoding="utf-8",
            )
            row = self.capability(
                docs=["docs/roadmap.md"],
                proof_test={
                    "target": "ExistingTests",
                    "source": "unittests/missing.cpp",
                    "filter": "Suite.Exists",
                },
            )

            self.assertEqual(
                capabilities.validate_manifest(
                    {"schema": 1, "capabilities": [row]}, root
                ),
                [
                    "capabilities[0].proof_test.source: path does not exist: "
                    "unittests/missing.cpp"
                ],
            )

    def test_evidence_rejects_a_filter_absent_from_its_source(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "docs").mkdir()
            (root / "docs" / "roadmap.md").write_text("# Roadmap\n", encoding="utf-8")
            (root / "unittests").mkdir()
            (root / "unittests" / "test.cpp").write_text(
                "TEST(Suite, Other) {}\n", encoding="utf-8"
            )
            (root / "CMakeLists.txt").write_text(
                "add_neverd_unittest(ExistingTests unittests/test.cpp)\n",
                encoding="utf-8",
            )
            row = self.capability(
                docs=["docs/roadmap.md"],
                proof_test={
                    "target": "ExistingTests",
                    "source": "unittests/test.cpp",
                    "filter": "Suite.Exists",
                },
            )

            self.assertEqual(
                capabilities.validate_manifest(
                    {"schema": 1, "capabilities": [row]}, root
                ),
                [
                    "capabilities[0].proof_test.filter: not found in "
                    "unittests/test.cpp: Suite.Exists"
                ],
            )

    def test_evidence_rejects_a_missing_cmake_target(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "docs").mkdir()
            (root / "docs" / "roadmap.md").write_text("# Roadmap\n", encoding="utf-8")
            (root / "unittests").mkdir()
            (root / "unittests" / "test.cpp").write_text(
                "TEST(Suite, Exists) {}\n", encoding="utf-8"
            )
            (root / "CMakeLists.txt").write_text(
                "add_neverd_unittest(OtherTests unittests/test.cpp)\n",
                encoding="utf-8",
            )
            row = self.capability(
                docs=["docs/roadmap.md"],
                proof_test={
                    "target": "MissingTests",
                    "source": "unittests/test.cpp",
                    "filter": "Suite.Exists",
                },
            )

            self.assertEqual(
                capabilities.validate_manifest(
                    {"schema": 1, "capabilities": [row]}, root
                ),
                [
                    "capabilities[0].proof_test.target: CMake target does not "
                    "exist: MissingTests"
                ],
            )

    def test_commented_test_declarations_are_not_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "docs").mkdir()
            (root / "docs" / "roadmap.md").write_text("# Roadmap\n", encoding="utf-8")
            (root / "unittests").mkdir()
            (root / "unittests" / "test.cpp").write_text(
                "// TEST(Suite, Claimed) {}\nTEST(Suite, Real) {}\n",
                encoding="utf-8",
            )
            (root / "CMakeLists.txt").write_text(
                "add_neverd_unittest(ExistingTests unittests/test.cpp)\n",
                encoding="utf-8",
            )
            row = self.capability(
                docs=["docs/roadmap.md"],
                proof_test={
                    "target": "ExistingTests",
                    "source": "unittests/test.cpp",
                    "filter": "Suite.Claimed",
                },
            )

            self.assertEqual(
                capabilities.validate_manifest(
                    {"schema": 1, "capabilities": [row]}, root
                ),
                [
                    "capabilities[0].proof_test.filter: not found in "
                    "unittests/test.cpp: Suite.Claimed"
                ],
            )

    def test_cmake_target_inventory_never_reads_build_directories(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "CMakeLists.txt").write_text(
                "add_neverd_unittest(SourceTests test.cpp)\n", encoding="utf-8"
            )
            build = root / "out"
            build.mkdir()
            (build / "CMakeCache.txt").write_text(
                "CMAKE_CACHE_MAJOR_VERSION:INTERNAL=3\n", encoding="utf-8"
            )
            (build / "CMakeLists.txt").write_bytes(b"\xff\xfe")

            self.assertEqual(
                capabilities.collect_cmake_targets(root), frozenset({"SourceTests"})
            )

    def test_cmake_target_inventory_excludes_git_ignored_files(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(("git", "init", "-q"), cwd=root, check=True)
            (root / ".gitignore").write_text("ignored/\n", encoding="utf-8")
            (root / "CMakeLists.txt").write_text(
                "add_neverd_unittest(SourceTests test.cpp)\n", encoding="utf-8"
            )
            ignored = root / "ignored"
            ignored.mkdir()
            (ignored / "CMakeLists.txt").write_bytes(b"\xff\xfe")

            self.assertEqual(
                capabilities.collect_cmake_targets(root), frozenset({"SourceTests"})
            )

    def test_commented_cmake_targets_are_not_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "CMakeLists.txt").write_text(
                "#[[\n"
                "add_neverd_unittest(CommentedTests test.cpp)\n"
                "]]\n"
                "add_neverd_unittest(SourceTests test.cpp)\n",
                encoding="utf-8",
            )

            self.assertEqual(
                capabilities.collect_cmake_targets(root), frozenset({"SourceTests"})
            )

    def test_documentation_paths_must_exist(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            row = self.capability(docs=["docs/missing.md"])
            self.assertEqual(
                capabilities.validate_manifest(
                    {"schema": 1, "capabilities": [row]}, root
                ),
                ["capabilities[0].docs[0]: path does not exist: docs/missing.md"],
            )

    def test_paths_cannot_escape_the_repository(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            parent = Path(directory)
            root = parent / "repo"
            root.mkdir()
            (parent / "outside.md").write_text("outside\n", encoding="utf-8")
            row = self.capability(docs=["../outside.md"])

            self.assertEqual(
                capabilities.validate_manifest(
                    {"schema": 1, "capabilities": [row]}, root
                ),
                [
                    "capabilities[0].docs[0]: path must be a canonical "
                    "repository-relative POSIX path: ../outside.md"
                ],
            )


class CapabilityCommandTests(unittest.TestCase):
    def test_main_reports_the_validated_capability_count(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "docs").mkdir()
            (root / "docs" / "roadmap.md").write_text("# Roadmap\n", encoding="utf-8")
            manifest_path = root / "docs" / "capabilities.json"
            manifest_path.write_text(
                json.dumps(
                    {
                        "schema": 1,
                        "capabilities": [
                            CapabilitySchemaTests.capability(docs=["docs/roadmap.md"])
                        ],
                    }
                ),
                encoding="utf-8",
            )
            stdout = io.StringIO()

            with redirect_stdout(stdout):
                result = capabilities.main([str(manifest_path)], root=root)

            self.assertEqual(result, 0)
            self.assertEqual(
                stdout.getvalue(), "capability manifest valid: 1 capability\n"
            )


class RepositoryCapabilityTests(unittest.TestCase):
    def test_repository_manifest_is_honest_and_executable(self) -> None:
        document = json.loads(
            (ROOT / "docs" / "capabilities.json").read_text(encoding="utf-8")
        )

        self.assertEqual(capabilities.validate_manifest(document, ROOT), [])
        self.assertEqual(
            {row["id"]: row["status"] for row in document["capabilities"]},
            {
                "debug.hardware": "unsupported",
                "debug.local": "unsupported",
                "debug.remote": "unsupported",
                "exception.itanium.ada-d": "experimental",
                "exception.rewrite.end-to-end": "unsupported",
                "semantic.mba.derivation": "supported",
                "semantic.synthesis.candidate": "supported",
                "symbolic.execution.path-exploration": "experimental",
                "llvm.semantic.synthesis-rewrite": "supported",
                "translation.executable-engine": "experimental",
                "translation.runtime-contract": "experimental",
            },
        )
        no_surfaces = {"c": [], "python": [], "cli": [], "json": []}
        expected_surfaces = {
            "debug.hardware": no_surfaces,
            "debug.local": no_surfaces,
            "debug.remote": no_surfaces,
            "exception.itanium.ada-d": no_surfaces,
            "exception.rewrite.end-to-end": no_surfaces,
            "semantic.mba.derivation": {
                "c": ["neverd_simplify_expr"],
                "python": ["simplify_expression"],
                "cli": ["neverd simplify"],
                "json": ["neverd_simplify_expr_json"],
            },
            "semantic.synthesis.candidate": {
                "c": ["neverd_synthesize_expr"],
                "python": ["synthesize_expression"],
                "cli": ["neverd simplify --synthesize"],
                "json": ["neverd_synthesize_expr_json_v1"],
            },
            "symbolic.execution.path-exploration": {
                "c": [],
                "python": [],
                "cli": ["neverd sym-explore"],
                "json": [],
            },
            "translation.executable-engine": no_surfaces,
            "translation.runtime-contract": {
                "c": ["neverd_translate_x86_64_block_to_aarch64_object_v1"],
                "python": ["translate_x86_64_block_to_aarch64_object"],
                "cli": ["neverd translate-object"],
                "json": [],
            },
            "llvm.semantic.synthesis-rewrite": {
                "c": ["neverd_synthesize_expr", "neverd_optimize_llvm_ir"],
                "python": ["synthesize_expression", "optimize_llvm_ir"],
                "cli": ["neverd simplify --synthesize", "neverd optimize-ir"],
                "json": [
                    "neverd_synthesize_expr_json_v1",
                    "neverd_optimize_llvm_ir_json_v1",
                ],
            },
        }
        for row in document["capabilities"]:
            self.assertEqual(row["public_surfaces"], expected_surfaces[row["id"]])

    def test_ci_runs_the_capability_checker_before_configure(self) -> None:
        workflow = (ROOT / ".github" / "workflows" / "ci.yml").read_text(
            encoding="utf-8"
        )
        test_command = "scripts.tests.test_check_capabilities"
        check_command = '"$PYTHON" scripts/check_capabilities.py'

        self.assertIn(test_command, workflow)
        self.assertIn(check_command, workflow)
        self.assertLess(workflow.index(test_command), workflow.index(check_command))
        self.assertLess(workflow.index(check_command), workflow.index("cmake -S"))

    def test_cmake_exposes_the_checker_without_the_plugin_host(self) -> None:
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        interpreter = "find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)"

        self.assertIn(interpreter, cmake)
        self.assertLess(cmake.index(interpreter), cmake.index("add_subdirectory(lib)"))
        self.assertIn("add_custom_target(check-neverd-capabilities", cmake)
        self.assertIn("${Python3_EXECUTABLE}", cmake)
        self.assertIn(
            "find_package(Python3 3.10 REQUIRED COMPONENTS Development.Embed)",
            cmake,
        )


if __name__ == "__main__":
    unittest.main()
