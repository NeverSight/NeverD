import tempfile
import unittest
from pathlib import Path

from scripts.audit_ci_test_inventory import (
    InventoryError,
    audit_inventory,
    write_github_reports,
)


SEMANTIC = "NeverDSemanticTests"
PATCH = "NeverDPatchFullTests"


def ctest_inventory(*entries: tuple[str, tuple[str, ...]]) -> dict:
    return {
        "version": {"major": 1, "minor": 0},
        "kind": "ctestInfo",
        "tests": [
            {
                "name": name,
                "properties": [{"name": "LABELS", "value": list(labels)}],
            }
            for name, labels in entries
        ],
    }


def valid_inventory() -> dict:
    return ctest_inventory(
        ("semantic/a", (SEMANTIC,)),
        ("semantic/b", (SEMANTIC,)),
        ("patch/a", (PATCH,)),
        ("patch/b", (PATCH,)),
        ("lift/a", ("NeverDLiftTests",)),
        ("cfg/a", ("NeverDCFGLoopXformTests",)),
    )


class AuditInventoryTests(unittest.TestCase):
    def audit(self, profile: str, expression: str):
        return audit_inventory(
            valid_inventory(),
            profile,
            expression,
            semantic_minimum=2,
            patch_minimum=2,
        )

    def test_linux_owns_semantic_and_keeps_focused_tests(self):
        result = self.audit("linux-semantic", r"^NeverDPatchFullTests$")
        self.assertEqual(result.full_count, 6)
        self.assertEqual(result.semantic_count, 2)
        self.assertEqual(result.patch_count, 2)
        self.assertEqual(result.selected_count, 4)
        self.assertEqual(result.excluded_count, 2)
        self.assertEqual(
            set(result.selected_names),
            {"semantic/a", "semantic/b", "lift/a", "cfg/a"},
        )

    def test_macos_owns_patch_and_keeps_focused_tests(self):
        result = self.audit("macos-patch", r"^NeverDSemanticTests$")
        self.assertEqual(result.selected_count, 4)
        self.assertEqual(
            set(result.selected_names),
            {"patch/a", "patch/b", "lift/a", "cfg/a"},
        )

    def test_windows_keeps_only_focused_tests(self):
        result = self.audit(
            "windows-focused", r"^NeverD(Semantic|PatchFull)Tests$"
        )
        self.assertEqual(result.selected_count, 2)
        self.assertEqual(set(result.selected_names), {"lift/a", "cfg/a"})

    def test_profile_rejects_a_different_expression(self):
        with self.assertRaisesRegex(InventoryError, "does not match profile"):
            self.audit("linux-semantic", r"^NeverDSemanticTests$")

    def test_duplicate_names_fail_with_a_bounded_diagnostic(self):
        document = valid_inventory()
        document["tests"].append(document["tests"][0])
        with self.assertRaisesRegex(InventoryError, "duplicate CTest names"):
            audit_inventory(
                document,
                "linux-semantic",
                r"^NeverDPatchFullTests$",
                semantic_minimum=2,
                patch_minimum=2,
            )

    def test_a_test_cannot_belong_to_both_exhaustive_suites(self):
        document = valid_inventory()
        document["tests"][0]["properties"][0]["value"].append(PATCH)
        with self.assertRaisesRegex(InventoryError, "overlap"):
            audit_inventory(
                document,
                "linux-semantic",
                r"^NeverDPatchFullTests$",
                semantic_minimum=2,
                patch_minimum=2,
            )

    def test_semantic_count_below_floor_fails(self):
        document = valid_inventory()
        document["tests"] = [
            test for test in document["tests"] if test["name"] != "semantic/b"
        ]
        with self.assertRaisesRegex(InventoryError, "semantic inventory.*minimum"):
            audit_inventory(
                document,
                "linux-semantic",
                r"^NeverDPatchFullTests$",
                semantic_minimum=2,
                patch_minimum=2,
            )

    def test_patch_count_below_floor_fails(self):
        document = valid_inventory()
        document["tests"] = [
            test for test in document["tests"] if test["name"] != "patch/b"
        ]
        with self.assertRaisesRegex(InventoryError, "patch inventory.*minimum"):
            audit_inventory(
                document,
                "macos-patch",
                r"^NeverDSemanticTests$",
                semantic_minimum=2,
                patch_minimum=2,
            )

    def test_not_built_sentinel_fails(self):
        document = valid_inventory()
        document["tests"].append(
            {
                "name": "NeverDTestProcessTests_NOT_BUILT",
                "properties": [],
            }
        )
        with self.assertRaisesRegex(InventoryError, "NOT_BUILT"):
            audit_inventory(
                document,
                "windows-focused",
                r"^NeverD(Semantic|PatchFull)Tests$",
                semantic_minimum=2,
                patch_minimum=2,
            )

    def test_malformed_labels_fail(self):
        document = valid_inventory()
        document["tests"][0]["properties"][0]["value"] = SEMANTIC
        with self.assertRaisesRegex(InventoryError, "malformed LABELS"):
            audit_inventory(
                document,
                "linux-semantic",
                r"^NeverDPatchFullTests$",
                semantic_minimum=2,
                patch_minimum=2,
            )

    def test_reports_include_profile_expression_and_all_counts(self):
        result = self.audit("linux-semantic", r"^NeverDPatchFullTests$")
        with tempfile.TemporaryDirectory() as directory:
            output_path = Path(directory) / "output"
            summary_path = Path(directory) / "summary"
            summary = write_github_reports(
                result,
                "Linux x64",
                output_path=output_path,
                summary_path=summary_path,
            )

            outputs = output_path.read_text(encoding="utf-8")
            self.assertIn("count=4\n", outputs)
            self.assertIn("full_count=6\n", outputs)
            self.assertIn("semantic_count=2\n", outputs)
            self.assertIn("patch_count=2\n", outputs)
            self.assertIn("excluded_count=2\n", outputs)
            self.assertIn("label_exclude=^NeverDPatchFullTests$\n", outputs)
            self.assertIn("### Linux x64", summary)
            self.assertIn("| Selected | 4 |", summary)
            self.assertEqual(summary_path.read_text(encoding="utf-8"), summary)
