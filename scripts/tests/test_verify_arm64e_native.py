"""Reject false native qualification from skipped or mismatched evidence."""

import json
import plistlib
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import verify_arm64e_native as native

CASES = json.loads(Path(__file__).with_suffix(".def").read_text())


def runtime_xml(executable=False, generated=False):
    key = "expected_" + ("generated_" if generated else "") + ("executable_" if executable else "") + "tests"
    root = ET.Element(CASES["suite_tag"])
    for name in CASES[key]:
        ET.SubElement(root, CASES["case_tag"], name=name,
                      status=CASES["run_status"])
    return root


class Arm64eNativeQualificationTests(unittest.TestCase):
    def test_cursor_requires_pc_sensitivity_for_explicit_instructions(self):
        observation = dict(CASES["cursor_observation"])
        def result():
            return subprocess.CompletedProcess([], 0, CASES["cursor_started"] +
                                               "\n" + json.dumps(observation) + "\n", "")
        self.assertEqual(native.cursor_observation(result(), CASES["cursor_recipes"][0])["status"],
                         native.POLICY["passed"])
        with self.assertRaises(ValueError):
            native.cursor_observation(result(), CASES["cursor_recipes"][1])
        observation["pc_rejected"] = observation["signed"]
        with self.assertRaises(ValueError):
            native.cursor_observation(result(), CASES["cursor_recipes"][1])
        observation["recipe_rejected"] = observation["signed"]
        self.assertEqual(native.cursor_observation(result(), CASES["cursor_recipes"][1])["status"],
                         native.POLICY["passed"])

    def test_cursor_does_not_accept_a_partial_or_unsigned_observation(self):
        for observation in CASES["cursor_invalid_observations"]:
            result = subprocess.CompletedProcess([], 0, CASES["cursor_started"] +
                                                 "\n" + json.dumps(observation) + "\n", "")
            with self.subTest(observation=observation), self.assertRaises(ValueError):
                native.cursor_observation(result, CASES["cursor_recipes"][0])

    def test_cursor_distinguishes_unavailable_isa_from_failed_checks(self):
        result = subprocess.CompletedProcess([], CASES["cursor_unavailable_code"],
                                             CASES["cursor_started"] + "\n", "")
        self.assertEqual(native.cursor_observation(result, CASES["cursor_recipes"][1])["status"],
                         native.POLICY["unavailable"])
        with self.assertRaises(ValueError):
            native.cursor_observation(result, CASES["cursor_recipes"][0])
        for code in CASES["cursor_invalid_return_codes"]:
            result.returncode = code
            with self.subTest(code=code), self.assertRaises(ValueError):
                native.cursor_observation(result, CASES["cursor_recipes"][1])

    def test_cpta_requires_checked_results_from_the_actual_x1_slice(self):
        policy = native.POLICY["cpta_probe"]
        checked = [row[3] for row in policy["vectors"]]
        unchecked = [row[2] for row in policy["vectors"]]
        for results, writeback, status in (
            (checked, policy["writeback_checked"], native.POLICY["passed"]),
            (unchecked, policy["writeback_unchecked"], native.POLICY["unavailable"]),
            (checked, policy["writeback_unchecked"], native.POLICY["unavailable"]),
            (checked, "", native.POLICY["failed"]),
            (checked[:-1], policy["writeback_checked"], native.POLICY["failed"]),
            (unchecked[:-1] + checked[-1:], policy["writeback_checked"], native.POLICY["failed"]),
        ):
            def run(products, output, label, argv, timeout, env=None):
                if label.endswith(native.POLICY["compile_suffix"]):
                    arch = label.removesuffix(native.POLICY["compile_suffix"])
                    Path(argv[-1]).write_bytes(struct.pack(
                        native.POLICY["header_format"], native.POLICY["macho_magic"],
                        native.POLICY["cpu_type"], policy["profiles"][arch]))
                if label == policy["read_label"]:
                    return subprocess.CompletedProcess(argv, 0, plistlib.dumps(policy["entitlements"]).decode(), "")
                if label.endswith(policy["writeback_suffix"]):
                    return subprocess.CompletedProcess(argv, 0, writeback, "")
                values = results if label == policy["x1"] else checked
                lines = [policy["started"], *values]
                return subprocess.CompletedProcess(argv, 0, "\n".join(lines), "")

            with self.subTest(results=results), tempfile.TemporaryDirectory() as temporary, \
                    mock.patch.object(native.platform, "system", return_value=CASES["probe_host"]["system"]), \
                    mock.patch.object(native.platform, "machine", return_value=CASES["probe_host"]["machine"]), \
                    mock.patch.object(native, "command", side_effect=run):
                report = native.probe_cpta(Path(temporary))
                self.assertEqual(report["status"], status)
                self.assertTrue(report[policy["observed_key"]])
                self.assertEqual(report[policy["qualified_key"]], status == native.POLICY["passed"])
                self.assertEqual(report[policy["writeback_qualified_key"]],
                                 results in (checked, unchecked) and writeback == policy["writeback_checked"])
                self.assertFalse(list(Path(temporary).glob("*" + native.POLICY["binary_suffix"])))

    def test_cpta_requires_the_signed_entitlements_to_round_trip(self):
        policy = native.POLICY["cpta_probe"]
        wrong = {k: int(v) if v is True else v for k, v in policy["entitlements"].items()}
        for contents in (plistlib.dumps({}).decode(), plistlib.dumps(wrong).decode(), ""):
            with self.subTest(contents=contents), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                with native.OwnedProducts(root, False) as products, mock.patch.object(
                    native, "command", return_value=subprocess.CompletedProcess([], 0, contents, "")
                ):
                    self.assertIsNone(native.sign_cpta_slice(products, root, root / policy["x1"], policy))
                self.assertFalse((root / policy["entitlements_file"]).exists())

    def test_cpta_records_unsupported_isa_and_loader_without_qualification(self):
        policy = native.POLICY["cpta_probe"]

        def run(products, output, label, argv, timeout, env=None):
            if label.endswith(native.POLICY["compile_suffix"]):
                arch = label.removesuffix(native.POLICY["compile_suffix"])
                Path(argv[-1]).write_bytes(struct.pack(
                    native.POLICY["header_format"], native.POLICY["macho_magic"],
                    native.POLICY["cpu_type"], policy["profiles"][arch]))
                return subprocess.CompletedProcess(argv, 0, "", "")
            if label in (policy["sign_label"], policy["x1"] + native.POLICY["signature_suffix"]):
                return subprocess.CompletedProcess(argv, 0, "", "")
            if label == policy["read_label"]:
                return subprocess.CompletedProcess(argv, 0, plistlib.dumps(policy["entitlements"]).decode(), "")
            if label.endswith(policy["writeback_suffix"]):
                return subprocess.CompletedProcess(argv, 0, policy["writeback_unchecked"], "")
            if label == policy["x1"]:
                raise OSError(policy["bad_cpu_type_errno"], "")
            return subprocess.CompletedProcess(argv, policy["undefined_instruction"],
                                               policy["started"], "")

        with tempfile.TemporaryDirectory() as temporary, \
                mock.patch.object(native.platform, "system", return_value=CASES["probe_host"]["system"]), \
                mock.patch.object(native.platform, "machine", return_value=CASES["probe_host"]["machine"]), \
                mock.patch.object(native, "command", side_effect=run):
            report = native.probe_cpta(Path(temporary))
            self.assertEqual(report["status"], native.POLICY["unavailable"])
            self.assertFalse(report[policy["observed_key"]])
            self.assertFalse(report[policy["qualified_key"]])
            for arch, profile in report["profiles"].items():
                self.assertEqual(profile["outcome"],
                                 policy["loader_unavailable" if arch == policy["x1"] else "isa_unavailable"])

    def test_probe_keeps_startup_and_dlopen_contracts_distinct(self):
        contract = native.fixture_contract()
        markers = "\n".join(contract[name] for name in
                            ("started", "completed", "runtime_completed"))
        def run(products, output, label, argv, timeout, env=None):
            stdout = CASES["syscall_macros"] if label == native.POLICY["syscall_label"] else markers
            return subprocess.CompletedProcess(argv, 0, stdout, "")

        for profile in CASES["probe_profiles"]:
            startup, userspace = profile["startup"], profile["userspace_fixups"]
            with self.subTest(profile=profile), tempfile.TemporaryDirectory() as temporary, \
                    mock.patch.object(native.platform, "system", return_value=CASES["probe_host"]["system"]), \
                    mock.patch.object(native.platform, "machine", return_value=CASES["probe_host"]["machine"]), \
                    mock.patch.object(native, "provenance", return_value=CASES["provenance"]), \
                    mock.patch.object(native, "check_binary", side_effect=lambda _: dict(CASES["probe_identity"])), \
                    mock.patch.object(native, "command", side_effect=run) as command:
                report = native.probe(Path(temporary), **profile)
                self.assertEqual(report["status"], native.POLICY["passed"])
                self.assertEqual(report["control_loading"], native.POLICY[
                    "startup_loading" if startup else "control_loading"])
                self.assertEqual(report[native.POLICY["startup_qualification_key"]], startup)
                self.assertEqual(report[native.POLICY["fixup_loader_key"]], native.POLICY[
                    "userspace_fixup_loader" if userspace else "system_fixup_loader"])
                self.assertFalse(report["whole_image_publication_qualified"])
                runtime_labels = {native.POLICY["runtime_label"].format(
                    optimization=optimization, dwarf=dwarf)
                    for optimization in native.POLICY["optimizations"]
                    for dwarf in native.POLICY["dwarf_modes"]}
                invocations = {call.args[2]: call.args[3] for call in command.call_args_list}
                prefix = []
                if userspace:
                    prefix = report[native.POLICY["startup_profile_key"]]["launcher"]
                    for optimization in native.POLICY["optimizations"]:
                        label = native.POLICY["capability_label"].format(optimization=optimization)
                        self.assertEqual(invocations[label][:-1], prefix)
                self.assertEqual(native.POLICY["syscall_label"] in invocations, userspace)
                for label in runtime_labels:
                    compile = invocations[label + native.POLICY["compile_suffix"]]
                    self.assertEqual(all(arg in compile for arg in contract["library_args"]),
                                     not startup)
                    self.assertEqual(len(invocations[label]), len(prefix) + (1 if startup else 2))
                    self.assertEqual(invocations[label][:len(prefix)], prefix)
                    self.assertEqual("loader" in next(row for row in report["controls"]
                                                     if row["label"] == label), not startup)

    def test_userspace_startup_requires_an_exact_successful_sdk_syscall_definition(self):
        for code, stdout in [(0, text) for text in CASES["invalid_syscall_macros"]] + [
                (CASES["syscall_failure"], CASES["syscall_macros"])]:
            def run(products, output, label, argv, timeout, env=None):
                if label == native.POLICY["syscall_label"]:
                    return subprocess.CompletedProcess(argv, code, stdout, "")
                return subprocess.CompletedProcess(argv, 0, "", "")

            with self.subTest(code=code, stdout=stdout), tempfile.TemporaryDirectory() as temporary, \
                    mock.patch.object(native.platform, "system", return_value=CASES["probe_host"]["system"]), \
                    mock.patch.object(native.platform, "machine", return_value=CASES["probe_host"]["machine"]), \
                    mock.patch.object(native, "provenance", return_value=CASES["provenance"]), \
                    mock.patch.object(native, "command", side_effect=run) as command:
                with self.assertRaises((ValueError, subprocess.CalledProcessError)):
                    native.probe(Path(temporary), startup=True, userspace_fixups=True)
                self.assertEqual(command.call_args.args[2], native.POLICY["syscall_label"])
                report = json.loads((Path(temporary) / native.POLICY["report_name"]).read_text())
                self.assertFalse(report["native_pac_qualified"])
                self.assertFalse(report[native.POLICY["startup_qualification_key"]])

    def test_failed_startup_does_not_retry_as_dlopen(self):
        contract = native.fixture_contract()
        markers = "\n".join(contract[name] for name in
                            ("started", "completed", "runtime_completed"))
        first_runtime = native.POLICY["runtime_label"].format(
            optimization=native.POLICY["optimizations"][0], dwarf=native.POLICY["dwarf_modes"][0])

        def run(products, output, label, argv, timeout):
            code = CASES["startup_failure"] if label == first_runtime else 0
            return subprocess.CompletedProcess(argv, code, markers, "")

        with tempfile.TemporaryDirectory() as temporary, \
                mock.patch.object(native.platform, "system", return_value=CASES["probe_host"]["system"]), \
                mock.patch.object(native.platform, "machine", return_value=CASES["probe_host"]["machine"]), \
                mock.patch.object(native, "provenance", return_value=CASES["provenance"]), \
                mock.patch.object(native, "check_binary", side_effect=lambda _: dict(CASES["probe_identity"])), \
                mock.patch.object(native, "command", side_effect=run) as command:
            report = native.probe(Path(temporary), startup=True)
            self.assertEqual(report["status"], native.POLICY["failed"])
            self.assertTrue(report["native_pac_qualified"])
            self.assertFalse(report["compiler_original_call_unwind_qualified"])
            self.assertFalse(report[native.POLICY["startup_qualification_key"]])
            self.assertFalse(report["whole_image_publication_qualified"])
            self.assertEqual(report["controls"][-1]["returncode"], CASES["startup_failure"])
            self.assertEqual(len(report["controls"]), len(native.POLICY["optimizations"]) + 1)
            for call in command.call_args_list:
                self.assertFalse(any(arg in call.args[3] for arg in contract["library_args"]))

    def test_failed_legacy_linker_lookup_stops_before_compiling(self):
        failure = subprocess.CompletedProcess([], CASES["legacy_lookup_failure"], "", "")
        with tempfile.TemporaryDirectory() as temporary, \
                mock.patch.object(native, "command", return_value=failure) as command:
            with self.assertRaises(RuntimeError):
                native.build_legacy_fixtures(Path(temporary))
            command.assert_called_once()

    def test_legacy_bundle_requires_current_sources_and_exact_image_bytes(self):
        with tempfile.TemporaryDirectory() as temporary, \
                mock.patch.object(native, "provenance", return_value=CASES["provenance"]):
            root = Path(temporary)
            report = {**CASES["provenance"], "status": native.POLICY["passed"], "images": []}
            for case in native.legacy_cases(native.fixture_contract()):
                path = root / case["name"]
                path.write_bytes(struct.pack(CASES["legacy_header_format"], *CASES["legacy_header"]))
                report["images"].append({**case, **native.check_binary(path)})
            manifest = root / native.POLICY["report_name"]
            native.save(manifest, report)
            self.assertEqual(native.verify_legacy_fixtures(root), report)
            for mutation in CASES["probe_mutations"]:
                if not any(key in mutation for key in native.POLICY["provenance_keys"]):
                    continue
                native.save(manifest, dict(report, **mutation))
                with self.assertRaises(ValueError):
                    native.verify_legacy_fixtures(root)
            native.save(manifest, report)
            path.write_bytes(path.read_bytes() + CASES["legacy_trailing_bytes"].encode())
            with self.assertRaises(ValueError):
                native.verify_legacy_fixtures(root)
            path.unlink()
            with self.assertRaises(OSError):
                native.verify_legacy_fixtures(root)

    def test_pac_requires_entry_completion_and_successful_exit(self):
        contract = native.fixture_contract()
        for case in CASES["capability_cases"]:
            with self.subTest(case=case["name"]):
                lines = [contract[name] for name in case["markers"]]
                result = subprocess.CompletedProcess([], case["returncode"],
                                                     "\n".join(lines) + "\n", "")
                self.assertEqual(native.capability_status(result, contract), case["status"])

    def test_complete_runtime_matrix_passes(self):
        root = runtime_xml()
        self.assertEqual(set(native.check_runtime_report(ET.tostring(root))),
                         set(CASES["expected_tests"]))

    def test_failed_skipped_and_unrun_cases_do_not_qualify(self):
        for tag in CASES["failure_tags"]:
            with self.subTest(tag=tag):
                root = runtime_xml()
                ET.SubElement(root[0], tag)
                with self.assertRaises(ValueError):
                    native.check_runtime_report(ET.tostring(root))
        for status in CASES["unrun_statuses"]:
            with self.subTest(status=status):
                root = runtime_xml()
                root[0].set(CASES["status_key"], status)
                with self.assertRaises(ValueError):
                    native.check_runtime_report(ET.tostring(root))

    def test_missing_duplicate_and_foreign_cases_do_not_qualify(self):
        root = runtime_xml()
        root.remove(root[-1])
        with self.assertRaises(ValueError):
            native.check_runtime_report(ET.tostring(root))
        for replacement in [CASES["expected_tests"][1], CASES["foreign_test"]]:
            root = runtime_xml()
            root[0].set(CASES["name_key"], replacement)
            with self.assertRaises(ValueError):
                native.check_runtime_report(ET.tostring(root))

    def test_ctest_parameter_annotations_preserve_exact_case_identity(self):
        root = runtime_xml()
        for case in root:
            case.set(CASES["name_key"], case.attrib[CASES["name_key"]] + CASES["annotation"])
        self.assertEqual(set(native.check_runtime_report(ET.tostring(root))),
                         set(CASES["expected_tests"]))

    def test_executable_matrix_requires_exact_unskipped_cases(self):
        root = runtime_xml(True)
        self.assertEqual(set(native.check_runtime_report(ET.tostring(root), True)),
                         set(CASES["expected_executable_tests"]))
        with self.assertRaises(ValueError):
            native.check_runtime_report(ET.tostring(runtime_xml()), True)
        for tag in CASES["failure_tags"]:
            root = runtime_xml(True)
            ET.SubElement(root[0], tag)
            with self.assertRaises(ValueError):
                native.check_runtime_report(ET.tostring(root), True)

    def test_executable_preflight_rejects_wrong_loading_or_stale_evidence(self):
        for mutation in CASES["executable_probe_mutations"]:
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                probe = root / CASES["probe_file"]
                native.save(probe, dict(CASES["passed_startup_probe"], **mutation))
                with mock.patch.object(native, "provenance", return_value=CASES["provenance"]), \
                        mock.patch.object(native, "command") as command:
                    with self.assertRaises(ValueError):
                        native.run_installation(root, root, probe, executable=True)
                    command.assert_not_called()

    def test_executable_run_derives_launcher_and_requires_matching_startup_profile(self):
        for valid in (False, True):
            with self.subTest(valid=valid), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                probe = root / CASES["probe_file"]
                binary = root / native.POLICY["test_binary"]
                binary.write_text(CASES["test_binary_bytes"])
                calls = []

                def run(products, output, label, argv, timeout, env=None):
                    calls.append((label, argv, env))
                    stdout = CASES["syscall_macros"]
                    if label == native.POLICY["selection_label"]:
                        stdout = json.dumps({"tests": [
                            {"command": [str(binary), native.POLICY["gtest_prefix"] + name]}
                            for name in CASES["expected_executable_tests"]]})
                    if label == native.POLICY["runtime_label_name"]:
                        (output / native.POLICY["xml_name"]).write_bytes(
                            ET.tostring(runtime_xml(True)))
                    return subprocess.CompletedProcess(argv, 0, stdout, "")

                with mock.patch.object(native, "provenance", return_value=CASES["provenance"]), \
                        mock.patch.object(native, "command", side_effect=run):
                    profile = native.startup_profile(None, root)
                    native.save(probe, dict(CASES["passed_startup_probe"], **{
                        native.POLICY["startup_profile_key"]:
                            profile if valid else CASES["foreign_profile"]}))
                    calls.clear()
                    if not valid:
                        with self.assertRaises(ValueError):
                            native.run_installation(root, root, probe, executable=True)
                        self.assertEqual([call[0] for call in calls], [native.POLICY["syscall_label"]])
                        continue
                    report = native.run_installation(root, root, probe, executable=True)
                    self.assertTrue(report[native.POLICY["executable_qualification_key"]])
                    self.assertFalse(report[native.POLICY["publication_qualification_key"]])
                    self.assertFalse(report["whole_image_publication_qualified"])
                    label, argv, env = calls[-1]
                    self.assertEqual(label, native.POLICY["runtime_label_name"])
                    self.assertEqual(argv[:len(profile["launcher"])], profile["launcher"])
                    self.assertEqual(env[native.POLICY["executable_environment"]],
                                     native.POLICY["executable_environment_value"])

    def test_unqualified_or_stale_probe_stops_before_test_execution(self):
        for mutation in CASES["probe_mutations"]:
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                report = dict(CASES["passed_probe"], **mutation)
                probe = root / CASES["probe_file"]
                probe.write_text(json.dumps(report))
                with mock.patch.object(native, "provenance", return_value=CASES["provenance"]), \
                        mock.patch.object(native, "command") as command:
                    with self.assertRaises(ValueError):
                        native.run_installation(root, root, probe)
                    command.assert_not_called()
                actual = json.loads((root / native.POLICY["report_name"]).read_text())
                self.assertFalse(actual[CASES["qualification_key"]])

    def test_default_executable_requires_current_system_pac_and_unwind_controls(self):
        for mutation in CASES["probe_mutations"]:
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                probe = root / CASES["probe_file"]
                native.save(probe, dict(CASES["passed_probe"], **mutation))
                with mock.patch.object(native, "provenance", return_value=CASES["provenance"]), \
                        mock.patch.object(native, "command") as command:
                    with self.assertRaises(ValueError):
                        native.run_installation(root, root, probe, executable=True,
                                                system_startup=True)
                    command.assert_not_called()

    def test_default_executable_runs_without_a_profile_and_checks_every_case(self):
        for skipped in (False, True):
            with self.subTest(skipped=skipped), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                probe = root / CASES["probe_file"]
                native.save(probe, CASES["passed_probe"])
                binary = root / native.POLICY["test_binary"]
                binary.write_text(CASES["test_binary_bytes"])

                def run(products, output, label, argv, timeout, env=None):
                    self.assertEqual(argv[:len(native.POLICY["ctest_command"])],
                                     native.POLICY["ctest_command"])
                    if label == native.POLICY["selection_label"]:
                        stdout = json.dumps({"tests": [
                            {"command": [str(binary), native.POLICY["gtest_prefix"] + name]}
                            for name in CASES["expected_executable_tests"]]})
                    else:
                        self.assertEqual(label, native.POLICY["runtime_label_name"])
                        self.assertEqual(env[native.POLICY["executable_environment"]],
                                         native.POLICY["system_fixup_loader"])
                        xml = runtime_xml(True)
                        if skipped:
                            ET.SubElement(xml[0], CASES["failure_tags"][-1])
                        (output / native.POLICY["xml_name"]).write_bytes(ET.tostring(xml))
                        stdout = ""
                    return subprocess.CompletedProcess(argv, 0, stdout, "")

                with mock.patch.object(native, "provenance", return_value=CASES["provenance"]), \
                        mock.patch.object(native, "command", side_effect=run):
                    if skipped:
                        with self.assertRaises(ValueError):
                            native.run_installation(root, root, probe, executable=True,
                                                    system_startup=True)
                    else:
                        native.run_installation(root, root, probe, executable=True,
                                                system_startup=True)
                report = json.loads((root / native.POLICY["report_name"]).read_text())
                self.assertEqual(report[native.POLICY["default_executable_qualification_key"]],
                                 not skipped)
                self.assertFalse(report["whole_image_publication_qualified"])
                self.assertNotIn(native.POLICY["startup_profile_key"], report)

    def test_generated_matrices_require_exact_cases_and_explicit_frontend(self):
        for executable in (False, True):
            for skipped in (False, True):
                with self.subTest(executable=executable, skipped=skipped), tempfile.TemporaryDirectory() as temporary:
                    root = Path(temporary)
                    probe = root / CASES["probe_file"]
                    native.save(probe, CASES["passed_probe"])
                    binary = root / native.POLICY["test_binary"]
                    binary.write_text(CASES["test_binary_bytes"])
                    compiler = root / CASES["ir_compiler_file"]
                    compiler.write_text(CASES["ir_compiler_bytes"])
                    compiler.chmod(compiler.stat().st_mode | 0o100)
                    matrix = runtime_xml(executable, True)
                    names = [case.attrib[CASES["name_key"]] for case in matrix]
                    with self.assertRaises(ValueError):
                        native.check_runtime_report(ET.tostring(runtime_xml(executable)), executable, True)

                    def run(products, output, label, argv, timeout, env=None):
                        if label == native.POLICY["ir_compiler_label"]:
                            self.assertEqual(argv, [str(compiler.resolve()), *native.POLICY["ir_compiler_version_args"]])
                            stdout = CASES["ir_compiler_version"]
                        elif label == native.POLICY["selection_label"]:
                            stdout = json.dumps({"tests": [
                                {"command": [str(binary), native.POLICY["gtest_prefix"] + name]}
                                for name in names]})
                        else:
                            self.assertEqual(label, native.POLICY["runtime_label_name"])
                            self.assertEqual(argv[:len(native.POLICY["ctest_command"])], native.POLICY["ctest_command"])
                            self.assertEqual(env[native.fixture_contract()["ir_compiler_environment"]], str(compiler.resolve()))
                            if executable:
                                self.assertEqual(env[native.POLICY["executable_environment"]], native.POLICY["system_fixup_loader"])
                            if skipped:
                                ET.SubElement(matrix[0], CASES["failure_tags"][-1])
                            (output / native.POLICY["xml_name"]).write_bytes(ET.tostring(matrix))
                            stdout = ""
                        return subprocess.CompletedProcess(argv, 0, stdout, "")

                    with mock.patch.object(native, "provenance", return_value=CASES["provenance"]), \
                            mock.patch.object(native, "command", side_effect=run):
                        if skipped:
                            with self.assertRaises(ValueError):
                                native.run_installation(root, root, probe, executable=executable,
                                                        system_startup=True, ir_compiler=compiler)
                        else:
                            native.run_installation(root, root, probe, executable=executable,
                                                    system_startup=True, ir_compiler=compiler)
                    report = json.loads((root / native.POLICY["report_name"]).read_text())
                    key = "generated_executable_qualification_key" if executable else "generated_qualification_key"
                    self.assertEqual(report[native.POLICY[key]], not skipped)
                    self.assertEqual(report[native.POLICY["ir_compiler_key"]]["sha256"], native.digest(compiler))
                    self.assertFalse(report["whole_image_publication_qualified"])
                    self.assertFalse(report[native.POLICY["default_executable_qualification_key"]])

    def test_unavailable_generated_frontend_stops_before_ctest(self):
        with tempfile.TemporaryDirectory() as temporary, \
                mock.patch.object(native, "provenance", return_value=CASES["provenance"]), \
                mock.patch.object(native, "command") as command:
            root = Path(temporary)
            probe = root / CASES["probe_file"]
            native.save(probe, CASES["passed_probe"])
            with self.assertRaises(ValueError):
                native.run_installation(root, root, probe, ir_compiler=root / CASES["ir_compiler_file"])
            command.assert_not_called()


if __name__ == "__main__":
    unittest.main()
