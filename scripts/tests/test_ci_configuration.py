import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CMAKE_HELPERS = ROOT / "cmake" / "AddNeverD.cmake"
SEMANTIC_CMAKE = ROOT / "unittests" / "semantic" / "CMakeLists.txt"
WORKFLOW = ROOT / ".github" / "workflows" / "ci.yml"


class CiConfigurationTests(unittest.TestCase):
    def test_google_test_discovery_is_serial_configurable_and_bounded(self):
        source = CMAKE_HELPERS.read_text(encoding="utf-8")
        self.assertIn('"TIMEOUT;DISCOVERY_TIMEOUT"', source)
        self.assertIn(
            'if(NOT DEFINED ARG_DISCOVERY_TIMEOUT OR ARG_DISCOVERY_TIMEOUT STREQUAL "")',
            source,
        )
        self.assertIn("set(ARG_DISCOVERY_TIMEOUT 120)", source)
        self.assertIn("DISCOVERY_MODE PRE_TEST", source)
        self.assertIn("DISCOVERY_TIMEOUT ${ARG_DISCOVERY_TIMEOUT}", source)
        self.assertNotIn("DISCOVERY_MODE POST_BUILD", source)
        self.assertNotIn("DISCOVERY_TIMEOUT -1", source)

    def test_inventory_heavy_test_suites_extend_discovery_timeout(self):
        source = SEMANTIC_CMAKE.read_text(encoding="utf-8")

        self.assertEqual(source.count("  DISCOVERY_TIMEOUT 600\n"), 2)
        for target in ("NeverDSemanticTests", "NeverDPatchFullTests"):
            marker = f"add_neverd_unittest({target}\n"
            with self.subTest(target=target):
                self.assertEqual(source.count(marker), 1)
                invocation = source.split(marker, 1)[1].split("\n)", 1)[0]
                self.assertIn("  DISCOVERY_TIMEOUT 600\n", f"{invocation}\n")

        patch_invocation = source.split(
            "add_neverd_unittest(NeverDPatchFullTests\n", 1
        )[1].split("\n)", 1)[0]
        self.assertIn("  TIMEOUT 1800\n", f"{patch_invocation}\n")

    def test_workflow_declares_all_three_test_profiles(self):
        source = WORKFLOW.read_text(encoding="utf-8")
        matrix_source = source.split("        include:\n", 1)[1].split(
            "\n    steps:", 1
        )[0]
        expected_profiles = {
            "Linux x64": (
                "runner: ubuntu-24.04",
                "test_profile: linux-semantic",
                "exclude_labels: '^NeverDPatchFullTests$'",
                "python: python3",
            ),
            "macOS arm64": (
                "runner: macos-15",
                "test_profile: macos-patch",
                "exclude_labels: '^NeverDSemanticTests$'",
                "python: python3",
            ),
            "Windows x64": (
                "runner: windows-latest",
                "test_profile: windows-focused",
                "exclude_labels: '^NeverD(Semantic|PatchFull)Tests$'",
                "python: python",
            ),
        }

        self.assertEqual(matrix_source.count("test_profile:"), len(expected_profiles))
        for matrix_name, expected_fields in expected_profiles.items():
            entry_marker = f"          - name: {matrix_name}\n"
            with self.subTest(matrix_name=matrix_name):
                self.assertEqual(matrix_source.count(entry_marker), 1)
                entry = matrix_source.split(entry_marker, 1)[1].split(
                    "\n          - name:", 1
                )[0]
                for expected_field in expected_fields:
                    self.assertIn(f"            {expected_field}\n", f"{entry}\n")

    def test_workflow_audits_json_then_uses_the_approved_expression(self):
        source = WORKFLOW.read_text(encoding="utf-8")

        def named_step(step_name):
            step_marker = f"      - name: {step_name}\n"
            self.assertEqual(source.count(step_marker), 1)
            return source.split(step_marker, 1)[1].split("\n      - name:", 1)[0]

        audit_step = named_step("Audit and select test profile")
        audit_contract = (
            "id: inventory",
            "ctest --test-dir build-ci --build-config Release --show-only=json-v1 |",
            '"$PYTHON" scripts/audit_ci_test_inventory.py',
            '--profile "$TEST_PROFILE"',
            '--exclude-label-regex "$EXCLUDE_LABELS"',
            '--matrix-name "$MATRIX_NAME"',
            "EXCLUDE_LABELS: ${{ matrix.exclude_labels }}",
            "MATRIX_NAME: ${{ matrix.name }}",
            "PYTHON: ${{ matrix.python }}",
            "TEST_PROFILE: ${{ matrix.test_profile }}",
        )
        for expected in audit_contract:
            with self.subTest(step="audit", expected=expected):
                self.assertIn(expected, audit_step)

        run_step = named_step("Run selected test profile")
        run_contract = (
            '--label-exclude "$EXCLUDE_LABELS"',
            'if [[ "$executed" != "$EXPECTED_TESTS" ]]; then',
            'echo "CTest executed $executed tests; expected $EXPECTED_TESTS" >&2',
            "EXCLUDE_LABELS: ${{ steps.inventory.outputs.label_exclude }}",
            "EXPECTED_TESTS: ${{ steps.inventory.outputs.count }}",
            "TEST_PARALLEL: ${{ matrix.parallel }}",
        )
        for expected in run_contract:
            with self.subTest(step="run", expected=expected):
                self.assertIn(expected, run_step)

    def test_workflow_stops_each_failed_profile_but_keeps_other_hosts_running(self):
        source = WORKFLOW.read_text(encoding="utf-8")
        strategy = source.split("    strategy:\n", 1)[1].split(
            "\n    steps:", 1
        )[0]
        self.assertIn("      fail-fast: false\n", strategy)

        step_marker = "      - name: Run selected test profile\n"
        self.assertEqual(source.count(step_marker), 1)
        run_step = source.split(step_marker, 1)[1].split(
            "\n      - name:", 1
        )[0]
        self.assertIn("set -o pipefail", run_step)
        self.assertIn("--stop-on-failure", run_step)
        self.assertIn("--output-on-failure", run_step)

    # The corpus is several hundred pinned real binaries, and the only thing
    # that puts them under test is this flag.  Losing it costs nothing that any
    # test would notice, which is why the workflow's own text is checked.
    def test_workflow_configures_and_verifies_the_pinned_binary_corpus(self):
        source = WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("submodules: recursive", source)

        verify_marker = "      - name: Verify the pinned binary corpus\n"
        self.assertEqual(source.count(verify_marker), 1)
        verify_step = source.split(verify_marker, 1)[1].split(
            "\n      - name:", 1
        )[0]
        for line in ("windows-eh", "rust-eh", "go-eh", "cxx-itanium-eh", "objc-eh"):
            with self.subTest(line=line):
                self.assertIn(line, verify_step)
        self.assertIn("exit 1", verify_step)

        configure_marker = "      - name: Configure Release build\n"
        configure_step = source.split(configure_marker, 1)[1].split(
            "\n      - name:", 1
        )[0]
        self.assertIn(
            "-DNEVERD_ENABLE_BINARY_CORPUS_TESTS=ON", configure_step
        )
        # Placed before the configure that depends on it, so a checkout which
        # did not bring the submodule says so instead of failing in CMake.
        self.assertLess(
            source.index(verify_marker), source.index(configure_marker)
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
