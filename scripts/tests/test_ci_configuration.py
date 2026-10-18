import ast
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from unittest.mock import patch


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
STYLE_EXPECTATIONS = ROOT / "scripts" / "tests" / "CIStyleExpectations.def"


class CiConfigurationTests(unittest.TestCase):
    def test_msvc_test_targets_select_extended_coff_objects(self):
        for msvc_style in (False, True):
            with self.subTest(msvc_style=msvc_style), tempfile.TemporaryDirectory(
                    prefix="neverd-test-object-format-") as directory:
                root = Path(directory)
                (root / "probe.cpp").write_text("int main() { return 0; }\n")
                # Exercise the real test helper and generated compile command.
                # The MSVC branch is configuration-only on non-Windows hosts;
                # the native CI compiler is still required to certify it.
                source = textwrap.dedent(f"""\
                    cmake_minimum_required(VERSION 3.20)
                    project(TestObjectFormat LANGUAGES CXX)
                    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
                    add_library(GTest::gtest INTERFACE IMPORTED)
                    add_library(GTest::gtest_main INTERFACE IMPORTED)
                    include("{CMAKE_HELPERS.as_posix()}")
                    set(MSVC {"TRUE" if msvc_style else "FALSE"})
                    add_neverd_unittest(OwnedFixture probe.cpp)
                    """)
                (root / "CMakeLists.txt").write_text(source)
                build = root / "build"
                result = subprocess.run(
                    ["cmake", "-S", str(root), "-B", str(build), "-G", "Ninja"],
                    capture_output=True, text=True, check=False,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                commands = json.loads((build / "compile_commands.json").read_text())
                self.assertEqual(len(commands), 1)
                command = commands[0]
                tokens = command.get("arguments") or re.findall(
                    r'"[^\"]*"|\S+', command["command"])
                flags = {token.strip('"').casefold() for token in tokens}
                self.assertEqual("/bigobj" in flags, msvc_style)

    def test_windows_serial_policy_runs_in_ctest_without_project_policies(self):
        execution = ROOT / "unittests" / "emulation" / "WindowsTestExecution.cmake"
        inventory = execution.with_suffix(".def").read_text(encoding="utf-8")
        rule = re.search(
            r'NEVERD_WINDOWS_TEST_SERIAL\(\s*"([^"]+)",\s*"([^"]+)",'
            r'\s*"([^"]+)",\s*"([^"]+)"\s*\)', inventory,
        )
        self.assertIsNotNone(rule)
        instantiation, suite, case, backend = rule.groups()
        selected = f"{instantiation}/{suite}.{case}/{backend}"
        neighbor = f"{instantiation}/{suite}.{case}/OtherBackend"
        annotated = selected + f"  # GetParam() = {backend}"
        other_annotated = neighbor + "  # GetParam() = OtherBackend"
        for names, serial in (
                ((selected, neighbor), {selected}),
                ((annotated, other_annotated), {annotated}),
                ((selected + "Extra", neighbor), set()),
                ((neighbor,), set()), ((), set())):
            with self.subTest(names=names), tempfile.TemporaryDirectory(
                    prefix="neverd-ctest-windows-policy-") as directory:
                root = Path(directory)
                # CTest evaluates TEST_INCLUDE_FILES without the project's
                # cmake_minimum_required policy scope. No guest runs here.
                source = ""
                for name in names:
                    source += (
                        f'add_test("{name}" "{Path(sys.executable).as_posix()}")\n'
                    )
                    source += (
                        f'set_tests_properties("{name}" PROPERTIES TIMEOUT 17)\n'
                    )
                source += "set(NeverDWindowsProcessTests_TESTS "
                source += " ".join(f'"{name}"' for name in names) + ")\n"
                source += f'include("{execution.as_posix()}")\n'
                (root / "CTestTestfile.cmake").write_text(source, encoding="utf-8")
                result = subprocess.run(
                    ["ctest", "--test-dir", str(root), "--show-only=json-v1"],
                    capture_output=True, text=True, timeout=30,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertNotIn("CMP0057", result.stderr)
                tests = json.loads(result.stdout)["tests"]
                self.assertEqual({test["name"] for test in tests}, set(names))
                for test in tests:
                    properties = {p["name"]: p["value"] for p in test["properties"]}
                    self.assertEqual(properties["TIMEOUT"], 17)
                    self.assertEqual(properties.get("RUN_SERIAL", False),
                                     test["name"] in serial)

    def test_driver_batch_timeouts_preserve_exact_method_and_backend_boundaries(self):
        execution = ROOT / "unittests" / "emulation" / "DriverBatchTestExecution.cmake"
        methods = (
            "Backends/KernelFileObjectExecution.OriginalBufferedNativeFileTransfers",
            "Backends/KernelIntegerFormattingExecution.OriginalImportsAndLookupPreserveAllValueBits",
        )
        variants = ("whpLegacyPreferred", "whpLegacyRebased",
                    "whpCheckedPreferred", "whpCheckedRebased")
        selected = {f"{method}/{variant}" for method in methods for variant in variants}
        neighbors = {
            f"{method}/{backend}LegacyPreferred"
            for method in methods for backend in ("unicorn", "kvm", "hvf")
        } | {name + "Extra" for name in selected} | {
            "Other/KernelFileObjectExecution.OriginalBufferedNativeFileTransfers/whpLegacyPreferred",
            "Backends/KernelFileObjectExecution.OriginalDriverQueriesBufferedFileInformation/whpLegacyPreferred",
        }
        for annotated in (False, True):
            with self.subTest(annotated=annotated), tempfile.TemporaryDirectory(
                    prefix="neverd-driver-batch-policy-") as directory:
                root = Path(directory)
                original = sorted(selected | neighbors)
                names = [name + ("  # GetParam() = bytes" if annotated else "")
                         for name in original]
                source = ""
                for name in names:
                    source += f'add_test("{name}" "{Path(sys.executable).as_posix()}")\n'
                    source += f'set_tests_properties("{name}" PROPERTIES TIMEOUT 30)\n'
                source += "set(NeverDNativeDriverTests_TESTS "
                source += " ".join(f'"{name}"' for name in names) + ")\n"
                source += f'include("{execution.as_posix()}")\n'
                (root / "CTestTestfile.cmake").write_text(source, encoding="utf-8")
                result = subprocess.run(
                    ["ctest", "--test-dir", str(root), "--show-only=json-v1"],
                    capture_output=True, text=True, timeout=30,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertNotIn("CMP0057", result.stderr)
                tests = json.loads(result.stdout)["tests"]
                self.assertEqual({test["name"] for test in tests}, set(names))
                for test in tests:
                    name = test["name"].split("  # GetParam() = ", 1)[0]
                    props = {p["name"]: p["value"] for p in test["properties"]}
                    if name in selected:
                        guest_allowance = 18 * 5 if "FileObject" in name else 4 * 30
                        self.assertGreater(props["TIMEOUT"], guest_allowance)
                        self.assertLessEqual(props["TIMEOUT"], 180)
                    else:
                        self.assertEqual(props["TIMEOUT"], 30)
                    self.assertFalse(props.get("RUN_SERIAL", False))

    def test_darwin_batch_timeouts_cover_guests_without_changing_neighbor_cases(self):
        execution = ROOT / "unittests" / "emulation" / "DarwinTestExecution.cmake"
        guest_counts = {
            "CreatedDirectoryFileSystemsKeepActualParentAndHeldLifetime": 8,
            "CreatedRegularAttributesKeepIndependentProvidersAndObjectState": 20,
        }
        profiles = ("MacOSARM64_hvf", "IOSARM64_hvf", "SimulatorARM64_hvf",
                    "MacOSARM64_unicorn", "MacOSX64_whp", "MacOSX64_kvm")
        selected = {
            f"Transports/DarwinProcess.{method}/{profile}": guests * 20
            for method, guests in guest_counts.items() for profile in profiles
        }
        neighbors = {name.replace("/MacOSARM64_hvf", "Extra/MacOSARM64_hvf")
                     for name in selected if name.endswith("/MacOSARM64_hvf")} | {
            "Other/DarwinProcess.CreatedDirectoryFileSystemsKeepActualParentAndHeldLifetime/MacOSARM64_hvf",
            "Transports/DarwinProcess.CreatedRegularSynchronizationKeepsIndependentProvidersAndState/MacOSARM64_hvf",
        }
        previous = {
            "Transports/DarwinProcess.ImmediateSelectPreservesBitmapsCopiesAndProviderPremises/MacOSARM64_hvf": 120,
            "Transports/DarwinProcess.SymbolicFileSystemsKeepOwnSnapshotAndFollowedTargetSeparate/MacOSARM64_hvf": 240,
        }
        with tempfile.TemporaryDirectory(prefix="neverd-darwin-batch-policy-") as directory:
            root = Path(directory)
            names = sorted(set(selected) | neighbors | set(previous))
            source = ""
            for name in names:
                source += f'add_test("{name}" "{Path(sys.executable).as_posix()}")\n'
                source += f'set_tests_properties("{name}" PROPERTIES TIMEOUT 30)\n'
            source += "set(NeverDDarwinProcessTests_TESTS "
            source += " ".join(f'"{name}"' for name in names) + ")\n"
            source += f'include("{execution.as_posix()}")\n'
            (root / "CTestTestfile.cmake").write_text(source, encoding="utf-8")
            result = subprocess.run(
                ["ctest", "--test-dir", str(root), "--show-only=json-v1"],
                capture_output=True, text=True, timeout=30,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            tests = json.loads(result.stdout)["tests"]
            self.assertEqual({test["name"] for test in tests}, set(names))
            for test in tests:
                name = test["name"]
                props = {p["name"]: p["value"] for p in test["properties"]}
                if name in selected:
                    self.assertGreater(props["TIMEOUT"], selected[name])
                    self.assertLessEqual(props["TIMEOUT"], selected[name] + 20)
                else:
                    self.assertEqual(props["TIMEOUT"], previous.get(name, 30))
                self.assertFalse(props.get("RUN_SERIAL", False))

    def test_mobile_focused_inventory_executes_on_every_runner(self):
        source = WORKFLOW.read_text(encoding="utf-8")
        step = self.workflow_step_containing(source, "focused contract has missing")
        script = step.split("<<'PY'\n", 1)[1]
        # Exercise the actual declarative inventory and its identity checks.
        # Stop before the first filesystem mutation or CTest invocation.
        inventory = textwrap.dedent(script.split('          evidence = Path(', 1)[0])
        for runner, profile in (("Linux", "linux-semantic"),
                                ("macOS", "macos-patch"),
                                ("Windows", "windows-focused")):
            with self.subTest(runner=runner), patch.dict(
                    "os.environ", {"RUNNER_OS": runner, "TEST_PROFILE": profile}):
                scope = {}
                exec(compile(inventory, str(WORKFLOW), "exec"), scope)
                self.assertEqual(len(scope["expected"]), scope["expected_count"])

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

        declarations = STYLE_EXPECTATIONS.read_text(encoding="utf-8")
        pattern = r'NEVERD_CI_STYLE_EXPECTATION\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\)'
        expectations = tuple(
            ast.literal_eval("(" + literal + ")")
            for literal in re.findall(pattern, declarations, re.DOTALL)
        )
        self.assertTrue(expectations)
        self.assertEqual(
            len(expectations), declarations.count("NEVERD_CI_STYLE_EXPECTATION(")
        )
        for expected in expectations:
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
        self.assertEqual(verify_step.count("scripts.tests.test_ci_test_reuse"), 1)
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
            source.split("  build-and-test:\n", 1)[1]
            .split("  native-cpu:\n", 1)[0]
            .count("-DNEVERD_LLVM_PREBUILT="),
            1,
            "the compiler cache must not introduce a prebuilt-LLVM fast path",
        )

    def test_native_cpu_dispatch_keeps_default_ci_and_requires_selected_native_evidence(self):
        source = WORKFLOW.read_text(encoding="utf-8")
        native = source.split("  native-cpu:\n", 1)[1].split(
            "  build-and-test:\n", 1
        )[0]
        self.assertIn(
            "github.event_name == 'workflow_dispatch' && inputs.native_cpu_only",
            native,
        )
        self.assertIn("inputs.native_cpu_backend == 'kvm' && 'ubuntu-24.04' || 'windows-latest'", native)
        self.assertIn("SCCACHE_GHA_ENABLED: 'true'", native)
        self.assertIn("mozilla-actions/sccache-action@", native)
        self.assertIn("-DCMAKE_C_COMPILER_LAUNCHER=sccache", native)
        self.assertIn("-DCMAKE_CXX_COMPILER_LAUNCHER=sccache", native)
        self.assertIn("-DNEVERD_EMULATION_BACKEND_UNICORN=OFF", native)
        self.assertIn("-DNEVERD_ENABLE_SEMANTIC_TESTS=OFF", native)
        self.assertIn("scripts/run_native_cpu_ci.py", native)
        self.assertIn("NEVERD_NATIVE_CPU_BACKEND: ${{ inputs.native_cpu_backend }}", native)
        self.assertIn('--require-"$NEVERD_NATIVE_CPU_BACKEND"', native)
        self.assertIn("scripts/prepare_kvm_ci.py", native)
        self.assertIn("build-ci-native/native-evidence/device-access.json", native)
        self.assertLess(native.index("scripts/prepare_kvm_ci.py"),
                        native.index("scripts/run_native_cpu_ci.py"))
        self.assertIn("inputs.native_cpu_backend == 'kvm' && 'clang' || 'cl'", native)
        self.assertIn("inputs.native_cpu_backend == 'kvm' && 'clang++' || 'cl'", native)
        linux_tools = native.split("      - name: Install Linux native CPU build tooling\n", 1)[1].split(
            "      - name: Enable the native CPU MSVC toolchain\n", 1)[0]
        self.assertIn("runner.os == 'Linux'", linux_tools)
        self.assertIn("signed-by=/usr/share/keyrings/neverd-llvm.asc", linux_tools)
        self.assertIn("clang-21 lld-21", linux_tools)
        self.assertIn('/usr/lib/llvm-21/bin >> "$GITHUB_PATH"', linux_tools)
        self.assertIn("inputs.native_driver_tests", native)
        self.assertIn("driver_args+=(--with-drivers)", native)
        self.assertIn("scripts/build_wdk_driver_fixtures.py", native)
        self.assertIn("-C build-ci-native/driver-fixtures/fixtures.cmake", native)
        self.assertLess(native.index("scripts/build_wdk_driver_fixtures.py"),
                        native.index("cmake -S . -B build-ci-native"))
        self.assertIn("build-ci-native/driver-fixtures/build-manifest.json", native)
        self.assertNotIn("build-ci-native/driver-packages/", native)
        self.assertNotIn("build-ci-native/driver-fixtures/kit/", native)
        self.assertIn(
            '-DNEVERD_ENABLE_DRIVER_EMULATION="$NEVERD_NATIVE_DRIVER_TESTS"',
            native,
        )
        self.assertIn("native-evidence/", native)
        full = source.split("  build-and-test:\n", 1)[1].split(
            "  native-cpu:\n", 1
        )[0]
        self.assertIn("!inputs.native_cpu_only", full)
        self.assertNotIn("-DNEVERD_ENABLE_SEMANTIC_TESTS=OFF", full)
        dispatch = source.split("      native_cpu_only:\n", 1)[1].split(
            "\npermissions:", 1
        )[0]
        self.assertIn("default: false", dispatch)
        transport = dispatch.split("      native_cpu_backend:\n", 1)[1].split(
            "      native_driver_tests:\n", 1)[0]
        self.assertIn("type: choice", transport)
        self.assertIn("default: whp", transport)
        self.assertIn("- whp\n", transport)
        self.assertIn("- kvm\n", transport)

    @unittest.skipUnless(shutil.which("bash"), "Bash is required to execute the CI step")
    def test_native_cache_startup_falls_back_without_hiding_configure_failures(self):
        source = WORKFLOW.read_text(encoding="utf-8")
        step = self.workflow_step_containing(
            source, "Check native compiler cache availability"
        )
        self.assertLess(source.index("Configure the native CPU profile"),
                        source.index("Check native compiler cache availability"))
        self.assertLess(source.index("Check native compiler cache availability"),
                        source.index("Build and verify native CPU owners"))
        self.assertNotIn("continue-on-error", step)
        script = textwrap.dedent(step.split("        run: |\n", 1)[1])
        commands = r'''
            sccache() {
              printf '%s\n' "$@" > "$RUNNER_TEMP/cache.args"
              printf '%s\n' 'startup failed: https://cache.invalid/?cache-token=secret'
              return "$CACHE_STATUS"
            }
            cmake() {
              printf '%s\n' "$@" > "$RUNNER_TEMP/cmake.args"
              return "$CMAKE_STATUS"
            }
        '''
        for cache_status, cmake_status in ((0, 0), (1, 0), (1, 69)):
            with (
                self.subTest(cache=cache_status, cmake=cmake_status),
                tempfile.TemporaryDirectory(prefix="neverd-cache-probe-") as directory,
            ):
                root = Path(directory)
                result = subprocess.run(
                    [shutil.which("bash"), "-e", "-o", "pipefail", "-c",
                     textwrap.dedent(commands) + script],
                    env={**os.environ, "RUNNER_TEMP": root.as_posix(),
                         "CACHE_STATUS": str(cache_status),
                         "CMAKE_STATUS": str(cmake_status)},
                    capture_output=True,
                    text=True,
                    timeout=15,
                )
                self.assertEqual(result.returncode, cmake_status,
                                 result.stdout + result.stderr)
                self.assertEqual((root / "cache.args").read_text().splitlines(),
                                 ["--zero-stats"])
                self.assertNotIn("cache-token", result.stdout + result.stderr)
                self.assertIn("cache-token", (root / "neverd-sccache-probe.log").read_text())
                if cache_status:
                    self.assertIn("::warning::", result.stdout)
                    self.assertEqual((root / "cmake.args").read_text().splitlines(),
                                     ["-S", ".", "-B", "build-ci-native",
                                      "-DCMAKE_C_COMPILER_LAUNCHER=",
                                      "-DCMAKE_CXX_COMPILER_LAUNCHER="])
                else:
                    self.assertFalse((root / "cmake.args").exists())

    def test_native_host_setup_keeps_four_hosts_separate_from_fault_diagnostics(self):
        host = WORKFLOW.with_name("native-host-probe.yml").read_text(encoding="utf-8")
        self.assertEqual(host.count("          - backend:"), 4)
        for backend, arch, runner in (
            ("kvm", "X64", "ubuntu-24.04"),
            ("kvm", "ARM64", "ubuntu-24.04-arm"),
            ("whp", "X64", "windows-latest"),
            ("whp", "ARM64", "windows-11-arm"),
        ):
            with self.subTest(backend=backend, arch=arch):
                self.assertIn(
                    f"          - backend: {backend}\n"
                    f"            arch: {arch}\n"
                    f"            runner: {runner}\n", host)
        self.assertIn("fail-fast: false", host)
        self.assertIn("  pull_request:\n", host)
        self.assertIn("    branches: [dev]\n", host)
        self.assertIn("--backend ${{ matrix.backend }} --arch ${{ matrix.arch }}", host)
        self.assertIn("name: native-host-${{ matrix.backend }}-${{ matrix.arch }}", host)
        self.assertIn("build-native-host-probe/summary.json", host)
        self.assertLess(host.index("scripts/prepare_kvm_ci.py --output"),
                        host.index("python scripts/probe_native_host.py"))
        self.assertNotIn("--require-setup", host)
        self.assertNotIn("Temporary", host)

        diagnostic = WORKFLOW.with_name("native-fault-observations.yml").read_text(
            encoding="utf-8")
        self.assertIn("  workflow_dispatch:\n", diagnostic)
        self.assertNotIn("  pull_request:\n", diagnostic)
        self.assertNotIn("  push:\n", diagnostic)
        self.assertIn("scripts/diagnostics/RepFaultProbe.cpp", diagnostic)
        self.assertIn("WindowsAlignmentNative.RunsOriginalFaultAndRetryExecutable", diagnostic)
