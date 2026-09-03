from __future__ import annotations

import io
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

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

    @staticmethod
    def evidence(**overrides: object) -> dict[str, object]:
        row: dict[str, object] = {
            "target": "ExistingTests",
            "source": "unittests/test.cpp",
            "filter": "Suite.Exists",
            "platforms": ["darwin", "linux", "windows"],
            "runner": "gtest",
        }
        row.update(overrides)
        return row

    def validate_gtest_fixture(
        self,
        source_text: str,
        test_filter: str,
        *,
        target: str = "ExistingTests",
        cmake_text: str = ("add_neverd_unittest(ExistingTests unittests/test.cpp)\n"),
        extra_sources: dict[str, str] | None = None,
    ) -> list[str]:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "docs").mkdir()
            (root / "docs" / "roadmap.md").write_text("# Roadmap\n", encoding="utf-8")
            (root / "unittests").mkdir()
            (root / "unittests" / "test.cpp").write_text(source_text, encoding="utf-8")
            for source, text in (extra_sources or {}).items():
                path = root / source
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding="utf-8")
            (root / "CMakeLists.txt").write_text(cmake_text, encoding="utf-8")
            row = self.capability(
                docs=["docs/roadmap.md"],
                proof_test=self.evidence(target=target, filter=test_filter),
            )
            return capabilities.validate_manifest(
                {"schema": 2, "capabilities": [row]}, root
            )

    def test_supported_rewrite_requires_proof_evidence(self) -> None:
        manifest = {
            "schema": 2,
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
            "schema": 2,
            "capabilities": [self.capability(status="complete")],
        }

        self.assertEqual(
            capabilities.validate_manifest(manifest, ROOT),
            [
                "capabilities[0].status: expected one of experimental, "
                "supported, unsupported"
            ],
        )

    def test_kind_vocabulary_is_closed(self) -> None:
        row = self.capability(kind="implementation")

        self.assertEqual(
            capabilities.validate_manifest({"schema": 2, "capabilities": [row]}, ROOT),
            [
                "capabilities[0].kind: expected one of analysis, debugging, "
                "derivation, execution, rewrite, synthesis, translation"
            ],
        )

    def test_supported_capability_requires_current_platform_evidence(self) -> None:
        row = self.capability(kind="analysis", status="supported", tests=[])
        platform = capabilities.current_test_platform()

        self.assertEqual(
            capabilities.validate_manifest({"schema": 2, "capabilities": [row]}, ROOT),
            [
                "capabilities[0]: supported capability requires tests or "
                f"proof_test evidence executable on {platform}"
            ],
        )

    def test_required_capability_kind_cannot_bypass_supported_proof(self) -> None:
        row = self.capability(
            id="llvm.semantic.synthesis-rewrite",
            kind="analysis",
            status="supported",
            tests=[],
        )
        diagnostics = capabilities.validate_manifest(
            {"schema": 2, "capabilities": [row]}, ROOT
        )

        self.assertIn(
            "capabilities[0].kind: llvm.semantic.synthesis-rewrite requires "
            "kind rewrite",
            diagnostics,
        )

    def test_supported_primary_evidence_must_be_statically_executable_here(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "docs").mkdir()
            (root / "docs" / "roadmap.md").write_text("# Roadmap\n", encoding="utf-8")
            (root / "unittests").mkdir()
            (root / "unittests" / "test.cpp").write_text(
                "TEST(Suite, Exists) {}\n", encoding="utf-8"
            )
            (root / "CMakeLists.txt").write_text(
                "add_neverd_unittest(ExistingTests unittests/test.cpp)\n",
                encoding="utf-8",
            )
            current = capabilities.current_test_platform()
            other = next(
                platform
                for platform in capabilities.TEST_PLATFORMS
                if platform != current
            )
            current_evidence = self.evidence(platforms=[current])
            other_evidence = self.evidence(platforms=[other])
            base = {
                "kind": "analysis",
                "status": "supported",
                "docs": ["docs/roadmap.md"],
            }

            for evidence_field in ("tests", "proof_test"):
                with self.subTest(valid_primary=evidence_field):
                    value: object = (
                        [current_evidence]
                        if evidence_field == "tests"
                        else current_evidence
                    )
                    row = self.capability(**base, **{evidence_field: value})
                    self.assertEqual(
                        capabilities.validate_manifest(
                            {"schema": 2, "capabilities": [row]}, root
                        ),
                        [],
                    )

            expected = (
                "capabilities[0]: supported capability requires tests or "
                f"proof_test evidence executable on {current}"
            )
            for overrides in (
                {"tests": [other_evidence]},
                {"unknown_test": current_evidence},
            ):
                with self.subTest(non_primary_or_other=overrides):
                    row = self.capability(**base, **overrides)
                    self.assertEqual(
                        capabilities.validate_manifest(
                            {"schema": 2, "capabilities": [row]}, root
                        ),
                        [expected],
                    )

            invalid = self.evidence(source="unittests/missing.cpp", platforms=[current])
            row = self.capability(**base, tests=[invalid])
            diagnostics = capabilities.validate_manifest(
                {"schema": 2, "capabilities": [row]}, root
            )
            self.assertIn(expected, diagnostics)
        self.assertIn(
            "capabilities[0]: supported capability requires tests or "
            f"proof_test evidence executable on {capabilities.current_test_platform()}",
            diagnostics,
        )

    def test_schema_version_is_exactly_integer_two(self) -> None:
        for invalid in (True, 1, "2"):
            with self.subTest(schema=invalid):
                self.assertEqual(
                    capabilities.validate_manifest(
                        {"schema": invalid, "capabilities": []}, ROOT
                    ),
                    ["schema: expected integer 2"],
                )

    def test_json_type_mutations_never_crash_and_are_deterministic(self) -> None:
        scalar_mutations = (None, [], {})
        capability_fields = (
            "id",
            "kind",
            "status",
            "owner",
            "targets",
            "docs",
            "limitations",
            "tests",
            "public_surfaces",
            "proof_test",
            "unknown_test",
            "poison_test",
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "docs" / "roadmap").mkdir(parents=True)
            (root / "docs" / "roadmap" / "README.md").write_text(
                "# Roadmap\n", encoding="utf-8"
            )
            for field in capability_fields:
                for mutation in scalar_mutations:
                    with self.subTest(field=field, mutation=mutation):
                        document = {
                            "schema": 2,
                            "capabilities": [self.capability(**{field: mutation})],
                        }
                        first = capabilities.validate_manifest(document, root)
                        second = capabilities.validate_manifest(document, root)
                        self.assertIsInstance(first, list)
                        self.assertEqual(first, second)

    def test_manifest_rejects_unknown_top_level_fields(self) -> None:
        self.assertEqual(
            capabilities.validate_manifest(
                {"schema": 2, "capabilities": [], "description": "drift"},
                ROOT,
            ),
            ["unexpected top-level field: description"],
        )

    def test_manifest_requires_the_capability_collection(self) -> None:
        self.assertEqual(
            capabilities.validate_manifest({"schema": 2}, ROOT),
            ["missing top-level field: capabilities"],
        )

    def test_capability_collection_cannot_be_empty(self) -> None:
        self.assertEqual(
            capabilities.validate_manifest({"schema": 2, "capabilities": []}, ROOT),
            ["capabilities: expected at least one capability"],
        )

    def test_capability_collection_must_be_an_array(self) -> None:
        self.assertEqual(
            capabilities.validate_manifest({"schema": 2, "capabilities": {}}, ROOT),
            ["capabilities: expected an array"],
        )

    def test_each_capability_must_be_an_object(self) -> None:
        self.assertEqual(
            capabilities.validate_manifest(
                {"schema": 2, "capabilities": ["rewrite"]}, ROOT
            ),
            ["capabilities[0]: expected an object"],
        )

    def test_capability_rejects_unknown_fields(self) -> None:
        row = self.capability(proof_tests=[])
        self.assertEqual(
            capabilities.validate_manifest({"schema": 2, "capabilities": [row]}, ROOT),
            ["capabilities[0]: unexpected field proof_tests"],
        )

    def test_capability_requires_every_core_field(self) -> None:
        row = self.capability()
        del row["owner"]
        self.assertEqual(
            capabilities.validate_manifest({"schema": 2, "capabilities": [row]}, ROOT),
            ["capabilities[0]: missing owner"],
        )

    def test_required_strings_are_non_empty(self) -> None:
        for field in ("id", "kind", "owner"):
            with self.subTest(field=field):
                row = self.capability(**{field: "  "})
                self.assertEqual(
                    capabilities.validate_manifest(
                        {"schema": 2, "capabilities": [row]}, ROOT
                    ),
                    [f"capabilities[0].{field}: expected a non-empty string"],
                )

    def test_capability_arrays_have_array_type(self) -> None:
        row = self.capability(targets="x86_64")
        self.assertEqual(
            capabilities.validate_manifest({"schema": 2, "capabilities": [row]}, ROOT),
            ["capabilities[0].targets: expected an array"],
        )

    def test_capability_arrays_reject_duplicate_strings(self) -> None:
        row = self.capability(targets=["x86_64", "x86_64"])
        self.assertEqual(
            capabilities.validate_manifest({"schema": 2, "capabilities": [row]}, ROOT),
            ["capabilities[0].targets: duplicate entry x86_64"],
        )

    def test_capability_array_entries_are_non_empty_strings(self) -> None:
        row = self.capability(limitations=[" ", 7])
        self.assertEqual(
            capabilities.validate_manifest({"schema": 2, "capabilities": [row]}, ROOT),
            [
                "capabilities[0].limitations[0]: expected a non-empty string",
                "capabilities[0].limitations[1]: expected a non-empty string",
            ],
        )

    def test_capability_ids_are_unique(self) -> None:
        rows = [self.capability(), self.capability(owner="symbolic")]
        self.assertEqual(
            capabilities.validate_manifest({"schema": 2, "capabilities": rows}, ROOT),
            ["duplicate capability id: llvm.semantic.test-rewrite"],
        )

    def test_required_capability_ids_cannot_be_removed(self) -> None:
        manifest = {"schema": 2, "capabilities": [self.capability()]}
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
            capabilities.validate_manifest({"schema": 2, "capabilities": [row]}, ROOT),
            ["capabilities[0].public_surfaces: expected an object"],
        )

    def test_public_surface_names_are_closed_and_required(self) -> None:
        row = self.capability(
            public_surfaces={"c": [], "python": [], "cli": [], "rust": []}
        )
        self.assertEqual(
            capabilities.validate_manifest({"schema": 2, "capabilities": [row]}, ROOT),
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
            capabilities.validate_manifest({"schema": 2, "capabilities": [row]}, ROOT),
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
            capabilities.validate_manifest({"schema": 2, "capabilities": [row]}, ROOT),
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
            capabilities.validate_manifest({"schema": 2, "capabilities": [row]}, ROOT),
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
                    {"schema": 2, "capabilities": [row]}, root
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

    def test_new_public_surface_must_be_claimed_or_exactly_excluded(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "docs").mkdir()
            (root / "docs" / "roadmap.md").write_text("# Roadmap\n", encoding="utf-8")
            include = root / "include" / "neverd" / "sdk"
            include.mkdir(parents=True)
            (include / "NeverDNewAPI.h").write_text(
                "NEVERD_API int neverd_new_public_api(void);\n",
                encoding="utf-8",
            )
            row = self.capability(docs=["docs/roadmap.md"])

            diagnostics = capabilities.validate_manifest(
                {"schema": 2, "capabilities": [row]}, root
            )

            self.assertIn(
                "public surface c: unclaimed declaration neverd_new_public_api",
                diagnostics,
            )

    def test_surface_exclusions_are_exact_owned_and_non_stale(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "docs").mkdir()
            (root / "docs" / "roadmap.md").write_text("# Roadmap\n", encoding="utf-8")
            include = root / "include" / "neverd" / "sdk"
            include.mkdir(parents=True)
            (include / "NeverDNewAPI.h").write_text(
                "NEVERD_API int neverd_new_public_api(void);\n",
                encoding="utf-8",
            )
            exclusions = {
                "c": [
                    {
                        "owner": "sdk",
                        "reason": "general SDK API",
                        "names": ["neverd_new_public_api"],
                    }
                ],
                "python": [],
                "cli": [],
                "json": [],
            }
            document = {
                "schema": 2,
                "surface_exclusions": exclusions,
                "capabilities": [self.capability(docs=["docs/roadmap.md"])],
            }

            self.assertEqual(capabilities.validate_manifest(document, root), [])

            exclusions["c"][0]["names"] = ["neverd_*"]
            diagnostics = capabilities.validate_manifest(document, root)
            self.assertIn(
                "surface_exclusions.c[0].names[0]: exclusions must use exact names",
                diagnostics,
            )

            exclusions["c"][0]["names"] = ["neverd_stale_api"]
            diagnostics = capabilities.validate_manifest(document, root)
            self.assertIn(
                "surface_exclusions.c: declaration not found: neverd_stale_api",
                diagnostics,
            )

    def test_surface_inventory_uses_qualified_python_methods_and_literal_cli_values(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            package = root / "pluginsdk" / "python" / "neverd_plugin"
            package.mkdir(parents=True)
            (package / "__init__.py").write_text(
                "from .api import Session, published\n"
                '__all__ = ["Session", "published"]\n',
                encoding="utf-8",
            )
            (package / "api.py").write_text(
                "def published():\n"
                "    return None\n\n"
                "def sanitize():\n"
                "    return None\n\n"
                "class Session:\n"
                "    def sanitize(self):\n"
                "        return None\n"
                "    def _private(self):\n"
                "        return None\n",
                encoding="utf-8",
            )
            cli = root / "tools" / "neverd"
            (cli / "cmd").mkdir(parents=True)
            (cli / "NeverDCLI.h").write_text(
                "using OptionalStringList = llvm::cl::list<std::string>;\n"
                "using CustomStringList = OptionalStringList;\n",
                encoding="utf-8",
            )
            (cli / "NeverDCLIOptions.cpp").write_text(
                'cl::SubCommand PatchCmd("patch", "patch");\n'
                "OptionalStringList PatchSanitize(\n"
                '    "sanitize", cl::ValueOptional, cl::sub(PatchCmd));\n'
                "CustomStringList PatchReceipt(\n"
                '    "receipt", cl::ValueOptional, cl::sub(PatchCmd));\n',
                encoding="utf-8",
            )
            (cli / "NeverDCLIValues.def").write_text(
                'NEVERD_CLI_LITERAL(PatchSanitize, "strict")\n',
                encoding="utf-8",
            )
            (cli / "cmd" / "NeverDCmdPipeline.cpp").write_text(
                '// if (PatchSanitize.front() != "unsafe") return 1;\n'
                'if (PatchSanitize.front() != "strict") return 1;\n',
                encoding="utf-8",
            )

            surfaces = capabilities.collect_public_surfaces(root)

            self.assertEqual(
                surfaces["python"],
                frozenset({"published", "Session.sanitize"}),
            )
            self.assertIn("neverd patch --sanitize=strict", surfaces["cli"])
            self.assertIn("neverd patch --receipt", surfaces["cli"])
            self.assertNotIn("neverd patch --sanitize", surfaces["cli"])
            self.assertNotIn("sanitize", surfaces["python"])

    def test_python_surface_inventory_follows_package_aliases_and_all_updates(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            package = root / "pluginsdk" / "python" / "neverd_plugin"
            package.mkdir(parents=True)
            (package / "__init__.py").write_text(
                "from .api import Session as PublicSession\n"
                "BASE = ['PublicSession']\n"
                "__all__ = []\n"
                "__all__ += BASE\n",
                encoding="utf-8",
            )
            (package / "api.py").write_text(
                "class Session:\n    def sanitize(self):\n        return None\n",
                encoding="utf-8",
            )
            (package / "unimported.py").write_text(
                "class PublicSession:\n    def fake(self):\n        return None\n",
                encoding="utf-8",
            )

            surfaces = capabilities.collect_public_surfaces(root)

            self.assertEqual(
                surfaces["python"],
                frozenset({"PublicSession.sanitize"}),
            )

    def test_cli_literal_inventory_uses_the_declarative_value_contract(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cli = root / "tools" / "neverd"
            (cli / "cmd").mkdir(parents=True)
            (cli / "NeverDCLI.h").write_text(
                "using OptionalStringList = llvm::cl::list<std::string>;\n",
                encoding="utf-8",
            )
            (cli / "NeverDCLIOptions.cpp").write_text(
                'cl::SubCommand PatchCmd("patch", "patch");\n'
                "OptionalStringList PatchSanitize(\n"
                '    "sanitize", cl::ValueOptional, cl::sub(PatchCmd));\n',
                encoding="utf-8",
            )
            (cli / "NeverDCLIValues.def").write_text(
                'NEVERD_CLI_LITERAL(PatchSanitize, "strict")\n',
                encoding="utf-8",
            )
            (cli / "cmd" / "NeverDCmdPipeline.cpp").write_text(
                'if ("strict" != PatchSanitize.front()) return 1;\n'
                'if (PatchSanitize.front() != "unsafe") return 1;\n',
                encoding="utf-8",
            )

            surfaces = capabilities.collect_public_surfaces(root)

            self.assertIn("neverd patch --sanitize=strict", surfaces["cli"])
            self.assertNotIn("neverd patch --sanitize=unsafe", surfaces["cli"])

    def test_cli_inventory_scans_namespaced_declarations_in_every_tool_tu(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "docs").mkdir()
            (root / "docs" / "roadmap.md").write_text("# Roadmap\n", encoding="utf-8")
            cli = root / "tools" / "neverd"
            command_directory = cli / "cmd"
            command_directory.mkdir(parents=True)
            (cli / "NeverDCLIOptions.cpp").write_text(
                "// Declarations may live in any NeverD tool TU.\n",
                encoding="utf-8",
            )
            (command_directory / "NeverDCmdInspect.cpp").write_text(
                'llvm::cl::SubCommand InspectCmd("inspect", "inspect");\n'
                "llvm::cl::opt<std::string> InspectInput(\n"
                '    "input", llvm::cl::sub(InspectCmd));\n'
                "llvm::cl::list<std::string> InspectSymbol(\n"
                '    "symbol", llvm::cl::sub(InspectCmd));\n'
                "llvm::cl::alias InspectAlias(\n"
                '    "in", llvm::cl::sub(InspectCmd));\n',
                encoding="utf-8",
            )

            surfaces = capabilities.collect_public_surfaces(root)

            self.assertEqual(
                surfaces["cli"],
                frozenset(
                    {
                        "neverd inspect",
                        "neverd inspect --in",
                        "neverd inspect --input",
                        "neverd inspect --symbol",
                    }
                ),
            )
            row = self.capability(docs=["docs/roadmap.md"])
            diagnostics = capabilities.validate_manifest(
                {"schema": 2, "capabilities": [row]}, root
            )
            self.assertIn(
                "public surface cli: unclaimed declaration neverd inspect --input",
                diagnostics,
            )

    def test_cli_inventory_keeps_translation_unit_local_names_separate(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cli = root / "tools" / "neverd"
            cli.mkdir(parents=True)
            (cli / "Alpha.cpp").write_text(
                "namespace {\n"
                'llvm::cl::SubCommand Command("alpha", "alpha");\n'
                "llvm::cl::opt<bool> Enabled(\n"
                '    "enabled", llvm::cl::sub(Command));\n'
                "}\n",
                encoding="utf-8",
            )
            (cli / "Beta.cpp").write_text(
                "namespace {\n"
                'llvm::cl::SubCommand Command("beta", "beta");\n'
                "llvm::cl::opt<bool> Enabled(\n"
                '    "enabled", llvm::cl::sub(Command));\n'
                "}\n",
                encoding="utf-8",
            )

            surfaces = capabilities.collect_public_surfaces(root)

            self.assertEqual(
                surfaces["cli"],
                frozenset(
                    {
                        "neverd alpha",
                        "neverd alpha --enabled",
                        "neverd beta",
                        "neverd beta --enabled",
                    }
                ),
            )

    def test_cli_inventory_keeps_using_aliases_translation_unit_local(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cli = root / "tools" / "neverd"
            cli.mkdir(parents=True)
            (cli / "Alpha.cpp").write_text(
                'llvm::cl::SubCommand Command("alpha", "alpha");\n'
                "using LocalOption = llvm::cl::opt<bool>;\n"
                "LocalOption Enabled(\n"
                '    "enabled", llvm::cl::sub(Command));\n',
                encoding="utf-8",
            )
            (cli / "Beta.cpp").write_text(
                'llvm::cl::SubCommand Command("beta", "beta");\n'
                "using LocalOption = UnrelatedTemplate<bool>;\n"
                "LocalOption Hidden(\n"
                '    "hidden", llvm::cl::sub(Command));\n',
                encoding="utf-8",
            )

            surfaces = capabilities.collect_public_surfaces(root)

            self.assertIn("neverd alpha --enabled", surfaces["cli"])
            self.assertNotIn("neverd beta --hidden", surfaces["cli"])

    def test_legacy_test_strings_are_not_executable_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "docs").mkdir()
            (root / "docs" / "roadmap.md").write_text("# Roadmap\n", encoding="utf-8")
            row = self.capability(
                docs=["docs/roadmap.md"], tests=["Suite.DoesNotExist"]
            )

            self.assertEqual(
                capabilities.validate_manifest(
                    {"schema": 2, "capabilities": [row]}, root
                ),
                ["capabilities[0].tests[0]: expected an object"],
            )

    def test_evidence_must_be_an_object(self) -> None:
        row = self.capability(proof_test=[])
        self.assertEqual(
            capabilities.validate_manifest({"schema": 2, "capabilities": [row]}, ROOT),
            ["capabilities[0].proof_test: expected an object"],
        )

    def test_evidence_fields_are_closed_and_required(self) -> None:
        row = self.capability(
            proof_test={
                "target": "T",
                "source": "s.cpp",
                "platforms": ["linux"],
                "runner": "gtest",
                "name": "S.N",
            }
        )
        self.assertEqual(
            capabilities.validate_manifest({"schema": 2, "capabilities": [row]}, ROOT),
            [
                "capabilities[0].proof_test: missing filter",
                "capabilities[0].proof_test: unexpected field name",
            ],
        )

    def test_evidence_values_are_non_empty_strings(self) -> None:
        row = self.capability(
            proof_test=self.evidence(target=" ", source=4, filter="", runner="")
        )
        self.assertEqual(
            capabilities.validate_manifest({"schema": 2, "capabilities": [row]}, ROOT),
            [
                "capabilities[0].proof_test.filter: expected a non-empty string",
                "capabilities[0].proof_test.runner: expected a non-empty string",
                "capabilities[0].proof_test.source: expected a non-empty string",
                "capabilities[0].proof_test.target: expected a non-empty string",
            ],
        )

    def test_evidence_target_rejects_cmake_option_injection(self) -> None:
        diagnostics = self.validate_gtest_fixture(
            "TEST(Suite, Exists) {}\n",
            "Suite.Exists",
            target="--clean-first",
            cmake_text=("add_neverd_unittest(--clean-first unittests/test.cpp)\n"),
        )

        self.assertEqual(
            diagnostics,
            [
                "capabilities[0].proof_test.target: expected a safe CMake "
                "logical target name"
            ],
        )
        unsafe_document = {
            "schema": 2,
            "capabilities": [
                {
                    "tests": [
                        self.evidence(
                            target="--verbose",
                            platforms=[capabilities.current_test_platform()],
                        )
                    ]
                }
            ],
        }
        with mock.patch.object(
            capabilities.subprocess,
            "Popen",
            side_effect=AssertionError("unsafe target must not reach Popen"),
        ) as popen:
            self.assertEqual(
                capabilities.build_configured_evidence(
                    unsafe_document, Path("unused-build")
                ),
                ["capabilities[0].tests[0].target: unsafe CMake logical target name"],
            )
        popen.assert_not_called()

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
                proof_test=self.evidence(source="unittests/missing.cpp"),
            )

            self.assertEqual(
                capabilities.validate_manifest(
                    {"schema": 2, "capabilities": [row]}, root
                ),
                [
                    "capabilities[0].proof_test.source: path does not exist: "
                    "unittests/missing.cpp"
                ],
            )

    def test_evidence_platforms_cannot_exceed_source_or_target_conditions(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "docs").mkdir()
            (root / "docs" / "roadmap.md").write_text("# Roadmap\n", encoding="utf-8")
            (root / "unittests").mkdir()
            (root / "unittests" / "test.cpp").write_text(
                "#ifdef __APPLE__\nTEST(Suite, Exists) {}\n#endif\n",
                encoding="utf-8",
            )
            (root / "CMakeLists.txt").write_text(
                "if(APPLE)\n"
                "  add_neverd_unittest(ExistingTests unittests/test.cpp)\n"
                "endif()\n",
                encoding="utf-8",
            )
            row = self.capability(
                docs=["docs/roadmap.md"],
                proof_test=self.evidence(platforms=["darwin", "linux"]),
            )

            self.assertEqual(
                capabilities.validate_manifest(
                    {"schema": 2, "capabilities": [row]}, root
                ),
                [
                    "capabilities[0].proof_test.platforms: Suite.Exists is "
                    "not compiled on linux",
                    "capabilities[0].proof_test.platforms: target ExistingTests "
                    "does not register unittests/test.cpp on linux",
                ],
            )

    def test_unknown_cmake_feature_preserves_known_apple_constraint(self) -> None:
        diagnostics = self.validate_gtest_fixture(
            "TEST(Suite, Exists) {}\n",
            "Suite.Exists",
            cmake_text=(
                "if(APPLE AND SOME_FEATURE)\n"
                "  add_neverd_unittest(ExistingTests unittests/test.cpp)\n"
                "endif()\n"
            ),
        )

        self.assertEqual(
            diagnostics,
            [
                "capabilities[0].proof_test.platforms: target ExistingTests "
                "does not register unittests/test.cpp on linux, windows"
            ],
        )

    def test_unknown_cmake_predicates_preserve_known_platform_atoms(self) -> None:
        cases = (
            (
                "APPLE AND TARGET X",
                "darwin",
                "linux, windows",
            ),
            (
                "WIN32 AND DEFINED FEATURE",
                "windows",
                "darwin, linux",
            ),
            (
                'APPLE AND CMAKE_SYSTEM_NAME STREQUAL "Darwin"',
                "darwin",
                "linux, windows",
            ),
        )
        for condition, _available, unavailable in cases:
            with self.subTest(condition=condition):
                diagnostics = self.validate_gtest_fixture(
                    "TEST(Suite, Exists) {}\n",
                    "Suite.Exists",
                    cmake_text=(
                        f"if({condition})\n"
                        "  add_neverd_unittest(ExistingTests unittests/test.cpp)\n"
                        "endif()\n"
                    ),
                )

                self.assertEqual(
                    diagnostics,
                    [
                        "capabilities[0].proof_test.platforms: target "
                        "ExistingTests does not register unittests/test.cpp on "
                        f"{unavailable}"
                    ],
                )

    def test_unknown_preprocessor_feature_preserves_known_apple_constraint(
        self,
    ) -> None:
        diagnostics = self.validate_gtest_fixture(
            "#if defined(__APPLE__) && SOME_FEATURE\nTEST(Suite, Exists) {}\n#endif\n",
            "Suite.Exists",
        )

        self.assertEqual(
            diagnostics,
            [
                "capabilities[0].proof_test.platforms: Suite.Exists is not "
                "compiled on linux, windows"
            ],
        )

    def test_linux_and_msvc_predicates_preserve_platform_constraints(self) -> None:
        linux_diagnostics = self.validate_gtest_fixture(
            "#if defined(__linux__) && SOME_FEATURE\nTEST(Suite, Exists) {}\n#endif\n",
            "Suite.Exists",
        )
        msvc_diagnostics = self.validate_gtest_fixture(
            "TEST(Suite, Exists) {}\n",
            "Suite.Exists",
            cmake_text=(
                "if(MSVC AND SOME_FEATURE)\n"
                "  add_neverd_unittest(ExistingTests unittests/test.cpp)\n"
                "endif()\n"
            ),
        )

        self.assertEqual(
            linux_diagnostics,
            [
                "capabilities[0].proof_test.platforms: Suite.Exists is not "
                "compiled on darwin, windows"
            ],
        )
        self.assertEqual(
            msvc_diagnostics,
            [
                "capabilities[0].proof_test.platforms: target ExistingTests "
                "does not register unittests/test.cpp on darwin, linux"
            ],
        )

    def test_msvc_preprocessor_predicate_is_windows_only(self) -> None:
        diagnostics = self.validate_gtest_fixture(
            "#if defined(_MSC_VER) && SOME_FEATURE\nTEST(Suite, Exists) {}\n#endif\n",
            "Suite.Exists",
        )

        self.assertEqual(
            diagnostics,
            [
                "capabilities[0].proof_test.platforms: Suite.Exists is not "
                "compiled on darwin, linux"
            ],
        )

    def test_parent_add_subdirectory_platforms_restrict_child_evidence(
        self,
    ) -> None:
        diagnostics = self.validate_gtest_fixture(
            "TEST(Suite, Exists) {}\n",
            "Suite.Exists",
            cmake_text=(
                "if(APPLE AND SOME_FEATURE)\n  add_subdirectory(unittests)\nendif()\n"
            ),
            extra_sources={
                "unittests/CMakeLists.txt": (
                    "add_neverd_unittest(ExistingTests test.cpp)\n"
                )
            },
        )

        self.assertEqual(
            diagnostics,
            [
                "capabilities[0].proof_test.platforms: target ExistingTests "
                "does not register unittests/test.cpp on linux, windows"
            ],
        )

    def test_multiple_parent_entries_union_child_platforms(self) -> None:
        diagnostics = self.validate_gtest_fixture(
            "TEST(Suite, Exists) {}\n",
            "Suite.Exists",
            cmake_text=(
                "if(APPLE)\n"
                "  add_subdirectory(unittests apple-tests)\n"
                "elseif(WIN32)\n"
                "  add_subdirectory(unittests windows-tests)\n"
                "endif()\n"
            ),
            extra_sources={
                "unittests/CMakeLists.txt": (
                    "add_neverd_unittest(ExistingTests test.cpp)\n"
                )
            },
        )

        self.assertEqual(
            diagnostics,
            [
                "capabilities[0].proof_test.platforms: target ExistingTests "
                "does not register unittests/test.cpp on linux"
            ],
        )

    def test_inactive_preprocessor_test_declaration_is_not_evidence(self) -> None:
        diagnostics = self.validate_gtest_fixture(
            "#if 0\nTEST(Suite, Exists) {}\n#endif\n",
            "Suite.Exists",
        )

        self.assertEqual(
            diagnostics,
            [
                "capabilities[0].proof_test.platforms: Suite.Exists is not "
                "compiled on darwin, linux, windows"
            ],
        )

    def test_false_preprocessor_short_circuit_cannot_manufacture_evidence(
        self,
    ) -> None:
        diagnostics = self.validate_gtest_fixture(
            "#if 0 && SOME_FEATURE\nTEST(Suite, Exists) {}\n#endif\n",
            "Suite.Exists",
        )

        self.assertEqual(
            diagnostics,
            [
                "capabilities[0].proof_test.platforms: Suite.Exists is not "
                "compiled on darwin, linux, windows"
            ],
        )

    def test_suffixed_false_preprocessor_constants_are_dead(self) -> None:
        for constant in ("0U", "0L", "0UL", "0x0U"):
            with self.subTest(constant=constant):
                diagnostics = self.validate_gtest_fixture(
                    f"#if {constant}\nTEST(Suite, Exists) {{}}\n#endif\n",
                    "Suite.Exists",
                )

                self.assertEqual(
                    diagnostics,
                    [
                        "capabilities[0].proof_test.platforms: Suite.Exists "
                        "is not compiled on darwin, linux, windows"
                    ],
                )

    def test_continued_false_preprocessor_condition_is_dead(self) -> None:
        diagnostics = self.validate_gtest_fixture(
            "#if 0x0U && \\\n    SOME_FEATURE\nTEST(Suite, Exists) {}\n#endif\n",
            "Suite.Exists",
        )

        self.assertEqual(
            diagnostics,
            [
                "capabilities[0].proof_test.platforms: Suite.Exists is not "
                "compiled on darwin, linux, windows"
            ],
        )

    def test_inactive_cmake_registration_is_not_evidence(self) -> None:
        diagnostics = self.validate_gtest_fixture(
            "TEST(Suite, Exists) {}\n",
            "Suite.Exists",
            cmake_text=(
                "if(FALSE)\n"
                "  add_neverd_unittest(ExistingTests unittests/test.cpp)\n"
                "endif()\n"
            ),
        )

        self.assertNotEqual(diagnostics, [])

    def test_false_cmake_short_circuit_cannot_manufacture_evidence(self) -> None:
        diagnostics = self.validate_gtest_fixture(
            "TEST(Suite, Exists) {}\n",
            "Suite.Exists",
            cmake_text=(
                "if(FALSE AND SOME_FEATURE)\n"
                "  add_neverd_unittest(ExistingTests unittests/test.cpp)\n"
                "endif()\n"
            ),
        )

        self.assertNotEqual(diagnostics, [])

    def test_cmake_false_constants_and_true_else_branches_are_dead(self) -> None:
        false_branch_conditions = ("", '""', "0")
        for condition in false_branch_conditions:
            with self.subTest(false_condition=condition):
                diagnostics = self.validate_gtest_fixture(
                    "TEST(Suite, Exists) {}\n",
                    "Suite.Exists",
                    cmake_text=(
                        f"if({condition})\n"
                        "  add_neverd_unittest(ExistingTests unittests/test.cpp)\n"
                        "endif()\n"
                    ),
                )
                self.assertEqual(
                    diagnostics,
                    [
                        "capabilities[0].proof_test.platforms: target "
                        "ExistingTests does not register unittests/test.cpp on "
                        "darwin, linux, windows"
                    ],
                )

        for condition in ("1", "7", "-3"):
            with self.subTest(true_condition=condition):
                diagnostics = self.validate_gtest_fixture(
                    "TEST(Suite, Exists) {}\n",
                    "Suite.Exists",
                    cmake_text=(
                        f"if({condition})\n"
                        "else()\n"
                        "  add_neverd_unittest(ExistingTests unittests/test.cpp)\n"
                        "endif()\n"
                    ),
                )
                self.assertEqual(
                    diagnostics,
                    [
                        "capabilities[0].proof_test.platforms: target "
                        "ExistingTests does not register unittests/test.cpp on "
                        "darwin, linux, windows"
                    ],
                )

        self.assertEqual(
            self.validate_gtest_fixture(
                "TEST(Suite, Exists) {}\n",
                "Suite.Exists",
                cmake_text=(
                    "if(7)\n"
                    "  add_neverd_unittest(ExistingTests unittests/test.cpp)\n"
                    "endif()\n"
                ),
            ),
            [],
        )

    def test_multiline_cmake_conditions_cannot_register_dead_evidence(
        self,
    ) -> None:
        dead_registrations = (
            (
                "if(\n"
                "  0\n"
                ")\n"
                "  add_neverd_unittest(ExistingTests unittests/test.cpp)\n"
                "endif()\n"
            ),
            (
                "if(\n"
                "  1\n"
                ")\n"
                "elseif(\n"
                "  1\n"
                ")\n"
                "  add_neverd_unittest(ExistingTests unittests/test.cpp)\n"
                "endif()\n"
            ),
        )
        for cmake_text in dead_registrations:
            with self.subTest(cmake_text=cmake_text):
                diagnostics = self.validate_gtest_fixture(
                    "TEST(Suite, Exists) {}\n",
                    "Suite.Exists",
                    cmake_text=cmake_text,
                )

                self.assertEqual(
                    diagnostics,
                    [
                        "capabilities[0].proof_test.platforms: target "
                        "ExistingTests does not register unittests/test.cpp on "
                        "darwin, linux, windows"
                    ],
                )

    def test_orphan_cmake_list_cannot_register_test_evidence(self) -> None:
        diagnostics = self.validate_gtest_fixture(
            "TEST(Suite, Exists) {}\n",
            "Suite.Exists",
            cmake_text="# orphan directory is deliberately not added\n",
            extra_sources={
                "unittests/orphan/CMakeLists.txt": (
                    "add_neverd_unittest(ExistingTests ../test.cpp)\n"
                )
            },
        )

        self.assertNotEqual(diagnostics, [])

    def test_uncalled_cmake_function_cannot_register_test_evidence(self) -> None:
        diagnostics = self.validate_gtest_fixture(
            "TEST(Suite, Exists) {}\n",
            "Suite.Exists",
            cmake_text=(
                "function(register_hidden_test)\n"
                "  add_neverd_unittest(ExistingTests unittests/test.cpp)\n"
                "endfunction()\n"
            ),
        )

        self.assertNotEqual(diagnostics, [])

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
                proof_test=self.evidence(),
            )

            self.assertEqual(
                capabilities.validate_manifest(
                    {"schema": 2, "capabilities": [row]}, root
                ),
                [
                    "capabilities[0].proof_test.filter: not found in "
                    "unittests/test.cpp: Suite.Exists"
                ],
            )

    def test_value_parameterized_evidence_uses_an_executable_wildcard(self) -> None:
        diagnostics = self.validate_gtest_fixture(
            "TEST_P(ParamSuite, Exists) {}\n"
            "INSTANTIATE_TEST_SUITE_P(Matrix, ParamSuite, Values(1));\n",
            "*/ParamSuite.Exists/*",
        )

        self.assertEqual(diagnostics, [])

    def test_value_parameterized_evidence_rejects_a_bare_filter(self) -> None:
        diagnostics = self.validate_gtest_fixture(
            "TEST_P(ParamSuite, Exists) {}\n"
            "INSTANTIATE_TEST_SUITE_P(Matrix, ParamSuite, Values(1));\n",
            "ParamSuite.Exists",
        )

        self.assertEqual(
            diagnostics,
            [
                "capabilities[0].proof_test.filter: TEST_P requires canonical "
                "executable filter */ParamSuite.Exists/*; got ParamSuite.Exists"
            ],
        )

    def test_value_parameterized_evidence_requires_a_target_owned_instantiation(
        self,
    ) -> None:
        diagnostics = self.validate_gtest_fixture(
            "TEST_P(ParamSuite, Exists) {}\n",
            "*/ParamSuite.Exists/*",
        )

        self.assertEqual(
            diagnostics,
            [
                "capabilities[0].proof_test.filter: TEST_P ParamSuite.Exists "
                "has no target-owned INSTANTIATE_TEST_SUITE_P in ExistingTests",
                "capabilities[0].proof_test.platforms: */ParamSuite.Exists/* "
                "is not executable on darwin, linux, windows",
            ],
        )

    def test_other_target_instantiation_cannot_make_a_test_executable(self) -> None:
        diagnostics = self.validate_gtest_fixture(
            "TEST_P(ParamSuite, Exists) {}\n",
            "*/ParamSuite.Exists/*",
            cmake_text=(
                "add_neverd_unittest(ExistingTests unittests/test.cpp)\n"
                "add_neverd_unittest(OtherTests unittests/instantiate.cpp)\n"
            ),
            extra_sources={
                "unittests/instantiate.cpp": (
                    "INSTANTIATE_TEST_SUITE_P(Matrix, ParamSuite, Values(1));\n"
                )
            },
        )

        self.assertEqual(
            diagnostics,
            [
                "capabilities[0].proof_test.filter: TEST_P ParamSuite.Exists "
                "has no target-owned INSTANTIATE_TEST_SUITE_P in ExistingTests",
                "capabilities[0].proof_test.platforms: */ParamSuite.Exists/* "
                "is not executable on darwin, linux, windows",
            ],
        )

    def test_ordinary_test_cannot_use_a_parameterized_filter(self) -> None:
        diagnostics = self.validate_gtest_fixture(
            "TEST(Suite, Exists) {}\n",
            "*/Suite.Exists/*",
        )

        self.assertEqual(
            diagnostics,
            [
                "capabilities[0].proof_test.filter: TEST requires canonical "
                "executable filter Suite.Exists; got */Suite.Exists/*"
            ],
        )

    def test_typed_test_requires_its_canonical_runtime_shape(self) -> None:
        diagnostics = self.validate_gtest_fixture(
            "TYPED_TEST_SUITE(TypedSuite, Types);\nTYPED_TEST(TypedSuite, Exists) {}\n",
            "TypedSuite/*.Exists",
        )

        self.assertEqual(diagnostics, [])

    def test_type_parameterized_test_closes_registration_and_instantiation(
        self,
    ) -> None:
        diagnostics = self.validate_gtest_fixture(
            "TYPED_TEST_SUITE_P(TypedPattern);\n"
            "TYPED_TEST_P(TypedPattern, Exists) {}\n"
            "REGISTER_TYPED_TEST_SUITE_P(TypedPattern, Exists);\n"
            "INSTANTIATE_TYPED_TEST_SUITE_P(Matrix, TypedPattern, Types);\n",
            "*/TypedPattern/*.Exists",
        )

        self.assertEqual(diagnostics, [])

    def test_type_parameterized_test_rejects_an_unregistered_test_name(
        self,
    ) -> None:
        diagnostics = self.validate_gtest_fixture(
            "TYPED_TEST_SUITE_P(TypedPattern);\n"
            "TYPED_TEST_P(TypedPattern, Exists) {}\n"
            "REGISTER_TYPED_TEST_SUITE_P(TypedPattern, Other);\n"
            "INSTANTIATE_TYPED_TEST_SUITE_P(Matrix, TypedPattern, Types);\n",
            "*/TypedPattern/*.Exists",
        )

        self.assertEqual(
            diagnostics,
            [
                "capabilities[0].proof_test.filter: TYPED_TEST_P "
                "TypedPattern.Exists has no target-owned "
                "REGISTER_TYPED_TEST_SUITE_P in ExistingTests",
                "capabilities[0].proof_test.platforms: "
                "*/TypedPattern/*.Exists is not executable on darwin, linux, "
                "windows",
            ],
        )

    def test_evidence_rejects_a_source_not_owned_by_its_target(self) -> None:
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
                proof_test=self.evidence(target="MissingTests"),
            )

            self.assertEqual(
                capabilities.validate_manifest(
                    {"schema": 2, "capabilities": [row]}, root
                ),
                [
                    "capabilities[0].proof_test.source: unittests/test.cpp is "
                    "not registered to gtest target MissingTests"
                ],
            )

    def test_plain_cmake_executable_is_not_a_registered_gtest_target(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "docs").mkdir()
            (root / "docs" / "roadmap.md").write_text("# Roadmap\n", encoding="utf-8")
            (root / "unittests").mkdir()
            (root / "unittests" / "test.cpp").write_text(
                "TEST(Suite, Exists) {}\n", encoding="utf-8"
            )
            (root / "CMakeLists.txt").write_text(
                "add_executable(ExistingTests unittests/test.cpp)\n",
                encoding="utf-8",
            )
            row = self.capability(docs=["docs/roadmap.md"], proof_test=self.evidence())

            self.assertEqual(
                capabilities.validate_manifest(
                    {"schema": 2, "capabilities": [row]}, root
                ),
                [
                    "capabilities[0].proof_test.source: unittests/test.cpp is "
                    "not registered to gtest target ExistingTests"
                ],
            )

    def test_python_evidence_requires_an_active_registered_unittest_runner(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "docs").mkdir()
            (root / "docs" / "roadmap.md").write_text("# Roadmap\n", encoding="utf-8")
            tests = root / "pluginsdk" / "python" / "tests"
            tests.mkdir(parents=True)
            (tests / "test_api.py").write_text(
                "class Suite:\n    def test_exists(self):\n        pass\n",
                encoding="utf-8",
            )
            workflows = root / ".github" / "workflows"
            workflows.mkdir(parents=True)
            workflow = workflows / "ci.yml"
            workflow.write_text(
                "runs-on: ubuntu-24.04\n"
                "# python -m unittest discover -s pluginsdk/python/tests -v\n",
                encoding="utf-8",
            )
            evidence = self.evidence(
                runner="python-unittest",
                target="python-unittest-discover:pluginsdk/python/tests",
                source="pluginsdk/python/tests/test_api.py",
                filter="Suite.test_exists",
            )
            row = self.capability(docs=["docs/roadmap.md"], proof_test=evidence)

            self.assertEqual(
                capabilities.validate_manifest(
                    {"schema": 2, "capabilities": [row]}, root
                ),
                [
                    "capabilities[0].proof_test.source: "
                    "pluginsdk/python/tests/test_api.py is not registered to "
                    "python-unittest target "
                    "python-unittest-discover:pluginsdk/python/tests"
                ],
            )

            (root / "CMakeLists.txt").write_text(
                "add_test(\n"
                "  NAME NeverDPythonSDKUnitTests\n"
                "  COMMAND ${Python3_EXECUTABLE} -m unittest discover\n"
                "          -s ${CMAKE_SOURCE_DIR}/pluginsdk/python/tests\n"
                "          -p test*.py -v)\n",
                encoding="utf-8",
            )
            evidence["target"] = "NeverDPythonSDKUnitTests"
            self.assertEqual(
                capabilities.validate_manifest(
                    {"schema": 2, "capabilities": [row]}, root
                ),
                [],
            )

    def test_disabled_workflow_step_cannot_register_python_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "docs").mkdir()
            (root / "docs" / "roadmap.md").write_text("# Roadmap\n", encoding="utf-8")
            tests = root / "pluginsdk" / "python" / "tests"
            tests.mkdir(parents=True)
            (tests / "test_api.py").write_text(
                "class Suite:\n    def test_exists(self):\n        pass\n",
                encoding="utf-8",
            )
            workflows = root / ".github" / "workflows"
            workflows.mkdir(parents=True)
            (workflows / "ci.yml").write_text(
                "jobs:\n"
                "  tests:\n"
                "    runs-on: ubuntu-24.04\n"
                "    steps:\n"
                "      - if: false\n"
                "        run: python -m unittest discover -s "
                "pluginsdk/python/tests -v\n",
                encoding="utf-8",
            )
            evidence = self.evidence(
                runner="python-unittest",
                target="python-unittest-discover:pluginsdk/python/tests",
                source="pluginsdk/python/tests/test_api.py",
                filter="Suite.test_exists",
                platforms=["linux"],
            )
            row = self.capability(docs=["docs/roadmap.md"], proof_test=evidence)

            diagnostics = capabilities.validate_manifest(
                {"schema": 2, "capabilities": [row]}, root
            )

            self.assertNotEqual(diagnostics, [])

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
                proof_test=self.evidence(filter="Suite.Claimed"),
            )

            self.assertEqual(
                capabilities.validate_manifest(
                    {"schema": 2, "capabilities": [row]}, root
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

    def test_cmake_file_api_codemodel_binds_target_source_and_artifact(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "source"
            build = Path(directory) / "build"
            (root / "unittests").mkdir(parents=True)
            (root / "unittests" / "test.cpp").write_text(
                "TEST(Suite, Exists) {}\n", encoding="utf-8"
            )
            reply = build / ".cmake" / "api" / "v1" / "reply"
            reply.mkdir(parents=True)
            (reply / "index-test.json").write_text(
                json.dumps(
                    {
                        "reply": {
                            "codemodel-v2": {
                                "kind": "codemodel",
                                "jsonFile": "codemodel-test.json",
                            }
                        }
                    }
                ),
                encoding="utf-8",
            )
            (reply / "codemodel-test.json").write_text(
                json.dumps(
                    {
                        "configurations": [
                            {
                                "name": "Release",
                                "targets": [{"jsonFile": "target-test.json"}],
                            }
                        ]
                    }
                ),
                encoding="utf-8",
            )
            (reply / "target-test.json").write_text(
                json.dumps(
                    {
                        "name": "ExistingTests",
                        "sources": [{"path": "unittests/test.cpp"}],
                        "artifacts": [{"path": "bin/ExistingTests"}],
                    }
                ),
                encoding="utf-8",
            )

            targets, diagnostics = capabilities.collect_configured_targets(
                root, build, build_config="Release"
            )

            self.assertEqual(diagnostics, [])
            self.assertEqual(
                targets["ExistingTests"].sources,
                frozenset({"unittests/test.cpp"}),
            )
            self.assertEqual(
                targets["ExistingTests"].artifacts,
                (build / "bin" / "ExistingTests",),
            )

    def test_build_audit_requires_a_codemodel_query_reply(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "source"
            build = Path(directory) / "build"
            root.mkdir()
            build.mkdir()

            _targets, diagnostics = capabilities.collect_configured_targets(root, build)

            self.assertEqual(len(diagnostics), 1)
            self.assertIn(
                "create the codemodel-v2 query before configure", diagnostics[0]
            )

    def test_build_audit_requires_a_post_build_completion_stamp(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "source"
            build = Path(directory) / "build"
            root.mkdir()
            build.mkdir()
            index = build / ".cmake" / "api" / "v1" / "reply" / "index.json"
            index.parent.mkdir(parents=True)
            index.write_text("{}\n", encoding="utf-8")
            (build / "CMakeCache.txt").write_text("cache\n", encoding="utf-8")

            diagnostics = capabilities.audit_build_freshness(
                root,
                build,
                {"schema": 2, "capabilities": []},
            )

            self.assertEqual(len(diagnostics), 1)
            self.assertIn("post-build completion stamp", diagnostics[0])

    def test_build_audit_rejects_source_newer_than_build_stamp(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "source"
            build = Path(directory) / "build"
            source = root / "unittests" / "test.cpp"
            source.parent.mkdir(parents=True)
            source.write_text("TEST(Suite, Exists) {}\n", encoding="utf-8")
            index = build / ".cmake" / "api" / "v1" / "reply" / "index.json"
            index.parent.mkdir(parents=True)
            index.write_text("{}\n", encoding="utf-8")
            cache = build / "CMakeCache.txt"
            cache.write_text("cache\n", encoding="utf-8")
            stamp = build / capabilities.BUILD_COMPLETION_STAMP
            stamp.write_text("complete\n", encoding="utf-8")
            os.utime(index, ns=(1_000_000_000, 1_000_000_000))
            os.utime(cache, ns=(1_000_000_000, 1_000_000_000))
            os.utime(stamp, ns=(2_000_000_000, 2_000_000_000))
            os.utime(source, ns=(3_000_000_000, 3_000_000_000))
            document = {
                "schema": 2,
                "capabilities": [
                    {
                        "tests": [
                            self.evidence(
                                platforms=[capabilities.current_test_platform()]
                            )
                        ]
                    }
                ],
            }

            diagnostics = capabilities.audit_build_freshness(root, build, document)

            self.assertEqual(
                diagnostics,
                [
                    "build audit: post-build completion stamp predates input "
                    "unittests/test.cpp"
                ],
            )

    def test_build_audit_rejects_file_api_index_older_than_configure(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "source"
            build = Path(directory) / "build"
            root.mkdir()
            index = build / ".cmake" / "api" / "v1" / "reply" / "index.json"
            index.parent.mkdir(parents=True)
            index.write_text("{}\n", encoding="utf-8")
            cache = build / "CMakeCache.txt"
            cache.write_text("cache\n", encoding="utf-8")
            stamp = build / capabilities.BUILD_COMPLETION_STAMP
            stamp.write_text("complete\n", encoding="utf-8")
            os.utime(index, ns=(1_000_000_000, 1_000_000_000))
            os.utime(cache, ns=(2_000_000_000, 2_000_000_000))
            os.utime(stamp, ns=(3_000_000_000, 3_000_000_000))

            diagnostics = capabilities.audit_build_freshness(
                root, build, {"schema": 2, "capabilities": []}
            )

            self.assertTrue(
                any("File API snapshot predates" in item for item in diagnostics),
                diagnostics,
            )

    def test_build_audit_rejects_mismatched_source_before_build_side_effect(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "source"
            other = Path(directory) / "other-source"
            build = Path(directory) / "build"
            root.mkdir()
            other.mkdir()
            build.mkdir()
            (build / "CMakeCache.txt").write_text(
                f"CMAKE_HOME_DIRECTORY:INTERNAL={other}\n",
                encoding="utf-8",
            )

            with (
                mock.patch.object(
                    capabilities.subprocess,
                    "run",
                    side_effect=AssertionError("subprocess.run must not be called"),
                ) as run,
                mock.patch.object(
                    capabilities.subprocess,
                    "Popen",
                    side_effect=AssertionError("subprocess.Popen must not be called"),
                ) as popen,
            ):
                diagnostics = capabilities.audit_configured_evidence(
                    {"schema": 2, "capabilities": []}, root, build
                )

            self.assertEqual(
                diagnostics,
                [
                    "build audit: CMake cache source directory does not match "
                    f"repository root: {other.resolve()}"
                ],
            )
            run.assert_not_called()
            popen.assert_not_called()

    def test_build_audit_rejects_mismatched_codemodel_before_build(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "source"
            other = Path(directory) / "other-source"
            build = Path(directory) / "build"
            root.mkdir()
            other.mkdir()
            (other / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.20)\nproject(OtherProject NONE)\n",
                encoding="utf-8",
            )
            query = build / ".cmake" / "api" / "v1" / "query"
            query.mkdir(parents=True)
            (query / "codemodel-v2").touch()
            subprocess.run(
                ("cmake", "-S", str(other), "-B", str(build), "-G", "Ninja"),
                check=True,
                stdout=subprocess.DEVNULL,
            )
            cache = build / "CMakeCache.txt"
            cache_lines = cache.read_text(encoding="utf-8").splitlines()
            cache_lines = [
                f"CMAKE_HOME_DIRECTORY:INTERNAL={root}"
                if line.startswith("CMAKE_HOME_DIRECTORY:INTERNAL=")
                else line
                for line in cache_lines
            ]
            cache.write_text("\n".join(cache_lines) + "\n", encoding="utf-8")

            with (
                mock.patch.object(
                    capabilities.subprocess,
                    "run",
                    side_effect=AssertionError("subprocess.run must not be called"),
                ) as run,
                mock.patch.object(
                    capabilities.subprocess,
                    "Popen",
                    side_effect=AssertionError("subprocess.Popen must not be called"),
                ) as popen,
            ):
                diagnostics = capabilities.audit_configured_evidence(
                    {"schema": 2, "capabilities": []}, root, build
                )

            self.assertEqual(
                diagnostics,
                [
                    "build audit: codemodel source directory does not match "
                    f"repository root: {other.resolve()}"
                ],
            )
            run.assert_not_called()
            popen.assert_not_called()

    @unittest.skipUnless(os.name == "posix", "POSIX process-group assertion")
    def test_configured_build_timeout_terminates_the_process_group(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            fake_bin = temporary / "bin"
            fake_bin.mkdir()
            parent_pid_file = temporary / "parent-pid"
            child_pid_file = temporary / "child-pid"
            fake_cmake = fake_bin / "cmake"
            fake_cmake.write_text(
                "#!/bin/sh\n"
                'printf \'%s\' "$$" > "$PARENT_PID_FILE"\n'
                "/bin/sleep 60 &\n"
                "child=$!\n"
                'printf \'%s\' "$child" > "$CHILD_PID_FILE"\n'
                'wait "$child"\n',
                encoding="utf-8",
            )
            fake_cmake.chmod(0o755)
            environment = {
                "PATH": f"{fake_bin}{os.pathsep}{os.environ.get('PATH', '')}",
                "PARENT_PID_FILE": str(parent_pid_file),
                "CHILD_PID_FILE": str(child_pid_file),
            }

            with mock.patch.dict(os.environ, environment):
                diagnostics = capabilities.build_configured_evidence(
                    {"schema": 2, "capabilities": []},
                    temporary / "build",
                    timeout=2.0,
                )

            self.assertEqual(len(diagnostics), 1)
            self.assertIn(
                "build audit: default build timed out after 2 seconds",
                diagnostics[0],
            )
            self.assertTrue(parent_pid_file.is_file())
            self.assertTrue(child_pid_file.is_file())
            for pid_file in (parent_pid_file, child_pid_file):
                completed = subprocess.run(
                    ("ps", "-o", "stat=", "-p", pid_file.read_text()),
                    check=False,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL,
                    text=True,
                )
                state = completed.stdout.strip()
                self.assertTrue(
                    completed.returncode != 0 or not state or state.startswith("Z"),
                    f"process {pid_file.read_text()} survived in state {state}",
                )

    def test_configured_build_failure_reports_only_the_log_tail(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            program = (
                "import sys\n"
                "sys.stdout.write('x' * 2000)\n"
                "sys.stderr.write('\\nTAIL-MARKER\\n')\n"
                "raise SystemExit(7)\n"
            )
            run_bounded_process = capabilities._run_bounded_process

            def run_fake_cmake(
                _command: object, **kwargs: object
            ) -> tuple[subprocess.CompletedProcess[str] | None, str | None]:
                return run_bounded_process(
                    (sys.executable, "-c", program), **kwargs
                )

            with mock.patch.object(
                capabilities,
                "_run_bounded_process",
                side_effect=run_fake_cmake,
            ):
                diagnostics = capabilities.build_configured_evidence(
                    {"schema": 2, "capabilities": []}, temporary / "build"
                )

            self.assertEqual(len(diagnostics), 1)
            self.assertIn("default build failed with exit 7", diagnostics[0])
            self.assertIn("TAIL-MARKER", diagnostics[0])
            self.assertLess(len(diagnostics[0]), 1400)

    def test_evidence_runner_rejects_output_beyond_the_capture_limit(self) -> None:
        completed, error = capabilities._run_bounded_process(
            (
                sys.executable,
                "-c",
                "import sys; sys.stdout.write('x' * 65)",
            ),
            timeout=5,
            output_limit=64,
        )

        self.assertIsNone(completed)
        self.assertEqual(
            error,
            "combined output exceeded 64-byte limit (65 bytes)",
        )

    def test_ctest_inventory_uses_dedicated_resource_bounds(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            root = temporary / "source"
            build = temporary / "build"
            root.mkdir()
            build.mkdir()
            completed = subprocess.CompletedProcess(
                ("ctest",), 0, '{"tests": []}', ""
            )

            with (
                mock.patch.object(
                    capabilities, "audit_configured_build_identity", return_value=[]
                ),
                mock.patch.object(
                    capabilities, "build_configured_evidence", return_value=[]
                ),
                mock.patch.object(capabilities, "audit_build_freshness", return_value=[]),
                mock.patch.object(
                    capabilities, "collect_configured_targets", return_value=({}, [])
                ),
                mock.patch.object(
                    capabilities,
                    "_run_bounded_process",
                    return_value=(completed, None),
                ) as run,
            ):
                diagnostics = capabilities.audit_configured_evidence(
                    {"schema": 2, "capabilities": []}, root, build
                )

            self.assertEqual(diagnostics, [])
            self.assertGreater(
                capabilities.CTEST_INVENTORY_OUTPUT_LIMIT,
                capabilities.EVIDENCE_OUTPUT_LIMIT,
            )
            self.assertEqual(
                run.call_args.kwargs["output_limit"],
                capabilities.CTEST_INVENTORY_OUTPUT_LIMIT,
            )
            self.assertGreaterEqual(run.call_args.kwargs["timeout"], 10 * 60)

    def test_configured_audit_reuses_saved_ctest_inventory(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            root = temporary / "source"
            build = temporary / "build"
            root.mkdir()
            build.mkdir()
            inventory = build / "ctest-inventory.json"
            inventory.write_text('{"tests": []}\n', encoding="utf-8")

            with (
                mock.patch.object(
                    capabilities, "audit_configured_build_identity", return_value=[]
                ),
                mock.patch.object(
                    capabilities, "build_configured_evidence", return_value=[]
                ),
                mock.patch.object(capabilities, "audit_build_freshness", return_value=[]),
                mock.patch.object(
                    capabilities, "collect_configured_targets", return_value=({}, [])
                ),
                mock.patch.object(
                    capabilities,
                    "_run_bounded_process",
                    side_effect=AssertionError("CTest must not be rediscovered"),
                ) as run,
            ):
                diagnostics = capabilities.audit_configured_evidence(
                    {"schema": 2, "capabilities": []},
                    root,
                    build,
                    ctest_inventory_path=inventory,
                )

            self.assertEqual(diagnostics, [])
            run.assert_not_called()

    def test_saved_ctest_inventory_uses_the_capture_limit(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            root = temporary / "source"
            build = temporary / "build"
            root.mkdir()
            build.mkdir()
            inventory = build / "ctest-inventory.json"
            inventory.write_text('{"tests": []}\n', encoding="utf-8")

            with (
                mock.patch.object(
                    capabilities, "audit_configured_build_identity", return_value=[]
                ),
                mock.patch.object(
                    capabilities, "build_configured_evidence", return_value=[]
                ),
                mock.patch.object(capabilities, "audit_build_freshness", return_value=[]),
                mock.patch.object(
                    capabilities, "collect_configured_targets", return_value=({}, [])
                ),
                mock.patch.object(capabilities, "CTEST_INVENTORY_OUTPUT_LIMIT", 8),
            ):
                diagnostics = capabilities.audit_configured_evidence(
                    {"schema": 2, "capabilities": []},
                    root,
                    build,
                    ctest_inventory_path=inventory,
                )

            self.assertEqual(len(diagnostics), 1)
            self.assertIn("CTest inventory exceeds 8-byte limit", diagnostics[0])

    @unittest.skipUnless(os.name == "posix", "POSIX process-group assertion")
    def test_evidence_output_flood_terminates_parent_and_child(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            parent_pid_file = temporary / "parent.pid"
            child_pid_file = temporary / "child.pid"
            child_program = (
                "import os, time\n"
                "payload = b'x' * 4096\n"
                "for _ in range(256):\n"
                "    os.write(1, payload)\n"
                "time.sleep(60)\n"
            )
            parent_program = (
                "import os, subprocess, sys, time\n"
                "from pathlib import Path\n"
                "Path(sys.argv[1]).write_text(str(os.getpid()))\n"
                "child = subprocess.Popen([sys.executable, '-c', sys.argv[3]])\n"
                "Path(sys.argv[2]).write_text(str(child.pid))\n"
                "time.sleep(60)\n"
            )
            process_ids: list[int] = []
            try:
                started = time.monotonic()
                completed, error = capabilities._run_bounded_process(
                    (
                        sys.executable,
                        "-c",
                        parent_program,
                        str(parent_pid_file),
                        str(child_pid_file),
                        child_program,
                    ),
                    timeout=2,
                    output_limit=32 * 1024,
                )
                elapsed = time.monotonic() - started

                self.assertIsNone(completed)
                self.assertIn("combined output exceeded", error or "")
                self.assertLess(elapsed, 1.5)
                self.assertTrue(parent_pid_file.is_file())
                self.assertTrue(child_pid_file.is_file())
                process_ids = [
                    int(parent_pid_file.read_text(encoding="utf-8")),
                    int(child_pid_file.read_text(encoding="utf-8")),
                ]
                for process_id in process_ids:
                    observed = subprocess.run(
                        ("ps", "-o", "stat=", "-p", str(process_id)),
                        check=False,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.DEVNULL,
                        text=True,
                    )
                    state = observed.stdout.strip()
                    self.assertTrue(
                        observed.returncode != 0 or not state or state.startswith("Z"),
                        f"flood process {process_id} survived in state {state}",
                    )
            finally:
                for pid_file in (parent_pid_file, child_pid_file):
                    if pid_file.is_file():
                        process_id = int(pid_file.read_text(encoding="utf-8"))
                        try:
                            os.kill(process_id, 9)
                        except ProcessLookupError:
                            pass

    def test_build_audit_executes_each_gtest_evidence_filter(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "source"
            build = Path(directory) / "build"
            source = root / "unittests" / "test.c"
            source.parent.mkdir(parents=True)
            source.write_text(
                r"""
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
  const char *xml = NULL;
  for (int index = 1; index < argc; ++index) {
    if (strcmp(argv[index], "--gtest_list_tests") == 0) {
      puts("Suite.\n  Exists");
      return 0;
    }
    const char *prefix = "--gtest_output=xml:";
    if (strncmp(argv[index], prefix, strlen(prefix)) == 0)
      xml = argv[index] + strlen(prefix);
  }
  if (xml == NULL)
    return 2;
  FILE *result = fopen(xml, "w");
  if (result == NULL)
    return 3;
  FILE *force_large = fopen("force-large-xml", "r");
  FILE *force_zero = fopen("force-zero", "r");
  if (force_large != NULL) {
    fclose(force_large);
    for (int index = 0; index < 2048; ++index)
      fputc('x', result);
  } else if (force_zero != NULL) {
    fclose(force_zero);
    fputs("<testsuites tests=\"0\" failures=\"0\" disabled=\"0\" "
          "errors=\"0\"></testsuites>", result);
  } else {
    fputs("<testsuites tests=\"1\" failures=\"0\" disabled=\"0\" "
          "errors=\"0\"><testsuite name=\"Suite\" tests=\"1\" "
          "skipped=\"0\"><testcase name=\"Exists\" classname=\"Suite\" "
          "status=\"run\" result=\"completed\"/></testsuite></testsuites>",
          result);
  }
  fclose(result);
  return 0;
}
""".lstrip(),
                encoding="utf-8",
            )
            (root / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.20)\n"
                "project(CapabilityAudit C)\n"
                "include(CTest)\n"
                "add_executable(FakeTests unittests/test.c)\n"
                "add_test(NAME Suite.Exists COMMAND $<TARGET_FILE:FakeTests> "
                "--gtest_filter=Suite.Exists --gtest_also_run_disabled_tests)\n"
                "set_tests_properties(Suite.Exists PROPERTIES "
                "LABELS FakeTests TIMEOUT 5)\n",
                encoding="utf-8",
            )
            query = build / ".cmake" / "api" / "v1" / "query"
            query.mkdir(parents=True)
            (query / "codemodel-v2").touch()
            (query / "cmakeFiles-v1").touch()
            subprocess.run(
                (
                    "cmake",
                    "-S",
                    str(root),
                    "-B",
                    str(build),
                    "-G",
                    "Ninja",
                    "-DCMAKE_BUILD_TYPE=Release",
                ),
                check=True,
                stdout=subprocess.DEVNULL,
            )
            subprocess.run(
                ("cmake", "--build", str(build)),
                check=True,
                stdout=subprocess.DEVNULL,
            )
            (build / capabilities.BUILD_COMPLETION_STAMP).touch()
            document = {
                "schema": 2,
                "capabilities": [
                    {
                        "tests": [
                            {
                                "runner": "gtest",
                                "target": "FakeTests",
                                "source": "unittests/test.c",
                                "filter": "Suite.Exists",
                                "platforms": [capabilities.current_test_platform()],
                            }
                        ]
                    }
                ],
            }

            diagnostics = capabilities.audit_configured_evidence(
                document, root, build, build_config="Release"
            )

            self.assertEqual(diagnostics, [])

            (build / "force-zero").touch()
            diagnostics = capabilities.audit_configured_evidence(
                document, root, build, build_config="Release"
            )
            self.assertTrue(
                any("evidence executed zero tests" in item for item in diagnostics),
                diagnostics,
            )

            (build / "force-zero").unlink()
            (build / "force-large-xml").touch()
            with mock.patch.object(capabilities, "GTEST_XML_OUTPUT_LIMIT", 1024):
                diagnostics = capabilities.audit_configured_evidence(
                    document, root, build, build_config="Release"
                )
            self.assertTrue(
                any(
                    "GoogleTest result exceeds 1024-byte limit" in item
                    for item in diagnostics
                ),
                diagnostics,
            )

    def test_build_audit_rebuilds_excluded_evidence_and_transitive_inputs(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "source"
            build = Path(directory) / "build"
            include = root / "include"
            tests = root / "unittests"
            include.mkdir(parents=True)
            tests.mkdir(parents=True)
            header = include / "contract.h"
            implementation = root / "library.c"
            source = tests / "test.c"
            header.write_text(
                "#pragma once\n#define CONTRACT_VALUE 1\nint contract_value(void);\n",
                encoding="utf-8",
            )
            implementation.write_text(
                '#include "contract.h"\n'
                "int contract_value(void) { return CONTRACT_VALUE; }\n",
                encoding="utf-8",
            )
            source.write_text(
                r"""
#include "contract.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  const char *xml = NULL;
  for (int index = 1; index < argc; ++index) {
    if (strcmp(argv[index], "--gtest_list_tests") == 0) {
      puts("Suite.\n  Transitive");
      return 0;
    }
    const char *prefix = "--gtest_output=xml:";
    if (strncmp(argv[index], prefix, strlen(prefix)) == 0)
      xml = argv[index] + strlen(prefix);
  }
  FILE *oracle = fopen("expected.txt", "r");
  if (xml == NULL || oracle == NULL)
    return 2;
  int expected = 0;
  if (fscanf(oracle, "%d", &expected) != 1) {
    fclose(oracle);
    return 3;
  }
  fclose(oracle);
  int matches = contract_value() == expected;
  FILE *result = fopen(xml, "w");
  if (result == NULL)
    return 4;
  if (matches) {
    fputs("<testsuites tests=\"1\" failures=\"0\" disabled=\"0\" "
          "errors=\"0\"><testsuite name=\"Suite\" tests=\"1\" "
          "skipped=\"0\"><testcase name=\"Transitive\" "
          "classname=\"Suite\" status=\"run\" result=\"completed\"/>"
          "</testsuite></testsuites>", result);
  } else {
    fputs("<testsuites tests=\"1\" failures=\"1\" disabled=\"0\" "
          "errors=\"0\"><testsuite name=\"Suite\" tests=\"1\" "
          "failures=\"1\" skipped=\"0\"><testcase name=\"Transitive\" "
          "classname=\"Suite\" status=\"run\" result=\"completed\">"
          "<failure/></testcase></testsuite></testsuites>", result);
  }
  fclose(result);
  return matches ? 0 : 1;
}
""".lstrip(),
                encoding="utf-8",
            )
            (root / "unrelated.c").write_text(
                "int main(void) { return 0; }\n", encoding="utf-8"
            )
            (root / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.20)\n"
                "project(TransitiveCapabilityAudit C)\n"
                "include(CTest)\n"
                "add_library(ContractLibrary STATIC EXCLUDE_FROM_ALL library.c)\n"
                "target_include_directories(ContractLibrary PRIVATE include)\n"
                "add_executable(FakeEvidence EXCLUDE_FROM_ALL unittests/test.c)\n"
                "target_include_directories(FakeEvidence PRIVATE include)\n"
                "target_link_libraries(FakeEvidence PRIVATE ContractLibrary)\n"
                "add_executable(Unrelated unrelated.c)\n"
                "add_test(NAME Suite.Transitive "
                "COMMAND $<TARGET_FILE:FakeEvidence> "
                "--gtest_filter=Suite.Transitive)\n"
                "set_tests_properties(Suite.Transitive PROPERTIES "
                "LABELS FakeEvidence TIMEOUT 5)\n",
                encoding="utf-8",
            )
            query = build / ".cmake" / "api" / "v1" / "query"
            query.mkdir(parents=True)
            (query / "codemodel-v2").touch()
            (query / "cmakeFiles-v1").touch()
            subprocess.run(
                (
                    "cmake",
                    "-S",
                    str(root),
                    "-B",
                    str(build),
                    "-G",
                    "Ninja",
                    "-DCMAKE_BUILD_TYPE=Release",
                ),
                check=True,
                stdout=subprocess.DEVNULL,
            )
            subprocess.run(
                (
                    "cmake",
                    "--build",
                    str(build),
                    "--target",
                    "FakeEvidence",
                    "Unrelated",
                ),
                check=True,
                stdout=subprocess.DEVNULL,
            )
            artifact = build / (
                "FakeEvidence.exe" if os.name == "nt" else "FakeEvidence"
            )
            initial_artifact_time = artifact.stat().st_mtime_ns
            (build / "expected.txt").write_text("2\n", encoding="utf-8")
            header.write_text(
                "#pragma once\n#define CONTRACT_VALUE 2\nint contract_value(void);\n",
                encoding="utf-8",
            )
            implementation.write_text(
                '#include "contract.h"\n'
                "int contract_value(void) { return CONTRACT_VALUE + 0; }\n",
                encoding="utf-8",
            )
            subprocess.run(
                ("cmake", "--build", str(build), "--target", "Unrelated"),
                check=True,
                stdout=subprocess.DEVNULL,
            )
            (build / capabilities.BUILD_COMPLETION_STAMP).touch()
            document = {
                "schema": 2,
                "capabilities": [
                    {
                        "tests": [
                            {
                                "runner": "gtest",
                                "target": "FakeEvidence",
                                "source": "unittests/test.c",
                                "filter": "Suite.Transitive",
                                "platforms": [capabilities.current_test_platform()],
                            }
                        ]
                    }
                ],
            }

            diagnostics = capabilities.audit_configured_evidence(
                document, root, build, build_config="Release"
            )

            self.assertEqual(diagnostics, [])
            self.assertGreater(artifact.stat().st_mtime_ns, initial_artifact_time)

    def test_build_audit_executes_each_python_evidence_filter(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "source"
            build = Path(directory) / "build"
            source = root / "tests" / "test_proof.py"
            source.parent.mkdir(parents=True)
            source.write_text(
                "import unittest\n\n"
                "from pathlib import Path\n\n"
                "class ProofTests(unittest.TestCase):\n"
                "    def test_passes(self):\n"
                "        if Path('force-skip').exists():\n"
                "            self.skipTest('forced skip')\n"
                "        self.assertTrue(True)\n",
                encoding="utf-8",
            )
            python = Path(sys.executable).as_posix()
            (root / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.20)\n"
                "project(PythonCapabilityAudit NONE)\n"
                "include(CTest)\n"
                f'add_test(NAME PythonProof COMMAND "{python}" -m unittest '
                f'discover -s "{source.parent.as_posix()}" '
                '-p "test_proof.py" -v)\n'
                "set_tests_properties(PythonProof PROPERTIES "
                "LABELS PythonProof TIMEOUT 5)\n",
                encoding="utf-8",
            )
            query = build / ".cmake" / "api" / "v1" / "query"
            query.mkdir(parents=True)
            (query / "codemodel-v2").touch()
            (query / "cmakeFiles-v1").touch()
            subprocess.run(
                ("cmake", "-S", str(root), "-B", str(build), "-G", "Ninja"),
                check=True,
                stdout=subprocess.DEVNULL,
            )
            subprocess.run(
                ("cmake", "--build", str(build)),
                check=True,
                stdout=subprocess.DEVNULL,
            )
            (build / capabilities.BUILD_COMPLETION_STAMP).touch()
            document = {
                "schema": 2,
                "capabilities": [
                    {
                        "tests": [
                            {
                                "runner": "python-unittest",
                                "target": "PythonProof",
                                "source": "tests/test_proof.py",
                                "filter": "ProofTests.test_passes",
                                "platforms": [capabilities.current_test_platform()],
                            }
                        ]
                    }
                ],
            }

            diagnostics = capabilities.audit_configured_evidence(document, root, build)

            self.assertEqual(diagnostics, [])

            (build / "force-skip").touch()
            diagnostics = capabilities.audit_configured_evidence(document, root, build)
            self.assertTrue(
                any("skipped=1" in item for item in diagnostics), diagnostics
            )

    @unittest.skipUnless(os.name == "posix", "POSIX process-group assertion")
    def test_python_evidence_timeout_terminates_spawned_children(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "source"
            build = Path(directory) / "build"
            source = root / "tests" / "test_timeout.py"
            source.parent.mkdir(parents=True)
            source.write_text(
                "import subprocess\n"
                "import sys\n"
                "import time\n"
                "import unittest\n"
                "from pathlib import Path\n\n"
                "class ProofTests(unittest.TestCase):\n"
                "    def test_hangs(self):\n"
                "        child = subprocess.Popen(\n"
                "            [sys.executable, '-c', "
                "'import time; time.sleep(60)']\n"
                "        )\n"
                "        Path('evidence-child.pid').write_text(str(child.pid))\n"
                "        time.sleep(60)\n",
                encoding="utf-8",
            )
            python = Path(sys.executable).as_posix()
            (root / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.20)\n"
                "project(PythonCapabilityTimeout NONE)\n"
                "include(CTest)\n"
                f'add_test(NAME PythonTimeout COMMAND "{python}" -m unittest '
                f'discover -s "{source.parent.as_posix()}" '
                '-p "test_timeout.py" -v)\n'
                "set_tests_properties(PythonTimeout PROPERTIES "
                "LABELS PythonTimeout TIMEOUT 1)\n",
                encoding="utf-8",
            )
            query = build / ".cmake" / "api" / "v1" / "query"
            query.mkdir(parents=True)
            (query / "codemodel-v2").touch()
            (query / "cmakeFiles-v1").touch()
            subprocess.run(
                ("cmake", "-S", str(root), "-B", str(build), "-G", "Ninja"),
                check=True,
                stdout=subprocess.DEVNULL,
            )
            subprocess.run(
                ("cmake", "--build", str(build)),
                check=True,
                stdout=subprocess.DEVNULL,
            )
            (build / capabilities.BUILD_COMPLETION_STAMP).touch()
            document = {
                "schema": 2,
                "capabilities": [
                    {
                        "tests": [
                            {
                                "runner": "python-unittest",
                                "target": "PythonTimeout",
                                "source": "tests/test_timeout.py",
                                "filter": "ProofTests.test_hangs",
                                "platforms": [capabilities.current_test_platform()],
                            }
                        ]
                    }
                ],
            }
            child_pid_file = build / "evidence-child.pid"
            child_pid: int | None = None
            try:
                diagnostics = capabilities.audit_configured_evidence(
                    document, root, build
                )
                self.assertTrue(
                    any("timed out" in item for item in diagnostics),
                    diagnostics,
                )
                self.assertTrue(child_pid_file.is_file())
                child_pid = int(child_pid_file.read_text(encoding="utf-8"))
                completed = subprocess.run(
                    ("ps", "-o", "stat=", "-p", str(child_pid)),
                    check=False,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL,
                    text=True,
                )
                state = completed.stdout.strip()
                self.assertTrue(
                    completed.returncode != 0 or not state or state.startswith("Z"),
                    f"evidence child {child_pid} survived in state {state}",
                )
            finally:
                if child_pid is None and child_pid_file.is_file():
                    child_pid = int(child_pid_file.read_text(encoding="utf-8"))
                if child_pid is not None:
                    try:
                        os.kill(child_pid, 9)
                    except ProcessLookupError:
                        pass

    def test_ctest_execution_context_rejects_result_rewriting_properties(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            for name, value in (
                ("SKIP_RETURN_CODE", 0),
                ("PASS_REGULAR_EXPRESSION", ["looks good"]),
                ("FAIL_REGULAR_EXPRESSION", ["known failure"]),
                ("SKIP_REGULAR_EXPRESSION", ["skip me"]),
                ("DISABLED", "TRUE"),
                ("WILL_FAIL", "ON"),
                ("REQUIRED_FILES", ["fixture.bin"]),
                ("FIXTURES_REQUIRED", ["database"]),
            ):
                with self.subTest(property=name):
                    context, error = capabilities._ctest_execution_context(
                        {"properties": [{"name": name, "value": value}]},
                        build,
                    )
                    self.assertIsNone(context)
                    self.assertIn(name, error or "")

    def test_ctest_context_allows_only_cmake_gtest_skip_marker(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            for value in (
                [r"\[  SKIPPED \]"],
                [r"\\[  SKIPPED \\]", r"\\[  SKIPPED \\]"],
            ):
                with self.subTest(value=value):
                    context, error = capabilities._ctest_execution_context(
                        {
                            "properties": [
                                {
                                    "name": "SKIP_REGULAR_EXPRESSION",
                                    "value": value,
                                }
                            ]
                        },
                        Path(directory),
                        allow_cmake_gtest_skip=True,
                    )

                    self.assertIsNotNone(context)
                    self.assertIsNone(error)

            context, error = capabilities._ctest_execution_context(
                {
                    "properties": [
                        {
                            "name": "SKIP_REGULAR_EXPRESSION",
                            "value": [r"\\[  SKIPPED \\]", "skip me"],
                        }
                    ]
                },
                Path(directory),
                allow_cmake_gtest_skip=True,
            )
            self.assertIsNone(context)
            self.assertIn("SKIP_REGULAR_EXPRESSION", error or "")

    def test_ctest_gtest_execution_ignores_same_name_from_other_artifact(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            target = build / "TargetTests"
            decoy = build / "AggregateTests"
            target.touch()
            decoy.touch()
            properties = [
                {
                    "name": "SKIP_REGULAR_EXPRESSION",
                    "value": [r"\\[  SKIPPED \\]", r"\\[  SKIPPED \\]"],
                },
                {"name": "WORKING_DIRECTORY", "value": str(build)},
            ]
            configured_artifacts = frozenset({target.resolve()})

            target_key, target_error = capabilities._ctest_gtest_execution_key(
                {
                    "command": [
                        str(target),
                        "--gtest_filter=Suite.Proof",
                        "--gtest_also_run_disabled_tests",
                    ],
                    "properties": properties,
                },
                build,
                configured_artifacts,
            )
            decoy_key, decoy_error = capabilities._ctest_gtest_execution_key(
                {
                    "command": [
                        str(decoy),
                        "--gtest_filter=Suite.Proof",
                        "--gtest_also_run_disabled_tests",
                    ],
                    "properties": properties,
                },
                build,
                configured_artifacts,
            )

            self.assertIsNone(target_error)
            self.assertIsNotNone(target_key)
            self.assertEqual(target_key[0], target.resolve())
            self.assertIsNone(decoy_key)
            self.assertIsNone(decoy_error)

    def test_gtest_list_parser_preserves_parameterized_runtime_names(self) -> None:
        inventory = capabilities.parse_gtest_list_tests(
            "SixFormatMatrix/LowIRConcolicIntegration.\n"
            "  FindsFlip/ELF_X64  # GetParam() = ELF_X64\n"
            "TypedSuite/0.  # TypeParam = int\n"
            "  HandlesValue\n"
        )

        self.assertEqual(
            inventory,
            frozenset(
                {
                    "SixFormatMatrix/LowIRConcolicIntegration.FindsFlip/ELF_X64",
                    "TypedSuite/0.HandlesValue",
                }
            ),
        )
        self.assertTrue(
            any(
                capabilities.fnmatch.fnmatchcase(
                    name, "*/LowIRConcolicIntegration.FindsFlip/*"
                )
                for name in inventory
            )
        )

    def test_gtest_xml_parser_reports_only_executed_runtime_tests(self) -> None:
        result = capabilities.parse_gtest_xml_result(
            '<?xml version="1.0"?>\n'
            '<testsuites tests="2" failures="0" disabled="0" '
            'errors="0" skipped="0">\n'
            '  <testsuite name="Prefix/Suite" tests="2">\n'
            '    <testcase name="Works/0" classname="Prefix/Suite" '
            'status="run" result="completed"/>\n'
            '    <testcase name="Works/1" classname="Prefix/Suite" '
            'status="run" result="completed"/>\n'
            "  </testsuite>\n"
            "</testsuites>\n"
        )

        self.assertEqual(result.tests, 2)
        self.assertEqual(result.failures, 0)
        self.assertEqual(result.errors, 0)
        self.assertEqual(result.disabled, 0)
        self.assertEqual(result.skipped, 0)
        self.assertEqual(
            result.runtime_names,
            frozenset({"Prefix/Suite.Works/0", "Prefix/Suite.Works/1"}),
        )

    def test_gtest_xml_parser_preserves_skip_and_disabled_counts(self) -> None:
        result = capabilities.parse_gtest_xml_result(
            '<testsuites tests="2" failures="0" disabled="1" '
            'errors="0">'
            '<testsuite name="Suite" tests="2" skipped="1">'
            '<testcase name="Skipped" classname="Suite" status="run" '
            'result="skipped"><skipped/></testcase>'
            '<testcase name="DISABLED_NotRun" classname="Suite" '
            'status="notrun" result="suppressed"/>'
            "</testsuite></testsuites>"
        )

        self.assertEqual(result.disabled, 1)
        self.assertEqual(result.skipped, 1)
        self.assertEqual(result.runtime_names, frozenset())

    def test_python_unittest_parser_requires_normal_success(self) -> None:
        result = capabilities.parse_python_unittest_result(
            "test_one (test_api.SessionTests.test_one) ... ok\n"
            "test_two (test_api.SessionTests.test_two) ... ok\n"
            "\n----------------------------------------------------------------------\n"
            "Ran 2 tests in 0.001s\n\nOK\n"
        )

        self.assertEqual(result.tests, 2)
        self.assertEqual(result.failures, 0)
        self.assertEqual(result.errors, 0)
        self.assertEqual(result.skipped, 0)
        self.assertEqual(
            result.runtime_names,
            frozenset(
                {
                    "test_api.SessionTests.test_one",
                    "test_api.SessionTests.test_two",
                }
            ),
        )

    def test_python_unittest_parser_does_not_treat_skip_as_success(self) -> None:
        result = capabilities.parse_python_unittest_result(
            "test_one (test_api.SessionTests.test_one) ... skipped 'missing'\n"
            "\n----------------------------------------------------------------------\n"
            "Ran 1 test in 0.001s\n\nOK (skipped=1)\n"
        )

        self.assertEqual(result.tests, 1)
        self.assertEqual(result.skipped, 1)
        self.assertEqual(
            result.runtime_names,
            frozenset({"test_api.SessionTests.test_one"}),
        )

    def test_python_unittest_parser_accepts_windows_line_endings(self) -> None:
        result = capabilities.parse_python_unittest_result(
            "test_one (test_api.SessionTests.test_one) ... ok\r\n"
            "\r\n----------------------------------------------------------------------\r\n"
            "Ran 1 test in 0.001s\r\n\r\nOK\r\n"
        )

        self.assertEqual(result.tests, 1)
        self.assertEqual(result.failures, 0)
        self.assertEqual(result.errors, 0)
        self.assertEqual(result.skipped, 0)
        self.assertEqual(
            result.runtime_names,
            frozenset({"test_api.SessionTests.test_one"}),
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
                    {"schema": 2, "capabilities": [row]}, root
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
                    {"schema": 2, "capabilities": [row]}, root
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
                        "schema": 2,
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
    def test_native_translation_evidence_matches_ci_host_architectures(self) -> None:
        document = json.loads(
            (ROOT / "docs" / "capabilities.json").read_text(encoding="utf-8")
        )
        executable_engine = next(
            row
            for row in document["capabilities"]
            if row["id"] == "translation.executable-engine"
        )
        portable_filters = {
            "NativeTranslationSession."
            "AcceptedCancellationWinsTheSuccessfulCommitLinearizationPoint",
            "NativeTranslationSession."
            "ValidatesAResolvedConditionalBranchAgainstBothManifestSuccessors",
        }

        for evidence in executable_engine["tests"]:
            expected = (
                ["darwin", "linux", "windows"]
                if evidence["filter"] in portable_filters
                else ["darwin"]
            )
            with self.subTest(test_filter=evidence["filter"]):
                self.assertEqual(evidence["platforms"], expected)

    def test_repository_manifest_is_honest_and_executable(self) -> None:
        document = json.loads(
            (ROOT / "docs" / "capabilities.json").read_text(encoding="utf-8")
        )

        self.assertEqual(
            capabilities.validate_manifest(
                document, ROOT, enforce_surface_completeness=True
            ),
            [],
        )
        self.assertEqual(
            {row["id"]: row["status"] for row in document["capabilities"]},
            {
                "debug.hardware": "unsupported",
                "debug.local": "unsupported",
                "debug.remote": "unsupported",
                "exception.itanium.ada-d": "experimental",
                "exception.rewrite.end-to-end": "unsupported",
                "safety.binary-sanitizer-publication": "experimental",
                "safety.process-replay-native-execution": "unsupported",
                "semantic.mba.derivation": "supported",
                "semantic.synthesis.candidate": "supported",
                "symbolic.execution.lowir-concolic": "experimental",
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
            "safety.binary-sanitizer-publication": {
                "c": [
                    "neverd_sanitize_publication_abi_version",
                    "neverd_session_sanitize",
                ],
                "python": ["Session.sanitize"],
                "cli": ["neverd patch --sanitize=strict"],
                "json": [],
            },
            "safety.process-replay-native-execution": no_surfaces,
            "semantic.mba.derivation": {
                "c": ["neverd_simplify_expr", "neverd_simplify_expr_json"],
                "python": ["simplify_expression"],
                "cli": ["neverd simplify"],
                "json": ["neverd_simplify_expr_json"],
            },
            "semantic.synthesis.candidate": {
                "c": [
                    "neverd_synthesize_expr",
                    "neverd_synthesize_expr_json_v1",
                ],
                "python": ["synthesize_expression"],
                "cli": ["neverd simplify --synthesize"],
                "json": ["neverd_synthesize_expr_json_v1"],
            },
            "symbolic.execution.path-exploration": {
                "c": ["neverd_symbolic_explore_json"],
                "python": ["Session.symbolic_explore"],
                "cli": ["neverd sym-explore"],
                "json": ["neverd_symbolic_explore_json"],
            },
            "symbolic.execution.lowir-concolic": {
                "c": ["neverd_lowir_concolic_json_v1"],
                "python": ["Session.lowir_concolic"],
                "cli": [
                    "neverd concolic",
                    "neverd concolic --func",
                    "neverd concolic --max-block-visits",
                    "neverd concolic --max-candidates",
                    "neverd concolic --max-flip-attempts",
                    "neverd concolic --max-loop-iterations",
                    "neverd concolic --max-steps",
                    "neverd concolic --o",
                    "neverd concolic --seed",
                    "neverd concolic --solver-conflicts",
                    "neverd concolic --solver-gates",
                    "neverd concolic --solver-propagations",
                    "neverd concolic --solver-watch-visits",
                ],
                "json": ["neverd_lowir_concolic_json_v1"],
            },
            "translation.executable-engine": no_surfaces,
            "translation.runtime-contract": {
                "c": ["neverd_translate_x86_64_block_to_aarch64_object_v1"],
                "python": ["translate_x86_64_block_to_aarch64_object"],
                "cli": ["neverd translate-object"],
                "json": [],
            },
            "llvm.semantic.synthesis-rewrite": {
                "c": [
                    "neverd_synthesize_expr",
                    "neverd_synthesize_expr_json_v1",
                    "neverd_optimize_llvm_ir",
                    "neverd_optimize_llvm_ir_json_v1",
                ],
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

        self.assertEqual(document["schema"], 2)
        evidence_keys = {
            "runner",
            "target",
            "source",
            "filter",
            "platforms",
        }
        for row in document["capabilities"]:
            for evidence in row["tests"]:
                self.assertEqual(set(evidence), evidence_keys)
            for field in capabilities.EVIDENCE_FIELDS:
                if field in row:
                    self.assertEqual(set(row[field]), evidence_keys)

        publication = next(
            row
            for row in document["capabilities"]
            if row["id"] == "safety.binary-sanitizer-publication"
        )
        self.assertTrue(
            any(
                "receipt is an attestation summary rather than a persistent "
                "independently re-verifiable path binding" in limitation
                for limitation in publication["limitations"]
            )
        )
        evidence_by_filter = {
            evidence["filter"]: evidence for evidence in publication["tests"]
        }
        self.assertEqual(
            evidence_by_filter[
                "DarwinSanitizerPublicationAdapterNativeTest."
                "PublishesInRealAnchoredTemporaryDirectory"
            ]["platforms"],
            ["darwin"],
        )
        self.assertEqual(
            evidence_by_filter[
                "RuntimeSanitizerCLI."
                "StrictSanitizeFailsBeforeMutationWithoutNativePublicationReceipt"
            ]["platforms"],
            ["linux", "windows"],
        )

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
        query = "build-ci/.cmake/api/v1/query/codemodel-v2"
        cmake_files_query = "build-ci/.cmake/api/v1/query/cmakeFiles-v1"
        completion_stamp = "build-ci/.neverd-capability-build-complete"
        inventory_path = "build-ci/ctest-inventory.json"
        inventory_capture = f"tee {inventory_path}"
        configured_check = "--build-dir build-ci --build-config Release"
        configured_inventory = f"--ctest-inventory {inventory_path}"
        self.assertIn(query, workflow)
        self.assertIn(cmake_files_query, workflow)
        self.assertLess(workflow.index(query), workflow.index("cmake -S"))
        self.assertLess(workflow.index(cmake_files_query), workflow.index("cmake -S"))
        self.assertIn(completion_stamp, workflow)
        self.assertIn(inventory_capture, workflow)
        main_build = next(
            line for line in workflow.splitlines() if "cmake --build build-ci" in line
        )
        self.assertNotIn("--target", main_build)
        self.assertGreater(
            workflow.index(completion_stamp), workflow.index("cmake --build build-ci")
        )
        self.assertIn(configured_check, workflow)
        self.assertIn(configured_inventory, workflow)
        self.assertGreater(
            workflow.index(configured_check),
            workflow.index("Run selected test profile"),
        )
        self.assertLess(
            workflow.index(inventory_capture), workflow.index(configured_inventory)
        )
        self.assertLess(
            workflow.index(completion_stamp), workflow.index(configured_check)
        )

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
