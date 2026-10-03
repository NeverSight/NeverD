import contextlib
import io
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

from scripts import run_native_cpu_ci as native
from scripts.audit_ci_test_inventory import TestRecord
from scripts.tests.test_audit_ci_test_results import junit


class NativeCPUEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.build = self.root / "build"
        self.evidence = self.root / "evidence"
        (self.root / "scripts").mkdir()
        (self.root / "scripts" / "NativeCPUTests.def").write_text(
            'NEVERD_NATIVE_CPU_OUTPUT_LIMIT(65536)\n'
            'NEVERD_NATIVE_CPU_OWNER(Owner)\n'
            'NEVERD_NATIVE_CPU_REQUIRED_CASES("Native/Case/", "cases.def", "CASE")\n'
        )
        (self.root / "cases.def").write_text("CASE(First, 1)\nCASE(Second, 2)\n")
        (self.build / "CMakeFiles").mkdir(parents=True)
        (self.build / "CMakeFiles" / "TargetDirectories.txt").write_text(
            str(self.build / "unittests/emulation/CMakeFiles/Owner.dir") + "\n"
        )
        self.records = tuple(
            TestRecord(name, frozenset({"Owner"}))
            for name in ("Native/Case/First", "Native/Case/Second", "Portable/Case")
        )
        self.reported = self.records
        self.changes = {}
        self.status = 0
        self.executions = []
        self.test_environment = {}

    def execute(self, command, **kwargs):
        self.executions.append(command)
        if command[0] == "ctest":
            self.test_environment = kwargs.get("env", {})
            document = junit(self.reported, self.changes)
            ET.ElementTree(document).write(self.evidence / "results.xml")
        return subprocess.CompletedProcess(command, self.status)

    def capture(self, command, **kwargs):
        if command[0] == "git":
            return "test-commit\n" if command[1] == "rev-parse" else ""
        return json.dumps({
            "kind": "ctestInfo", "version": {"major": 1},
            "tests": [
                {"name": record.name, "properties": [
                    {"name": "LABELS", "value": sorted(record.labels)}
                ]}
                for record in self.records
            ],
        })

    def run_evidence(self, with_drivers=False, require_hvf=False,
                     host_architecture="arm64", darwin_backend=None, hvf_transport_only=False,
                     execution_methods=False):
        with (
            mock.patch.object(native, "ROOT", self.root),
            mock.patch.object(native.platform, "machine", return_value=host_architecture),
            mock.patch.object(native.subprocess, "run", side_effect=self.execute),
            mock.patch.object(
                native.subprocess, "check_output", side_effect=self.capture
            ),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            return native.run(
                self.build, self.evidence, 2, not (require_hvf or darwin_backend),
                with_drivers=with_drivers, require_hvf=require_hvf,
                darwin_backend=darwin_backend,
                hvf_transport_only=hvf_transport_only,
                execution_methods=execution_methods,
            )

    def add_drivers(self):
        (self.root / "scripts" / "NativeDriverTests.def").write_text(
            'NEVERD_NATIVE_DRIVER_OWNER(DriverOwner)\n'
            'NEVERD_NATIVE_DRIVER_REQUIRED_CASES('
            '"Driver/Original/", "drivers.def", "IMAGE")\n'
            'NEVERD_NATIVE_DRIVER_REQUIRED_CASES('
            '"Driver/Rebased/", "drivers.def", "IMAGE")\n'
        )
        (self.root / "drivers.def").write_text("IMAGE(Fixture, 1)\n")
        targets = self.build / "CMakeFiles" / "TargetDirectories.txt"
        targets.write_text(targets.read_text() + str(
            self.build / "unittests/emulation/CMakeFiles/DriverOwner.dir"
        ) + "\n")
        self.records += tuple(
            TestRecord(name, frozenset({"DriverOwner"}))
            for name in ("Driver/Original/Fixture", "Driver/Rebased/Fixture")
        )
        self.reported = self.records

    def test_driver_mode_builds_and_requires_both_load_outcomes(self):
        self.add_drivers()
        self.assertEqual(self.run_evidence(with_drivers=True), 0)
        self.assertIn("DriverOwner", self.executions[0])
        self.assertEqual(self.summary()["required_native_tests"], 4)
        self.assertTrue(self.summary()["with_drivers"])

    def test_missing_driver_registration_cannot_pass_vacuously(self):
        self.add_drivers()
        self.records = self.records[:-1]
        with self.assertRaisesRegex(ValueError, "missing required native WHP"):
            self.run_evidence(with_drivers=True)
        self.assertEqual(len(self.executions), 1)

    def test_skipped_builtin_driver_fails_even_when_cpu_tests_pass(self):
        self.add_drivers()
        self.changes[self.records[-1]] = (
            "notrun", "SKIP_REGULAR_EXPRESSION_MATCHED", "missing built-in image"
        )
        self.assertEqual(self.run_evidence(with_drivers=True), 1)
        self.assertEqual(
            self.summary()["required_native_unexecuted"], [self.records[-1].name]
        )

    def test_cpu_profile_does_not_require_driver_configuration(self):
        self.assertEqual(self.run_evidence(), 0)
        self.assertEqual(self.summary()["owners"], ["Owner"])
        self.assertFalse(self.summary()["with_drivers"])

    def test_method_execution_uses_the_same_missing_and_native_skip_gate(self):
        from scripts.audit_ci_test_results import TestOutcome
        for reported, outcome, child_status, expected in (
            (self.records, "passed", 0, 0),
            (self.records[:-1], "passed", 0, 1),
            (self.records, "skipped", 0, 1),
            (self.records, "passed", 1, 1),
        ):
            cases = [TestOutcome(record, outcome) for record in reported]
            with self.subTest(outcome=outcome, count=len(reported), status=child_status), \
                    mock.patch.object(native, "run_methods", return_value=(cases, child_status)):
                self.assertEqual(self.run_evidence(execution_methods=True), expected)
                self.assertEqual(self.summary()["execution"], "gtest-methods")
                self.assertEqual(self.summary()["parallel"], 1)
                self.assertIsNone(self.summary()["ctest_status"])

    def test_bounded_native_observations_are_retained_for_both_outcomes(self):
        self.assertEqual(self.run_evidence(), 0)
        command = self.executions[1]
        for outcome in ("passed", "failed"):
            index = command.index("--test-output-size-" + outcome)
            self.assertEqual(command[index + 1], "65536")

    def test_hvf_profile_rejects_native_skips_and_preserves_optional_skips(self):
        definition = (self.root / "scripts/NativeCPUTests.def").read_text()
        (self.root / "scripts/NativeHVFTests.def").write_text(
            definition.replace("NEVERD_NATIVE_CPU", "NEVERD_NATIVE_HVF"))
        self.changes[self.records[-1]] = (
            "notrun", "SKIP_REGULAR_EXPRESSION_MATCHED", "other ISA")
        self.assertEqual(self.run_evidence(require_hvf=True), 0)
        self.assertTrue(self.summary()["require_hvf"])
        self.changes[self.records[0]] = (
            "notrun", "SKIP_REGULAR_EXPRESSION_MATCHED", "missing entitlement")
        self.assertEqual(self.run_evidence(require_hvf=True), 1)

    def test_hvf_profile_rejects_deleted_required_registration(self):
        definition = (self.root / "scripts/NativeCPUTests.def").read_text()
        (self.root / "scripts/NativeHVFTests.def").write_text(
            definition.replace("NEVERD_NATIVE_CPU", "NEVERD_NATIVE_HVF"))
        self.records = self.records[1:]
        with self.assertRaisesRegex(ValueError, "missing required native HVF"):
            self.run_evidence(require_hvf=True)

    def test_transport_profile_still_fails_missing_and_skipped_native_execution(self):
        (self.root / "scripts/NativeHVFTests.def").write_text(
            'NEVERD_NATIVE_HVF_OWNER(NeverDHvfTests)\n'
            'NEVERD_NATIVE_HVF_OWNER(GuestOwner)\n'
            'NEVERD_NATIVE_HVF_REQUIRED_TEST("Hvf.Executes")\n'
            'NEVERD_NATIVE_HVF_REQUIRED_TEST("DarwinNative.Reference")\n'
        )
        (self.build / "CMakeFiles/TargetDirectories.txt").write_text(
            str(self.build / "unittests/emulation/CMakeFiles/NeverDHvfTests.dir") + "\n"
        )
        self.records = (TestRecord("Hvf.Executes", frozenset({"NeverDHvfTests"})),)
        self.reported = self.records
        self.assertEqual(self.run_evidence(require_hvf=True, hvf_transport_only=True), 0)
        self.assertEqual(self.summary()["owners"], ["NeverDHvfTests"])
        self.assertTrue(self.summary()["hvf_transport_only"])
        self.assertEqual(self.summary()["required_native_names"], ["Hvf.Executes"])
        self.assertEqual(self.test_environment["NEVERD_REQUIRE_HVF"], "1")
        self.changes[self.records[0]] = ("notrun", "SKIP_REGULAR_EXPRESSION_MATCHED", "no HVF")
        self.assertEqual(self.run_evidence(require_hvf=True, hvf_transport_only=True), 1)
        self.records = (TestRecord("OnlyALoader", frozenset({"NeverDHvfTests"})),)
        with self.assertRaisesRegex(ValueError, "missing required native HVF"):
            self.run_evidence(require_hvf=True, hvf_transport_only=True)

    def test_transport_profile_cannot_disable_hardware_enforcement(self):
        with self.assertRaisesRegex(ValueError, "requires --require-hvf"):
            self.run_evidence(hvf_transport_only=True)

    def test_repository_transport_requirements_are_the_full_gates_transport_subset(self):
        for host in ("arm64", "x86_64"):
            with self.subTest(host=host):
                _, full = native.hvf_inventory(native.ROOT, host)
                owners, transport = native.hvf_inventory(native.ROOT, host, True)
                self.assertEqual(owners, ["NeverDHvfTests"])
                self.assertTrue(transport)
                self.assertLess(transport, full)
                self.assertTrue(all(name.startswith(("Hvf.", "HvfExecutor.", "HvfConfiguration."))
                                    for name in transport))

    def add_hvf_host_requirements(self):
        definition = (self.root / "scripts/NativeCPUTests.def").read_text()
        (self.root / "scripts/NativeHVFTests.def").write_text(
            definition.replace("NEVERD_NATIVE_CPU", "NEVERD_NATIVE_HVF")
            + 'NEVERD_NATIVE_HVF_REQUIRED_HOST_TEST(ARM64, "Darwin/ARM64")\n'
            + 'NEVERD_NATIVE_HVF_REQUIRED_HOST_TEST(X64, "Darwin/X64")\n'
        )
        self.records += tuple(TestRecord(name, frozenset({"Owner"}))
                              for name in ("Darwin/ARM64", "Darwin/X64"))
        self.reported = self.records

    def test_hvf_requires_own_host_guest_execution_and_allows_foreign_isa_skip(self):
        self.add_hvf_host_requirements()
        for host, current, foreign in (("arm64", self.records[-2], self.records[-1]),
                                        ("x86_64", self.records[-1], self.records[-2])):
            with self.subTest(host=host):
                self.changes = {foreign: ("notrun", "SKIP_REGULAR_EXPRESSION_MATCHED", "foreign ISA")}
                self.assertEqual(self.run_evidence(require_hvf=True, host_architecture=host), 0)
                self.assertEqual(self.summary()["required_native_tests"], 3)
                self.assertEqual(self.summary()["host_architecture"], host)
                self.assertIn(current.name, self.summary()["required_native_names"])
                self.assertNotIn(foreign.name, self.summary()["required_native_names"])
                self.changes[current] = ("notrun", "SKIP_REGULAR_EXPRESSION_MATCHED", "missing fixture")
                self.assertEqual(self.run_evidence(require_hvf=True, host_architecture=host), 1)
                self.assertEqual(self.summary()["required_native_unexecuted"], [current.name])

    def test_deleted_host_guest_registration_fails_even_when_owner_still_has_tests(self):
        self.add_hvf_host_requirements()
        self.records = tuple(record for record in self.records if record.name != "Darwin/ARM64")
        with self.assertRaisesRegex(ValueError, "missing required native HVF.*Darwin/ARM64"):
            self.run_evidence(require_hvf=True)

    def test_unknown_host_cannot_choose_an_easier_inventory(self):
        self.add_hvf_host_requirements()
        with self.assertRaisesRegex(ValueError, "host architecture"):
            self.run_evidence(require_hvf=True, host_architecture="riscv64")

    def test_repository_hvf_inventory_requires_all_native_darwin_platforms(self):
        prefix = "Transports/DarwinProcess.StartupDataBSSCarryAndBinaryOutput/"
        for host, suffixes in (("arm64", ("MacOSARM64", "IOSARM64", "SimulatorARM64")),
                                ("x86_64", ("MacOSX64", "SimulatorX64"))):
            with self.subTest(host=host):
                _, required = native.read_inventory(native.ROOT, "NativeHVFTests.def",
                                                     "NEVERD_NATIVE_HVF", host)
                self.assertEqual({name for name in required if name.startswith(prefix)},
                                 {prefix + suffix + "_hvf" for suffix in suffixes})

    def add_darwin_requirements(self, backend):
        (self.root / "scripts/NativeDarwinTests.def").write_text(
            'NEVERD_NATIVE_DARWIN_OWNER(Owner)\n'
            'NEVERD_NATIVE_DARWIN_REQUIRED_HOST_TEST(ARM64, "Darwin/{case}/ARM64_{backend}")\n'
            'NEVERD_NATIVE_DARWIN_REQUIRED_HOST_TEST(X64, "Darwin/{case}/X64_{backend}")\n'
            'NEVERD_NATIVE_DARWIN_CASE(Startup)\n'
            'NEVERD_NATIVE_DARWIN_CASE(Memory)\n'
        )
        self.records += tuple(
            TestRecord(f"Darwin/{case}/{isa}_{backend}", frozenset({"Owner"}))
            for isa in ("ARM64", "X64") for case in ("Startup", "Memory")
        )
        self.reported = self.records

    def test_darwin_requires_every_matching_workload_and_allows_foreign_skips(self):
        self.add_darwin_requirements("kvm")
        self.changes = {
            record: ("notrun", "SKIP_REGULAR_EXPRESSION_MATCHED", "foreign ISA")
            for record in self.records[-2:]
        }
        self.assertEqual(self.run_evidence(darwin_backend="kvm"), 0)
        self.assertEqual(self.summary()["darwin_backend"], "kvm")
        self.assertEqual(self.summary()["required_native_tests"], 2)
        memory = self.records[-3]
        self.changes[memory] = ("notrun", "SKIP_REGULAR_EXPRESSION_MATCHED", "missing fixture")
        self.assertEqual(self.run_evidence(darwin_backend="kvm"), 1)
        self.assertEqual(self.summary()["required_native_unexecuted"], [memory.name])

    def test_darwin_deleted_workload_cannot_be_replaced_by_loader_tests(self):
        self.add_darwin_requirements("whp")
        self.records = tuple(record for record in self.records
                             if record.name != "Darwin/Memory/X64_whp")
        with self.assertRaisesRegex(ValueError, "missing required native WHP.*Darwin/Memory"):
            self.run_evidence(darwin_backend="whp", host_architecture="AMD64")

    def test_darwin_hvf_enables_required_hardware_in_the_test_process(self):
        self.add_darwin_requirements("hvf")
        self.assertEqual(self.run_evidence(darwin_backend="hvf"), 0)
        self.assertEqual(self.test_environment["NEVERD_REQUIRE_HVF"], "1")

    def test_darwin_formatting_cannot_drop_a_required_workload(self):
        self.add_darwin_requirements("hvf")
        path = self.root / "scripts/NativeDarwinTests.def"
        path.write_text(path.read_text().replace(
            "NEVERD_NATIVE_DARWIN_CASE(Memory)",
            "  NEVERD_NATIVE_DARWIN_CASE(\n    Memory\n  )  ",
        ))
        self.assertEqual(native.darwin_inventory(self.root, "hvf", "arm64")[1],
                         {"Darwin/Startup/ARM64_hvf", "Darwin/Memory/ARM64_hvf"})
        memory = self.records[-3]
        self.changes[memory] = ("notrun", "SKIP_REGULAR_EXPRESSION_MATCHED", "missing fixture")
        self.assertEqual(self.run_evidence(darwin_backend="hvf"), 1)
        self.assertEqual(self.summary()["required_native_unexecuted"], [memory.name])

    def test_native_whp_cli_sets_its_test_policy_without_workflow_environment(self):
        self.assertEqual(self.run_evidence(), 0)
        self.assertEqual(self.test_environment["NEVERD_REQUIRE_NATIVE_WHP"], "1")

    def test_repository_darwin_inventory_requires_each_workload_on_every_native_platform(self):
        for backend in ("hvf", "kvm", "whp"):
            for host, platforms in (
                ("aarch64", ("MacOSARM64", "IOSARM64", "SimulatorARM64")),
                ("AMD64", ("MacOSX64", "SimulatorX64")),
            ):
                with self.subTest(backend=backend, host=host):
                    owners, required = native.darwin_inventory(native.ROOT, backend, host)
                    self.assertEqual(owners, ["NeverDDarwinProcessTests"])
                    self.assertEqual(len(required), 13 * len(platforms))
                    self.assertEqual({name.rsplit("/", 1)[1] for name in required},
                                     {f"{platform}_{backend}" for platform in platforms})
                    for platform in platforms:
                        self.assertIn(
                            "Transports/DarwinProcess."
                            "UnixThreadReceivesArgcAtTheInitialStackPointer/"
                            f"{platform}_{backend}", required,
                        )
                        self.assertIn(
                            "Transports/DarwinProcess."
                            "InitialStackAndDataPartialUnmapReleasesPhysicalBudget/"
                            f"{platform}_{backend}", required,
                        )

    def test_darwin_rejects_software_and_unknown_architecture(self):
        with self.assertRaisesRegex(ValueError, "requires HVF, KVM or WHP"):
            native.darwin_inventory(native.ROOT, "unicorn", "arm64")
        with self.assertRaisesRegex(ValueError, "host architecture"):
            native.darwin_inventory(native.ROOT, "kvm", "riscv64")
        with self.assertRaisesRegex(ValueError, "separate native profile"):
            native.run(self.build, self.evidence, 2, True, darwin_backend="whp")

    def test_every_darwin_process_case_is_required_on_native_hosts(self):
        source = (native.ROOT / "unittests/emulation/DarwinProcessTests.cpp").read_text()
        cases = set(re.findall(r"TEST_P\(\s*DarwinProcess,\s*(\w+)\s*\)", source))
        self.assertTrue(cases)
        for host in ("arm64", "x86_64"):
            with self.subTest(host=host):
                _, required = native.darwin_inventory(native.ROOT, "hvf", host)
                declared = {name.split(".", 1)[1].split("/", 1)[0]
                            for name in required}
                self.assertEqual(declared, cases,
                                 "every new workload must join the native evidence gate")

    def test_darwin_rejects_missing_or_duplicate_workload_requirements(self):
        self.add_darwin_requirements("hvf")
        path = self.root / "scripts/NativeDarwinTests.def"
        definition = path.read_text()
        for invalid in (
            definition + 'NEVERD_NATIVE_DARWIN_CASE(Memory)\n',
            definition.replace('NEVERD_NATIVE_DARWIN_CASE(Startup)\n', '')
                      .replace('NEVERD_NATIVE_DARWIN_CASE(Memory)\n', ''),
            definition.replace('{backend}', 'unicorn'),
        ):
            with self.subTest(invalid=invalid):
                path.write_text(invalid)
                with self.assertRaises(ValueError):
                    native.darwin_inventory(self.root, "hvf", "arm64")

    def summary(self):
        return json.loads((self.evidence / "summary.json").read_text())

    def test_all_required_native_cases_pass_while_optional_software_skips(self):
        self.changes[self.records[-1]] = (
            "notrun", "SKIP_REGULAR_EXPRESSION_MATCHED", "software disabled"
        )
        self.assertEqual(self.run_evidence(), 0)
        self.assertEqual(self.summary()["counts"]["passed"], 2)
        self.assertEqual(self.summary()["counts"]["skipped"], 1)
        self.assertIn("Owner", self.executions[0])

    def test_deleted_native_registration_cannot_pass_vacuously(self):
        self.records = self.records[1:]
        with self.assertRaisesRegex(ValueError, "missing required native WHP"):
            self.run_evidence()
        self.assertEqual(len(self.executions), 1)

    def test_native_skip_is_failure_even_when_ctest_returns_zero(self):
        self.changes[self.records[0]] = (
            "notrun", "SKIP_REGULAR_EXPRESSION_MATCHED", "host unavailable"
        )
        self.assertEqual(self.run_evidence(), 1)
        self.assertEqual(
            self.summary()["required_native_unexecuted"], [self.records[0].name]
        )

    def test_infrastructure_skip_remains_not_run(self):
        self.changes[self.records[-1]] = ("notrun", "Unable to find executable", "")
        self.assertEqual(self.run_evidence(), 1)
        self.assertEqual(self.summary()["counts"]["not_run"], 1)
        self.assertEqual(self.summary()["counts"]["skipped"], 0)

    def test_same_count_with_different_owner_identity_is_failure(self):
        self.reported = (
            *self.records[:-1],
            TestRecord("Portable/Case", frozenset({"Other"})),
        )
        self.assertEqual(self.run_evidence(), 1)
        self.assertEqual(self.summary()["missing"], ["Portable/Case"])
        self.assertEqual(self.summary()["unexpected"], ["Portable/Case"])

    def test_missing_outcome_cannot_be_hidden_by_successful_exit(self):
        self.reported = self.records[:-1]
        self.assertEqual(self.run_evidence(), 1)
        self.assertEqual(self.summary()["missing"], [self.records[-1].name])

    def test_failed_or_disabled_case_prevents_success(self):
        for state in ("fail", "disabled"):
            with self.subTest(state=state):
                self.changes[self.records[-1]] = (state, "", "")
                self.assertEqual(self.run_evidence(), 1)

    def test_missing_owner_fails_before_build(self):
        (self.build / "CMakeFiles" / "TargetDirectories.txt").write_text("")
        with self.assertRaisesRegex(ValueError, "unconfigured"):
            self.run_evidence()
        self.assertEqual(self.executions, [])

    def test_declared_cases_follow_the_independent_def_inventory(self):
        owners, required = native.declared_inventory(self.root)
        self.assertEqual(owners, ["Owner"])
        self.assertEqual(required, {"Native/Case/First", "Native/Case/Second"})
        (self.root / "cases.def").write_text("CASE(First, 1)\nCASE(Third, 3)\n")
        _, required = native.declared_inventory(self.root)
        self.assertEqual(required, {"Native/Case/First", "Native/Case/Third"})

    def test_formatted_adjacent_literals_preserve_required_families(self):
        definition = self.root / "scripts" / "NativeCPUTests.def"
        definition.write_text(definition.read_text().replace(
            '"Native/Case/", "cases.def", "CASE"',
            '"Native/"\n "Case/", "cases."\n "def", "CA"\n "SE"',
        ))
        self.assertEqual(self.run_evidence(), 0)
        self.assertEqual(self.summary()["required_native_tests"], 2)

    def test_named_native_execution_cannot_be_replaced_by_mapping_only(self):
        definition = self.root / "scripts" / "NativeCPUTests.def"
        definition.write_text(
            definition.read_text()
            + 'NEVERD_NATIVE_CPU_REQUIRED_TEST("Portable/"\n "Case")\n'
        )
        self.changes[self.records[-1]] = (
            "notrun", "SKIP_REGULAR_EXPRESSION_MATCHED", "CPU startup unavailable"
        )
        self.assertEqual(self.run_evidence(), 1)
        self.assertEqual(
            self.summary()["required_native_unexecuted"], [self.records[-1].name]
        )


if __name__ == "__main__":
    unittest.main()
