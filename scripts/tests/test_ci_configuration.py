import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CMAKE_HELPERS = ROOT / "cmake" / "AddNeverD.cmake"
CLANG_FORMAT_CONFIG = ROOT / ".clang-format"
EXAMPLE_PLUGIN_CMAKE = ROOT / "plugins" / "example" / "CMakeLists.txt"
EXAMPLE_PLUGIN_SOURCE = ROOT / "plugins" / "example" / "example_plugin.c"
SDK_CMAKE = ROOT / "lib" / "sdk" / "CMakeLists.txt"
SEMANTIC_CMAKE = ROOT / "unittests" / "semantic" / "CMakeLists.txt"
SEMANTIC_PLUGIN_CMAKE = ROOT / "plugins" / "semantic" / "CMakeLists.txt"
WORKFLOW = ROOT / ".github" / "workflows" / "ci.yml"
STYLE_WORKFLOW = ROOT / ".github" / "workflows" / "llvm-style.yml"


class CiConfigurationTests(unittest.TestCase):
    def test_med_ir_exports_source_abi_and_objc_dependencies(self):
        with tempfile.TemporaryDirectory(prefix="neverd-med-ir-link-") as directory:
            root = Path(directory)
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.20)\n'
                'project(MedIRDependencies LANGUAGES CXX)\n'
                'foreach(component capstone_static NeverDIR NeverDLoader)\n'
                '  add_library(${component} INTERFACE)\n'
                'endforeach()\n'
                f'include("{CMAKE_HELPERS.as_posix()}")\n'
                f'add_subdirectory("{(ROOT / "lib/ir/med").as_posix()}" med)\n'
                'get_target_property(dependencies NeverDIRMed INTERFACE_LINK_LIBRARIES)\n'
                'foreach(required NeverDIR NeverDLoader)\n'
                '  if(NOT required IN_LIST dependencies)\n'
                '    message(SEND_ERROR "IRMed must export ${required}")\n'
                '  endif()\n'
                'endforeach()\n',
                encoding="utf-8",
            )
            result = subprocess.run(
                ["cmake", "-S", str(root), "-B", str(root / "build")],
                capture_output=True,
                text=True,
                timeout=120,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_loader_exports_its_source_abi_component_dependency(self):
        with tempfile.TemporaryDirectory(prefix="neverd-loader-link-") as directory:
            root = Path(directory)
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.20)\n'
                'project(LoaderDependency LANGUAGES CXX)\n'
                'foreach(component capstone_static NeverDIR NeverDEVM NeverDSBF)\n'
                '  add_library(${component} INTERFACE)\n'
                'endforeach()\n'
                f'include("{CMAKE_HELPERS.as_posix()}")\n'
                f'add_subdirectory("{(ROOT / "lib/loader").as_posix()}" loader)\n'
                'get_target_property(dependencies NeverDLoader INTERFACE_LINK_LIBRARIES)\n'
                'if(NOT "NeverDIR" IN_LIST dependencies)\n'
                '  message(FATAL_ERROR "Loader must export its SourceABI dependency")\n'
                'endif()\n',
                encoding="utf-8",
            )
            result = subprocess.run(
                ["cmake", "-S", str(root), "-B", str(root / "build")],
                capture_output=True,
                text=True,
                timeout=120,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def workflow_step_containing(self, source: str, needle: str) -> str:
        command_index = source.index(needle)
        step_start = source.rfind("      - name:", 0, command_index)
        self.assertNotEqual(step_start, -1, f"no workflow step owns {needle!r}")
        step_end = source.find("\n      - name:", command_index)
        if step_end == -1:
            step_end = len(source)
        return source[step_start:step_end]

    def test_low_ir_exports_decoder_and_libc_dependencies(self):
        with tempfile.TemporaryDirectory(prefix="neverd-low-ir-link-") as directory:
            root = Path(directory)
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.20)\n'
                'project(LowIRDependencies LANGUAGES C CXX)\n'
                'foreach(component capstone_static NeverDDecode NeverDLibC '
                'NeverDSolver NeverDSymbolic)\n'
                '  add_library(${component} INTERFACE)\n'
                'endforeach()\n'
                f'include("{CMAKE_HELPERS.as_posix()}")\n'
                f'add_subdirectory("{(ROOT / "lib/ir/low").as_posix()}" low)\n'
                'get_target_property(dependencies NeverDIRLow INTERFACE_LINK_LIBRARIES)\n'
                'foreach(required NeverDDecode NeverDLibC)\n'
                '  if(NOT required IN_LIST dependencies)\n'
                '    message(SEND_ERROR "IRLow must export ${required}")\n'
                '  endif()\n'
                'endforeach()\n',
                encoding="utf-8",
            )
            result = subprocess.run(
                ["cmake", "-S", str(root), "-B", str(root / "build")],
                capture_output=True,
                text=True,
                timeout=120,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_llvm_style_workflow_pins_formatter_and_checks_event_diff(self):
        source = STYLE_WORKFLOW.read_text(encoding="utf-8")

        for expected in (
            "name: LLVM Style",
            "    branches:\n      - dev",
            "  pull_request:",
            "  workflow_dispatch:",
            "  contents: read",
            "runs-on: ubuntu-24.04",
            "actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1",
            "fetch-depth: 0",
            "actions/setup-python@5fda3b95a4ea91299a34e894583c3862153e4b97",
            "python-version: '3.12'",
            "timeout-minutes: 5",
            "clang-format==22.1.2",
            "python -m unittest scripts.tests.test_check_clang_format -v",
            "FORMAT_BASE: ${{ github.event.pull_request.base.sha || github.event.before }}",
            "FORMAT_HEAD: ${{ github.sha }}",
            'python scripts/check_clang_format.py --base "$FORMAT_BASE" --head "$FORMAT_HEAD"',
        ):
            with self.subTest(expected=expected):
                self.assertIn(expected, source)

        self.assertNotIn("submodules:", source)
        self.assertNotIn("cmake --build", source)
        checkout_step = self.workflow_step_containing(source, "actions/checkout@")
        self.assertIn("persist-credentials: false", checkout_step)

    def test_clang_format_configuration_is_based_on_llvm_style(self):
        source = CLANG_FORMAT_CONFIG.read_text(encoding="utf-8")

        self.assertEqual(source.count("BasedOnStyle: LLVM\n"), 1)
        self.assertNotIn("DisableFormat: true", source)

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

    def test_computed_goto_structure_gate_is_small_and_never_profile_excluded(self):
        semantic = SEMANTIC_CMAKE.read_text(encoding="utf-8")
        workflow = WORKFLOW.read_text(encoding="utf-8")

        marker = "add_neverd_unittest(NeverDComputedGotoTests\n"
        self.assertEqual(semantic.count(marker), 1)
        invocation = semantic.split(marker, 1)[1].split("\n)", 1)[0]
        self.assertEqual(
            invocation.count(
                "  allplatform/controlflow/AllPlatform_ComputedGotoRTTests.cpp\n"
            ),
            1,
        )
        self.assertIn("NEVERD_COMPUTED_GOTO_STRUCTURAL_ONLY=1", semantic)
        self.assertNotIn(
            "ComputedGoto",
            "\n".join(
                line for line in workflow.splitlines() if "exclude_labels:" in line
            ),
        )

    def test_workflow_executes_this_configuration_contract(self):
        source = WORKFLOW.read_text(encoding="utf-8")
        step_marker = "      - name: Verify Debug and Release target flags\n"
        self.assertEqual(source.count(step_marker), 1)
        verify_step = source.split(step_marker, 1)[1].split("\n      - name:", 1)[0]
        self.assertEqual(verify_step.count("scripts.tests.test_ci_configuration"), 1)
        self.assertEqual(
            verify_step.count("scripts.tests.test_audit_ci_test_inventory"), 1
        )
        self.assertEqual(
            verify_step.count("scripts.tests.test_audit_ci_test_results"), 1
        )
        self.assertEqual(
            verify_step.count("scripts.tests.test_neverd_bench_harness"), 1
        )

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
            '--output-junit "$PWD/build-ci/ctest-results.xml"',
            '--output-log "$PWD/ctest.log"',
            'ctest_status="$?"',
            'printf \'%s\\n\' "$ctest_status" > build-ci/ctest-exit-status.txt',
            'exit "$ctest_status"',
            "EXCLUDE_LABELS: ${{ steps.inventory.outputs.label_exclude }}",
            "TEST_PARALLEL: ${{ matrix.parallel }}",
        )
        for expected in run_contract:
            with self.subTest(step="run", expected=expected):
                self.assertIn(expected, run_step)

        self.assertNotIn("EXPECTED_TESTS", run_step)
        self.assertNotIn("sed -nE", run_step)
        self.assertIn("cmake -E rm -f", run_step)
        self.assertLess(run_step.index("cmake -E rm -f"), run_step.index("ctest --test-dir"))
        for stale_file in ("ctest-results.xml", "ctest-exit-status.txt", "ctest-outcomes.json"):
            self.assertIn(stale_file, run_step.split("set +e", 1)[0])

        outcome_step = named_step("Audit selected test outcomes")
        self.assertIn("always() && steps.inventory.outcome == 'success'", outcome_step)
        for expected in (
            '"$PYTHON" scripts/audit_ci_test_results.py audit',
            "--inventory build-ci/ctest-inventory.json",
            "--junit build-ci/ctest-results.xml",
            "--ctest-status build-ci/ctest-exit-status.txt",
            "--output build-ci/ctest-outcomes.json",
            '--profile "$TEST_PROFILE"',
            '--exclude-label-regex "$EXCLUDE_LABELS"',
        ):
            self.assertIn(expected, outcome_step)

        tool_step = named_step("Verify CTest outcome tooling")
        self.assertIn('"$PYTHON" scripts/audit_ci_test_results.py check-tool', tool_step)
        self.assertLess(source.index("Verify CTest outcome tooling"), source.index("Verify Debug and Release target flags"))

        artifact_step = named_step("Upload CTest execution evidence")
        self.assertIn("always() && steps.inventory.outcome == 'success'", artifact_step)
        for expected in ("ctest-inventory.json", "ctest-results.xml", "ctest-exit-status.txt", "ctest-outcomes.json", "ctest.log"):
            self.assertIn(expected, artifact_step)
        self.assertNotIn("continue-on-error", run_step)

    def test_workflow_stops_each_failed_profile_but_keeps_other_hosts_running(self):
        source = WORKFLOW.read_text(encoding="utf-8")
        strategy = source.split("    strategy:\n", 1)[1].split("\n    steps:", 1)[0]
        self.assertIn("      fail-fast: false\n", strategy)

        step_marker = "      - name: Run selected test profile\n"
        self.assertEqual(source.count(step_marker), 1)
        run_step = source.split(step_marker, 1)[1].split("\n      - name:", 1)[0]
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
        verify_step = source.split(verify_marker, 1)[1].split("\n      - name:", 1)[0]
        for line in ("windows-eh", "rust-eh", "go-eh", "cxx-itanium-eh", "objc-eh"):
            with self.subTest(line=line):
                self.assertIn(line, verify_step)
        self.assertIn("exit 1", verify_step)

        configure_marker = "      - name: Configure Release build\n"
        configure_step = source.split(configure_marker, 1)[1].split(
            "\n      - name:", 1
        )[0]
        self.assertIn("-DNEVERD_ENABLE_BINARY_CORPUS_TESTS=ON", configure_step)
        # Placed before the configure that depends on it, so a checkout which
        # did not bring the submodule says so instead of failing in CMake.
        self.assertLess(source.index(verify_marker), source.index(configure_marker))

    def test_workflow_configures_and_explicitly_smokes_the_native_example_plugin(self):
        source = WORKFLOW.read_text(encoding="utf-8")
        configure_step = self.workflow_step_containing(
            source, "cmake -S . -B build-ci -G Ninja"
        )
        self.assertIn("-DNEVERD_BUILD_PLUGINS=ON", configure_step)

        plugin_step = self.workflow_step_containing(
            source, "build-ci/bin/neverd plugins"
        )
        required_contract = (
            "plugins --list --json",
            "json.load(sys.stdin)",
            'plugin.get("name") == "Example Plugin"',
            'plugin.get("kind") == "native"',
            'plugins --run "Example Plugin"',
        )
        for expected in required_contract:
            with self.subTest(expected=expected):
                self.assertIn(expected, plugin_step)

    def test_workflow_does_not_rescan_the_automatic_native_plugin_directory(self):
        source = WORKFLOW.read_text(encoding="utf-8")
        plugin_step = self.workflow_step_containing(
            source, "build-ci/bin/neverd plugins"
        )
        self.assertNotIn("--plugin-dir build-ci/bin/plugins", plugin_step)

    def test_example_plugins_share_the_cross_generator_output_contract(self):
        helper_source = CMAKE_HELPERS.read_text(encoding="utf-8")
        helper_contract = (
            "function(set_neverd_plugin_output_directories target)",
            "if(CMAKE_CONFIGURATION_TYPES)",
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/plugins",
            "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/$<CONFIG>/plugins",
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins",
            "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/plugins",
            'LIBRARY_OUTPUT_DIRECTORY "${_library_dir}"',
            'RUNTIME_OUTPUT_DIRECTORY "${_runtime_dir}"',
        )
        for expected in helper_contract:
            with self.subTest(expected=expected):
                self.assertIn(expected, helper_source)

        for target, path in (
            ("example_plugin", EXAMPLE_PLUGIN_CMAKE),
            ("semantic_plugin", SEMANTIC_PLUGIN_CMAKE),
        ):
            source = path.read_text(encoding="utf-8")
            with self.subTest(target=target):
                self.assertEqual(
                    source.count(f"set_neverd_plugin_output_directories({target})"),
                    1,
                )
                self.assertNotIn("LIBRARY_OUTPUT_DIRECTORY", source)
                self.assertNotIn("RUNTIME_OUTPUT_DIRECTORY", source)

    def test_shared_plugin_runtime_links_the_platform_dynamic_loader(self):
        source = SDK_CMAKE.read_text(encoding="utf-8")
        marker = "target_link_libraries(neverd_shared PRIVATE\n"
        self.assertIn(marker, source)
        link_contract = source.split(marker, 1)[1].split(")\n", 1)[0]
        self.assertIn("${CMAKE_DL_LIBS}", link_contract)

    def test_native_example_documents_and_registers_its_event_handler(self):
        source = EXAMPLE_PLUGIN_SOURCE.read_text(encoding="utf-8")
        for expected in (
            "neverd_plugins_dispatch_event()",
            "pointers in its payload are borrowed",
            "NEVERD_EVT_BINARY_LOADED",
            "Evt->Data.BinaryLoaded.Path",
            ".Event = exampleEvent",
        ):
            with self.subTest(expected=expected):
                self.assertIn(expected, source)

    def test_workflow_still_builds_the_unfiltered_default_target(self):
        source = WORKFLOW.read_text(encoding="utf-8")
        step_marker = "      - name: Build default targets\n"
        command = 'cmake --build build-ci --config Release --parallel "$BUILD_PARALLEL"'
        self.assertEqual(source.count(step_marker), 1)
        self.assertEqual(source.count(command), 1)
        build_section = source.split(step_marker, 1)[1].split("\n      - name:", 1)[0]
        self.assertEqual(build_section.count(command), 1)
        self.assertNotIn("--target", build_section)

    def test_workflow_caches_but_still_source_builds_llvm(self):
        source = WORKFLOW.read_text(encoding="utf-8")

        self.assertIn("SCCACHE_GHA_ENABLED: 'true'", source)
        self.assertIn("mozilla-actions/sccache-action@", source)
        self.assertIn("-DCMAKE_C_COMPILER_LAUNCHER=sccache", source)
        self.assertIn("-DCMAKE_CXX_COMPILER_LAUNCHER=sccache", source)
        self.assertIn('-DNEVERD_LLVM_PREBUILT="$NEVERD_LLVM_PREBUILT_MODE"', source)
        self.assertEqual(
            source.count("-DNEVERD_LLVM_PREBUILT="),
            1,
            "the compiler cache must not introduce a prebuilt-LLVM fast path",
        )
