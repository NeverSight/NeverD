import tempfile
import unittest
from pathlib import Path

from scripts.audit_ci_test_inventory import (
    CONCOLIC_LABELS,
    CORPUS_LABELS,
    PLUGIN_LABELS,
    SAFETY_LABELS,
    InventoryError,
    audit_inventory,
    write_github_reports,
)


SEMANTIC = "NeverDSemanticTests"
PATCH = "NeverDPatchFullTests"
CORPUS_NAMES = tuple(f"corpus/{label}" for label in CORPUS_LABELS)
PLUGIN_NAMES = tuple(f"plugin/{label}" for label in PLUGIN_LABELS)
SAFETY_NAMES = tuple(f"safety/{label}" for label in SAFETY_LABELS)
CONCOLIC_NAMES = tuple(f"concolic/{label}" for label in CONCOLIC_LABELS)


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
    # Every profile runs the corpus and native example smoke, so an inventory
    # without either is not one a profile could be selected from.
    return ctest_inventory(
        ("semantic/a", (SEMANTIC,)),
        ("semantic/b", (SEMANTIC,)),
        ("patch/a", (PATCH,)),
        ("patch/b", (PATCH,)),
        ("lift/a", ("NeverDLiftTests",)),
        ("cfg/a", ("NeverDCFGLoopXformTests",)),
        *((name, (label,)) for name, label in zip(PLUGIN_NAMES, PLUGIN_LABELS)),
        *((name, (label,)) for name, label in zip(SAFETY_NAMES, SAFETY_LABELS)),
        *((name, (label,)) for name, label in zip(CONCOLIC_NAMES, CONCOLIC_LABELS)),
        *((name, (label,)) for name, label in zip(CORPUS_NAMES, CORPUS_LABELS)),
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
        self.assertEqual(
            result.full_count,
            8 + len(PLUGIN_NAMES) + len(CONCOLIC_NAMES) + len(CORPUS_NAMES),
        )
        self.assertEqual(result.semantic_count, 2)
        self.assertEqual(result.patch_count, 2)
        self.assertEqual(
            result.selected_count,
            6 + len(PLUGIN_NAMES) + len(CONCOLIC_NAMES) + len(CORPUS_NAMES),
        )
        self.assertEqual(result.excluded_count, 2)
        self.assertEqual(
            set(result.selected_names),
            {
                "semantic/a",
                "semantic/b",
                "lift/a",
                "cfg/a",
                *PLUGIN_NAMES,
                *SAFETY_NAMES,
                *CONCOLIC_NAMES,
                *CORPUS_NAMES,
            },
        )

    def test_macos_owns_patch_and_keeps_focused_tests(self):
        result = self.audit("macos-patch", r"^NeverDSemanticTests$")
        self.assertEqual(
            result.selected_count,
            6 + len(PLUGIN_NAMES) + len(CONCOLIC_NAMES) + len(CORPUS_NAMES),
        )
        self.assertEqual(
            set(result.selected_names),
            {
                "patch/a",
                "patch/b",
                "lift/a",
                "cfg/a",
                *PLUGIN_NAMES,
                *SAFETY_NAMES,
                *CONCOLIC_NAMES,
                *CORPUS_NAMES,
            },
        )

    def test_windows_keeps_only_focused_tests(self):
        result = self.audit(
            "windows-focused", r"^NeverD(Semantic|PatchFull)Tests$"
        )
        self.assertEqual(
            result.selected_count,
            4 + len(PLUGIN_NAMES) + len(CONCOLIC_NAMES) + len(CORPUS_NAMES),
        )
        self.assertEqual(
            set(result.selected_names),
            {
                "lift/a",
                "cfg/a",
                *PLUGIN_NAMES,
                *SAFETY_NAMES,
                *CONCOLIC_NAMES,
                *CORPUS_NAMES,
            },
        )

    # Every host reads the corpus.  The bytes are the same everywhere, but what
    # reads them is not: a path, a filesystem and a `std::filesystem` differ by
    # host, and a corpus run on one host proves nothing about the other two.
    def test_every_profile_selects_the_whole_corpus(self):
        for profile, expression in (
            ("linux-semantic", r"^NeverDPatchFullTests$"),
            ("macos-patch", r"^NeverDSemanticTests$"),
            ("windows-focused", r"^NeverD(Semantic|PatchFull)Tests$"),
        ):
            with self.subTest(profile=profile):
                result = self.audit(profile, expression)
                self.assertTrue(set(CORPUS_NAMES) <= set(result.selected_names))

    def test_every_profile_selects_the_safety_suites(self):
        for profile, expression in (
            ("linux-semantic", r"^NeverDPatchFullTests$"),
            ("macos-patch", r"^NeverDSemanticTests$"),
            ("windows-focused", r"^NeverD(Semantic|PatchFull)Tests$"),
        ):
            with self.subTest(profile=profile):
                result = self.audit(profile, expression)
                self.assertTrue(set(SAFETY_NAMES) <= set(result.selected_names))

    def test_every_profile_selects_the_native_example_plugin_suite(self):
        for profile, expression in (
            ("linux-semantic", r"^NeverDPatchFullTests$"),
            ("macos-patch", r"^NeverDSemanticTests$"),
            ("windows-focused", r"^NeverD(Semantic|PatchFull)Tests$"),
        ):
            with self.subTest(profile=profile):
                result = self.audit(profile, expression)
                self.assertTrue(set(PLUGIN_NAMES) <= set(result.selected_names))

    def test_every_profile_selects_all_concolic_suites(self):
        for profile, expression in (
            ("linux-semantic", r"^NeverDPatchFullTests$"),
            ("macos-patch", r"^NeverDSemanticTests$"),
            ("windows-focused", r"^NeverD(Semantic|PatchFull)Tests$"),
        ):
            with self.subTest(profile=profile):
                result = self.audit(profile, expression)
                self.assertTrue(set(CONCOLIC_NAMES) <= set(result.selected_names))

    def test_a_build_that_left_concolic_out_fails(self):
        for label, name in zip(CONCOLIC_LABELS, CONCOLIC_NAMES):
            document = valid_inventory()
            document["tests"] = [
                test for test in document["tests"] if test["name"] != name
            ]
            with self.subTest(label=label):
                with self.assertRaisesRegex(InventoryError, label):
                    audit_inventory(
                        document,
                        "linux-semantic",
                        r"^NeverDPatchFullTests$",
                        semantic_minimum=2,
                        patch_minimum=2,
                    )

    def test_profile_cannot_exclude_required_concolic_suites(self):
        document = valid_inventory()
        concolic_test = next(
            test for test in document["tests"] if test["name"] == CONCOLIC_NAMES[0]
        )
        concolic_test["properties"][0]["value"].append(PATCH)
        with self.assertRaisesRegex(
            InventoryError, "does not select required concolic tests"
        ):
            audit_inventory(
                document,
                "linux-semantic",
                r"^NeverDPatchFullTests$",
                semantic_minimum=2,
                patch_minimum=2,
            )

    def test_a_build_that_left_the_native_example_plugin_out_fails(self):
        for label, name in zip(PLUGIN_LABELS, PLUGIN_NAMES):
            document = valid_inventory()
            document["tests"] = [
                test for test in document["tests"] if test["name"] != name
            ]
            with self.subTest(label=label):
                with self.assertRaisesRegex(InventoryError, label):
                    audit_inventory(
                        document,
                        "linux-semantic",
                        r"^NeverDPatchFullTests$",
                        semantic_minimum=2,
                        patch_minimum=2,
                    )

    def test_profile_cannot_exclude_the_required_native_example_plugin(self):
        document = valid_inventory()
        plugin_test = next(
            test for test in document["tests"] if test["name"] == PLUGIN_NAMES[0]
        )
        plugin_test["properties"][0]["value"].append(PATCH)
        with self.assertRaisesRegex(
            InventoryError, "does not select required native plugin tests"
        ):
            audit_inventory(
                document,
                "linux-semantic",
                r"^NeverDPatchFullTests$",
                semantic_minimum=2,
                patch_minimum=2,
            )

    def test_a_build_that_left_safety_out_fails(self):
        for label, name in zip(SAFETY_LABELS, SAFETY_NAMES):
            document = valid_inventory()
            document["tests"] = [
                test for test in document["tests"] if test["name"] != name
            ]
            with self.subTest(label=label):
                with self.assertRaisesRegex(InventoryError, label):
                    audit_inventory(
                        document,
                        "linux-semantic",
                        r"^NeverDPatchFullTests$",
                        semantic_minimum=2,
                        patch_minimum=2,
                    )

    def test_a_build_that_left_the_corpus_out_fails(self):
        for label, name in zip(CORPUS_LABELS, CORPUS_NAMES):
            document = valid_inventory()
            document["tests"] = [
                test for test in document["tests"] if test["name"] != name
            ]
            with self.subTest(label=label):
                with self.assertRaisesRegex(InventoryError, label):
                    audit_inventory(
                        document,
                        "linux-semantic",
                        r"^NeverDPatchFullTests$",
                        semantic_minimum=2,
                        patch_minimum=2,
                    )

    def test_profile_rejects_a_different_expression(self):
        with self.assertRaisesRegex(InventoryError, "does not match profile"):
            self.audit("linux-semantic", r"^NeverDSemanticTests$")

    def test_same_name_on_a_focused_binary_is_not_a_duplicate(self):
        document = valid_inventory()
        document["tests"].append(
            {
                "name": "semantic/a",
                "properties": [{"name": "LABELS", "value": ["NeverDFP16ReduceTests"]}],
            }
        )
        result = audit_inventory(
            document,
            "macos-patch",
            r"^NeverDSemanticTests$",
            semantic_minimum=2,
            patch_minimum=2,
        )
        self.assertIn("semantic/a", result.selected_names)

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

            selected = (
                6 + len(PLUGIN_NAMES) + len(CONCOLIC_NAMES) + len(CORPUS_NAMES)
            )
            outputs = output_path.read_text(encoding="utf-8")
            self.assertIn(f"count={selected}\n", outputs)
            self.assertIn(
                "full_count="
                f"{8 + len(PLUGIN_NAMES) + len(CONCOLIC_NAMES) + len(CORPUS_NAMES)}\n",
                outputs,
            )
            self.assertIn("semantic_count=2\n", outputs)
            self.assertIn("patch_count=2\n", outputs)
            self.assertIn("excluded_count=2\n", outputs)
            self.assertIn("label_exclude=^NeverDPatchFullTests$\n", outputs)
            self.assertIn("### Linux x64", summary)
            self.assertIn(f"| Selected | {selected} |", summary)
            self.assertEqual(summary_path.read_text(encoding="utf-8"), summary)
