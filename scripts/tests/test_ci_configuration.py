import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CMAKE_HELPERS = ROOT / "cmake" / "AddNeverD.cmake"
WORKFLOW = ROOT / ".github" / "workflows" / "ci.yml"


class CiConfigurationTests(unittest.TestCase):
    def test_google_test_discovery_is_serial_and_bounded(self):
        source = CMAKE_HELPERS.read_text(encoding="utf-8")
        self.assertIn("DISCOVERY_MODE PRE_TEST", source)
        self.assertIn("DISCOVERY_TIMEOUT 120", source)
        self.assertNotIn("DISCOVERY_MODE POST_BUILD", source)
        self.assertNotIn("DISCOVERY_TIMEOUT -1", source)

    def test_workflow_declares_all_three_test_profiles(self):
        source = WORKFLOW.read_text(encoding="utf-8")
        self.assertEqual(source.count("test_profile:"), 3)
        self.assertIn("test_profile: linux-semantic", source)
        self.assertIn("exclude_labels: '^NeverDPatchFullTests$'", source)
        self.assertIn("test_profile: macos-patch", source)
        self.assertIn("exclude_labels: '^NeverDSemanticTests$'", source)
        self.assertIn("test_profile: windows-focused", source)
        self.assertIn(
            "exclude_labels: '^NeverD(Semantic|PatchFull)Tests$'", source
        )

    def test_workflow_audits_json_then_uses_the_approved_expression(self):
        source = WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("--show-only=json-v1", source)
        self.assertIn('"$PYTHON" scripts/audit_ci_test_inventory.py', source)
        self.assertIn('--profile "$TEST_PROFILE"', source)
        self.assertIn('--exclude-label-regex "$EXCLUDE_LABELS"', source)
        self.assertIn('--label-exclude "$EXCLUDE_LABELS"', source)
        self.assertIn(
            "EXCLUDE_LABELS: ${{ steps.inventory.outputs.label_exclude }}", source
        )
        self.assertIn(
            "EXPECTED_TESTS: ${{ steps.inventory.outputs.count }}", source
        )

    def test_workflow_still_builds_the_unfiltered_default_target(self):
        source = WORKFLOW.read_text(encoding="utf-8")
        self.assertIn(
            'run: cmake --build build-ci --config Release --parallel "$BUILD_PARALLEL"',
            source,
        )
        build_section = source.split("- name: Build default targets", 1)[1].split(
            "- name:", 1
        )[0]
        self.assertNotIn("--target", build_section)
