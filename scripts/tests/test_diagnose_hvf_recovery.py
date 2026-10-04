import copy
import json
import os
import re
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest import mock

from scripts import diagnose_hvf_recovery as diagnostic
from scripts import run_native_cpu_methods as runner


NAME = "HvfExecutor.NativeIntelCancellationAndCompletionFailureAllowRetry"


def lifecycle_marker(phase, generation, owner=42):
    return f"INTEL_LIFECYCLE phase={phase} generation={generation} owner={owner}\n"


def owner_marker(phase, generation, owner=None):
    owner = generation + 41 if owner is None else owner
    return (f"INTEL_OWNER phase={phase} vcpu_generation={generation} "
            f"owner_generation={generation} owner={owner} vm_generation=1\n")


def retirement(number, owner=False):
    return "".join((owner_marker if owner else lifecycle_marker)(phase, number + 1)
                   for phase in ("executor_retire_begin", "executor_retire_end"))


def iteration(number, name=NAME, reuse=False, recreate=False, recreate_vm=False,
              recreate_owner=False, controls=False, omit_kick=False):
    marker = "INTEL_PROBE phase=executor_retained\n" if reuse else ""
    if omit_kick:
        marker += "INTEL_RECOVERY host_kick=omitted\n"
    if recreate_owner:
        marker += owner_marker("replacement_parked", number + 1)
        marker += "".join(owner_marker(phase, number) for phase in (
            "vcpu_destroy_begin", "vcpu_destroy_end", "lease_release_end",
            "owner_join_begin", "owner_join_end"))
        marker += "".join(owner_marker(phase, number + 1) for phase in (
            "owner_activate", "lease_acquire_end", "vcpu_create_begin", "vcpu_create_end"))
    elif recreate:
        phases = ("vcpu_destroy_begin", "vcpu_destroy_end", "vcpu_create_begin", "vcpu_create_end")
        if recreate_vm:
            phases = phases[:2] + ("vm_destroy_begin", "vm_destroy_end", "vm_create_begin", "vm_create_end") + phases[2:]
        marker += "".join(lifecycle_marker(phase, number + int("create" in phase)) for phase in phases)
    if controls:
        marker += "".join(f"INTEL_HANDOFF_CONTROL case={case} phase={phase}\n"
                          for case in ("replacement", "old_destroy", "new_create")
                          for phase in ("begin", "state_checked", "cleanup_end"))
    return (f"Repeating all tests (iteration {number}) . . .\n"
            f"[ RUN      ] {name}\n{marker}[       OK ] {name} (150 ms)\n"
            "[  PASSED  ] 1 test.\n")


class RecoveryLogTests(unittest.TestCase):
    def test_host_kick_control_requires_one_witness_before_each_vm_reset(self):
        name = diagnostic.EXPERIMENTS['recovery-vm-no-host-kick']
        log = ''.join(iteration(i, name, True, True, True, omit_kick=True)
                      for i in range(1, 101)) + retirement(100)
        parse = lambda text: diagnostic.read_repetitions(
            text, name, 100, True, True, True, host_kick_omitted=True)
        self.assertIsNone(parse(log)['error'])
        witness = 'INTEL_RECOVERY host_kick=omitted\n'
        start = lifecycle_marker('vcpu_destroy_begin', 1)
        retained = 'INTEL_PROBE phase=executor_retained\n'
        changes = (log.replace(witness, '', 1), log.replace(witness, witness * 2, 1),
                   log.replace(witness + start, start + witness, 1),
                   log.replace(retained + witness, witness + retained, 1),
                   log.replace(witness, 'INTEL_RECOVERY host_kick=issued\n', 1),
                   log.replace(retirement(100), ''), witness + log, log + witness)
        for changed in changes:
            self.assertIsNotNone(parse(changed)['error'])
        # The original test cannot pass by accidentally selecting the control.
        self.assertIsNotNone(diagnostic.read_repetitions(log, name, 100, True, True, True)['error'])
        with self.assertRaises(ValueError):
            diagnostic.read_repetitions(log, NAME, 100, True, True, True, host_kick_omitted=True)

    def test_owner_handoff_requires_join_lease_generations_and_actual_final_owner(self):
        log = "".join(iteration(i, reuse=True, recreate=True, recreate_owner=True)
                      for i in range(1, 101)) + retirement(100, owner=True)
        parse = lambda text: diagnostic.read_repetitions(text, NAME, 100, True, True, False, True)
        self.assertIsNone(parse(log)["error"])
        self.assertIsNotNone(parse(re.sub(r" owner=\d+ ", " owner=42 ", log))["error"])
        reused_ids = re.sub(r" owner=(\d+) ", lambda m: f" owner={42 + int(m[1]) % 2} ", log)
        self.assertIsNone(parse(reused_ids)["error"])
        event = owner_marker("owner_join_end", 22)
        following = owner_marker("owner_activate", 23)
        last = owner_marker("executor_retire_end", 101)
        changes = (log.replace(event, ""), log.replace(event, event * 2),
                   log.replace(event + following, following + event),
                   log.replace(following, owner_marker("owner_activate", 23, 42)),
                   log.replace(event, owner_marker("owner_join_end", 22, 100)),
                   log.replace("owner_generation=22", "owner_generation=21", 1),
                   log.replace("vm_generation=1", "vm_generation=2", 1),
                   log.replace(last, owner_marker("executor_retire_end", 100)),
                   log.replace(last, ""), last + log, log + last,
                   log.replace(owner_marker("lease_release_end", 22), ""),
                   log.replace(owner_marker("lease_acquire_end", 23), ""),
                   log.replace(event, lifecycle_marker("vcpu_destroy_end", 22)))
        for changed in changes:
            with self.subTest(log=changed[-200:]):
                self.assertIsNotNone(parse(changed)["error"])
        self.assertIsNotNone(diagnostic.read_repetitions(log, NAME, 100, True, True)["error"])
        for args in ((True, False, False, True), (True, True, True, True)):
            with self.assertRaises(ValueError):
                diagnostic.read_repetitions(log, NAME, 100, *args)

    def test_each_injected_control_requires_state_check_and_cleanup(self):
        log = "".join(iteration(i, controls=True) for i in range(1, 101))
        parse = lambda text: diagnostic.read_repetitions(text, NAME, 100, owner_failure_controls=True)
        self.assertIsNone(parse(log)["error"])
        end = "INTEL_HANDOFF_CONTROL case=old_destroy phase=cleanup_end\n"
        following = "INTEL_HANDOFF_CONTROL case=new_create phase=begin\n"
        for changed in (log.replace(end, "", 1), log.replace(end, end * 2, 1),
                        log.replace(end + following, following + end, 1),
                        log.replace("phase=state_checked", "phase=cleanup_end", 1),
                        log.replace("case=replacement", "case=old_destroy", 1), end + log, log + end):
            self.assertIsNotNone(parse(changed)["error"])
        self.assertIsNotNone(diagnostic.read_repetitions(log, NAME, 100)["error"])
        with self.assertRaises(ValueError):
            diagnostic.read_repetitions(log, NAME, 100, True, owner_failure_controls=True)

    def test_vm_recreation_requires_both_resources_in_order(self):
        log = "".join(iteration(i, reuse=True, recreate=True, recreate_vm=True) for i in range(1, 101)) + retirement(100)
        self.assertIsNone(diagnostic.read_repetitions(log, NAME, 100, True, True, True)["error"])
        event = lifecycle_marker("vm_destroy_end", 22)
        following = lifecycle_marker("vm_create_begin", 23)
        vm_only = "\n".join(line for line in log.splitlines() if "phase=vcpu_" not in line)
        cpu_only = "\n".join(line for line in log.splitlines() if "phase=vm_" not in line)
        changes = (log.replace(event, ""), log.replace(event, event * 2),
                   log.replace(event + following, following + event),
                   log.replace(following, lifecycle_marker("vm_create_begin", 22)),
                   log.replace(event, lifecycle_marker("vm_destroy_end", 22, 43)),
                   vm_only, cpu_only, log.replace(retirement(100), ""))
        for changed in changes:
            with self.subTest(log=changed[-200:]):
                self.assertIsNotNone(diagnostic.read_repetitions(changed, NAME, 100, True, True, True)["error"])
        self.assertIsNotNone(diagnostic.read_repetitions(log, NAME, 100, True, True)["error"])
        for args in ((False, True, False), (True, False, True)):
            with self.assertRaises(ValueError):
                diagnostic.read_repetitions(log, NAME, 100, *args)

    def test_recreation_requires_owner_generation_native_calls_and_final_retirement(self):
        log = "".join(iteration(i, reuse=True, recreate=True) for i in range(1, 101)) + retirement(100)
        self.assertIsNone(diagnostic.read_repetitions(log, NAME, 100, True, True)["error"])
        event = lifecycle_marker("vcpu_destroy_end", 22)
        end = lifecycle_marker("executor_retire_end", 101)
        malformed = [log.replace(event, ""), log.replace(event, event * 2),
                     log.replace(event, lifecycle_marker("vcpu_create_end", 22)),
                     log.replace(event, lifecycle_marker("vcpu_destroy_end", 23)),
                     log.replace(event, lifecycle_marker("vcpu_destroy_end", 22, 43)),
                     log.replace(event, lifecycle_marker("vcpu_destroy_end", 22, 0)),
                     log.replace(event, "INTEL_LIFECYCLE corrupt\n"),
                     log.replace(retirement(100), ""), log.replace(end, ""), log + end,
                     retirement(100) + log, log.replace(event, event + retirement(100)),
                     log.replace("INTEL_PROBE phase=executor_retained\n", "", 1)]
        for changed in malformed:
            with self.subTest(log=changed[-200:]):
                self.assertIsNotNone(diagnostic.read_repetitions(changed, NAME, 100, True, True)["error"])
        self.assertIsNotNone(diagnostic.read_repetitions(log, NAME, 100, True)["error"])

    def test_reuse_requires_one_native_marker_inside_every_iteration(self):
        log = "".join(iteration(i, reuse=True) for i in range(1, 101))
        marker = "INTEL_PROBE phase=executor_retained\n"
        self.assertIsNone(diagnostic.read_repetitions(log, NAME, 100, True)["error"])
        for changed in (log.replace(marker, "", 1), log.replace(marker, marker * 2, 1),
                        marker + log, log + marker):
            with self.subTest(log=changed[:100]):
                self.assertIsNotNone(diagnostic.read_repetitions(changed, NAME, 100, True)["error"])
        self.assertIsNotNone(diagnostic.read_repetitions(log, NAME, 100, False)["error"])

    def test_requires_continuous_exact_native_iterations(self):
        log = "".join(iteration(i) for i in range(1, 101))
        self.assertIsNone(diagnostic.read_repetitions(log, NAME, 100)["error"])
        mutations = [log.replace("iteration 31", "iteration 30"),
                     log.replace(iteration(22), ""), log + iteration(101),
                     log.replace(NAME, "HvfExecutor.Other", 1),
                     log.replace("[  PASSED  ] 1 test.", "[  PASSED  ] 2 tests.", 1),
                     log.replace("[ RUN      ]", "[  SKIPPED ]", 1),
                     log.replace(f"[ RUN      ] {NAME}", f"[ RUN      ] {NAME}\n[ RUN      ] {NAME}", 1),
                     log + "[  FAILED  ] native\n", log.rsplit("[  PASSED  ]", 1)[0]]
        for malformed in mutations:
            with self.subTest(log=malformed[-160:]):
                self.assertIsNotNone(diagnostic.read_repetitions(malformed, NAME, 100)["error"])

    def test_green_previous_round_is_not_evidence_of_a_later_failure(self):
        log = iteration(1) + f"Repeating all tests (iteration 2) . . .\n[ RUN      ] {NAME}\nsource:1: Failure\n"
        result = diagnostic.read_repetitions(log, NAME, 100)
        self.assertEqual(result["completed_repetitions"], 1)
        self.assertEqual(result["started_repetitions"], 2)
        self.assertIn("failure", result["error"])

    def test_recovery_vm_prefix_requires_all_completed_boundaries(self):
        first = iteration(1, reuse=True, recreate=True, recreate_vm=True)
        second = iteration(2, reuse=True, recreate=True, recreate_vm=True)
        prefix = first + second.split(lifecycle_marker("vm_create_begin", 3))[0]
        result = diagnostic.read_repetitions(prefix, NAME, 1000, True, True, True)
        self.assertEqual(result, {"completed_repetitions": 1, "started_repetitions": 2,
                                 "next_expected_event": "ok", "error": "incomplete repetition evidence"})
        for broken in (prefix.replace(lifecycle_marker("vm_destroy_end", 1), ""),
                       prefix.replace(lifecycle_marker("vm_create_begin", 2), "")):
            self.assertNotEqual(diagnostic.read_repetitions(broken, NAME, 1000, True, True, True)["error"],
                                "incomplete repetition evidence")


class RecoveryContractTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.build = self.root / "build"
        (self.build / "bin").mkdir(parents=True)
        self.binary = self.build / "bin/NeverDHvfTests"
        self.evidence = self.build / "evidence"
        (self.build / "CMakeCache.txt").write_text(
            f"CMAKE_HOME_DIRECTORY:INTERNAL={self.root}\nCMAKE_BUILD_TYPE:STRING=Release\n"
            "NEVERD_EMULATION_BACKEND_HVF:BOOL=ON\nNEVERD_EMULATION_BACKEND_UNICORN:BOOL=OFF\n"
            "NEVERD_LLVM_PREBUILT:BOOL=OFF\n")
        self.document = {"kind": "ctestInfo", "version": {"major": 1}, "tests": [{
            "name": NAME, "command": [str(self.binary), "--gtest_filter=" + NAME,
                                     "--gtest_also_run_disabled_tests"],
            "properties": [
                {"name": "LABELS", "value": ["NeverDHvfTests"]},
                {"name": "ENVIRONMENT", "value": ["NEVERD_SIGNATURE_CACHE=off"]},
                {"name": "WORKING_DIRECTORY", "value": str(self.root)},
                {"name": "SKIP_REGULAR_EXPRESSION", "value": [r"\[  SKIPPED \]"]},
                {"name": "TIMEOUT", "value": 20.0}]}]}
        ci = SimpleNamespace(hvf_inventory=lambda *_: (["NeverDHvfTests"], {NAME}))
        self.patches = [
            mock.patch.object(diagnostic.shared, "source_identity", return_value="a" * 40),
            mock.patch.object(diagnostic.shared, "source_modules", return_value=(ci, runner)),
            mock.patch.object(diagnostic.platform, "machine", return_value="x86_64"),
            mock.patch.object(diagnostic.platform, "system", return_value="Darwin")]
        for patch in self.patches:
            patch.start()
            self.addCleanup(patch.stop)

    def prepare(self, repetitions=100, experiment="recovery"):
        with mock.patch.object(diagnostic.subprocess, "check_output", return_value=json.dumps(self.document)):
            return diagnostic.prepare(self.root, self.build, self.evidence, repetitions, experiment)

    def executable(self, suffix=""):
        log = "".join(iteration(i) for i in range(1, 101))
        self.binary.write_text(f"#!{sys.executable}\nimport os, sys, signal\n"
            "assert os.environ['NEVERD_REQUIRE_HVF'] == '1'\n"
            "assert 'ACTIONS_RUNTIME_TOKEN' not in os.environ\n"
            "assert 'GITHUB_TOKEN' not in os.environ\n"
            "assert sys.argv[1:] == ['--gtest_filter=HvfExecutor.Native*', '--gtest_repeat=100', '--gtest_break_on_failure']\n"
            f"assert os.getcwd() == {str(self.root)!r}\nprint({log!r}, end='', flush=True)\n{suffix}\n")
        self.binary.chmod(0o755)

    def test_original_single_process_argv_and_required_native_environment(self):
        plan = self.prepare()
        self.assertEqual(plan["timeout_seconds"], 180)
        self.assertFalse(plan["complete_inventory"])
        self.assertEqual(len(plan["command"]), 4)
        self.executable()
        with mock.patch.dict(os.environ, {"ACTIONS_RUNTIME_TOKEN": "secret", "GITHUB_TOKEN": "secret"}):
            self.assertEqual(diagnostic.execute(self.root, self.evidence), 0)
        self.assertTrue(json.loads((self.evidence / "result.json").read_text())["passed"])
        self.assertEqual(len(json.loads((self.evidence / "retirement.json").read_text())), 1)
        self.assertFalse(list(self.evidence.rglob("*.xml")))

    def add_probe(self, name):
        record = copy.deepcopy(self.document["tests"][0])
        record["name"] = name
        record["command"][1] = "--gtest_filter=" + name
        self.document["tests"].append(record)

    def test_opt_in_experiments_never_replace_required_recovery(self):
        for experiment, name in diagnostic.EXPERIMENTS.items():
            if not any(record["name"] == name for record in self.document["tests"]):
                self.add_probe(name)
            contract = diagnostic.recovery_contract(
                self.root, self.build, self.document, {NAME}, runner, 100, experiment)
            self.assertEqual(contract["native_name"], name)
            native_filter = "HvfExecutor.Native*" if experiment in ("recovery-reuse", "recovery-vcpu-recreate", "recovery-vm-recreate") else name
            self.assertEqual(contract["command"][1], "--gtest_filter=" + native_filter)
            environment = {"NEVERD_REQUIRE_HVF": "1", "NEVERD_HVF_INTEL_PROBE": "1"}
            recreate_owner = experiment == "instruction-owner-recreate"
            recreate_vm = experiment in ("instruction-vm-recreate", "recovery-vm-recreate", "recovery-vm-no-host-kick", "finite-vm-recreate")
            recreate = recreate_owner or recreate_vm or experiment in ("instruction-vcpu-recreate", "recovery-vcpu-recreate", "finite-vcpu-recreate")
            finite_lifecycle = experiment in ("finite-vcpu-recreate", "finite-vm-recreate")
            reuse = recreate or experiment in ("instruction-reuse", "recovery-reuse")
            if reuse:
                environment["NEVERD_HVF_INTEL_REUSE_EXECUTOR"] = "1"
            if recreate:
                environment["NEVERD_HVF_INTEL_RECREATE_VCPU"] = "1"
            if recreate_vm:
                environment["NEVERD_HVF_INTEL_RECREATE_VM"] = "1"
            if recreate_owner:
                environment["NEVERD_HVF_INTEL_RECREATE_OWNER"] = "1"
            self.assertEqual(contract["native_requirements"], environment)
            self.assertEqual(contract["executor_reuse"], reuse)
            self.assertEqual(contract["vcpu_recreate"], recreate)
            recovery_recreate = experiment in ("recovery-vcpu-recreate", "recovery-vm-recreate", "recovery-vm-no-host-kick")
            self.assertEqual(contract["vcpu_generations"], 101 if recreate and not recovery_recreate else None)
            self.assertEqual(contract["vcpu_boundary_generations"], 101 if recovery_recreate else None)
            self.assertEqual(contract["vm_recreate"], recreate_vm)
            self.assertEqual(contract["vm_generations"], 101 if recreate_vm else 1 if finite_lifecycle else None)
            controls = experiment == "owner-failure-controls"
            self.assertEqual(contract["owner_recreate"], recreate_owner)
            self.assertEqual(contract["owner_generations"], 101 if recreate_owner else 1 if finite_lifecycle else None)
            self.assertEqual(contract["owner_failure_controls"], controls)
            self.assertEqual(contract["expected_fault_checks"], 300 if controls else None)
            self.assertEqual(contract["native_execution"], experiment != "lifecycle" and not controls)
            self.assertEqual(contract["guest_execution"], contract["native_execution"])
            self.assertTrue(contract["native_hvf_calls"])
            self.assertFalse(contract["required_for_acceptance"])
        original = self.prepare()
        self.assertEqual(original["native_name"], NAME)
        self.assertEqual(original["native_requirements"], {"NEVERD_REQUIRE_HVF": "1"})
        self.assertTrue(original["required_for_acceptance"])

    def test_finite_lifecycle_keeps_one_body_and_requires_separate_observations(self):
        name = 'HvfIntelProbe.FiniteDeadline'
        self.add_probe(name)
        for vm in (False, True):
            mode = 'finite-vm-recreate' if vm else 'finite-vcpu-recreate'
            self.evidence = self.build / mode
            plan = self.prepare(1000, mode)
            self.assertEqual(plan['native_name'], name)
            self.assertEqual(plan['command'][1:], ['--gtest_filter=' + name,
                '--gtest_repeat=1000', '--gtest_break_on_failure'])
            self.assertEqual(plan['timeout_seconds'], 600)
            self.assertEqual(plan['vcpu_generations'], 1001)
            self.assertIsNone(plan['vcpu_boundary_generations'])
            self.assertEqual(plan['vm_generations'], 1001 if vm else 1)
            self.assertEqual(plan['owner_generations'], 1)
            self.assertFalse(plan['owner_recreate'] or plan['required_for_acceptance'])
            self.assertTrue(plan['native_execution'] and plan['guest_execution'])
            expected = dict(NEVERD_REQUIRE_HVF='1', NEVERD_HVF_INTEL_PROBE='1',
                NEVERD_HVF_INTEL_REUSE_EXECUTOR='1', NEVERD_HVF_INTEL_RECREATE_VCPU='1')
            if vm:
                expected['NEVERD_HVF_INTEL_RECREATE_VM'] = '1'
            self.assertEqual(plan['native_requirements'], expected)
            self.assertTrue(plan['finite_observation_audit_required'])
            self.assertEqual(plan['finite_slice_ns'], 5000000)
            self.assertEqual(plan['finite_witness_loop_budget_ns'], 2000000000)
            self.assertEqual(plan['finite_witness_loop_max_calls'], 4096)
            self.assertEqual(plan['finite_mtf_observations_per_iteration'], 2)
            text = ''.join(iteration(i, name, True, True, vm) for i in range(1, 1001)) + retirement(1000)
            parse = lambda value: diagnostic.read_repetitions(value, name, 1000, True, True, vm)
            self.assertIsNone(parse(text)['error'])
            for bad in (text.replace(retirement(1000), ''),
                        text.replace(lifecycle_marker('vcpu_destroy_end', 7), '', 1),
                        text.replace('owner=42', 'owner=43', 1)):
                self.assertIsNotNone(parse(bad)['error'])
            # The opposite reset mode cannot satisfy the sealed lifecycle.
            wrong = ''.join(iteration(i, name, True, True, not vm) for i in range(1, 1001)) + retirement(1000)
            self.assertIsNotNone(parse(wrong)['error'])

    def test_parent_probe_environment_cannot_change_selected_experiment(self):
        name = diagnostic.EXPERIMENTS["instruction"]
        self.add_probe(name)
        self.add_probe(diagnostic.EXPERIMENTS["owner-failure-controls"])
        keys = ("NEVERD_HVF_INTEL_PROBE", "NEVERD_HVF_INTEL_REUSE_EXECUTOR", "NEVERD_HVF_INTEL_RECREATE_VCPU", "NEVERD_HVF_INTEL_RECREATE_VM", "NEVERD_HVF_INTEL_RECREATE_OWNER")
        for mode in ("recovery", "instruction", "instruction-reuse", "recovery-reuse", "recovery-vcpu-recreate", "recovery-vm-recreate", "instruction-vcpu-recreate", "instruction-vm-recreate", "instruction-owner-recreate", "owner-failure-controls"):
            with self.subTest(mode=mode):
                self.evidence = self.build / ("evidence-" + mode)
                plan = self.prepare(experiment=mode)
                expected = {key: plan["native_requirements"][key]
                            for key in keys if key in plan["native_requirements"]}
                log = "".join(iteration(i, plan["native_name"], plan["executor_reuse"], plan["vcpu_recreate"], plan["vm_recreate"], plan["owner_recreate"], plan["owner_failure_controls"])
                              for i in range(1, 101))
                if plan["vcpu_recreate"]:
                    log += retirement(100, owner=plan["owner_recreate"])
                self.binary.write_text(f"#!{sys.executable}\nimport os\n"
                    f"assert {{key: os.environ[key] for key in {keys!r} if key in os.environ}} == {expected!r}\n"
                    f"print({log!r}, end='', flush=True)\n")
                self.binary.chmod(0o755)
                with mock.patch.dict(os.environ, dict.fromkeys(keys, "1")):
                    self.assertEqual(diagnostic.execute(self.root, self.evidence), 0)

    def test_recovery_vm_retains_native_identity_budget_and_boundary_only_counter(self):
        plan = self.prepare(1000, "recovery-vm-recreate")
        self.assertEqual(plan["native_name"], NAME)
        self.assertEqual(plan["required_ctest_name"], NAME)
        self.assertEqual(plan["command"][1:], ["--gtest_filter=HvfExecutor.Native*",
                                             "--gtest_repeat=1000", "--gtest_break_on_failure"])
        self.assertEqual(plan["timeout_seconds"], 600)
        self.assertEqual(plan["native_requirements"], {
            "NEVERD_REQUIRE_HVF": "1", "NEVERD_HVF_INTEL_PROBE": "1",
            "NEVERD_HVF_INTEL_REUSE_EXECUTOR": "1", "NEVERD_HVF_INTEL_RECREATE_VCPU": "1",
            "NEVERD_HVF_INTEL_RECREATE_VM": "1"})
        self.assertTrue(plan["executor_reuse"] and plan["vcpu_recreate"] and plan["vm_recreate"])
        self.assertTrue(plan["guest_execution"] and plan["native_execution"] and plan["native_hvf_calls"])
        self.assertFalse(plan["required_for_acceptance"] or plan["owner_recreate"])
        self.assertIsNone(plan["vcpu_generations"])
        self.assertEqual(plan["vcpu_boundary_generations"], 1001)
        self.assertEqual(plan["vm_generations"], 1001)

    def test_recovery_cpu_boundary_preserves_vm_and_rejects_missing_retirement(self):
        plan = self.prepare(1000, "recovery-vcpu-recreate")
        self.assertEqual(plan["native_name"], NAME)
        self.assertEqual(plan["required_ctest_name"], NAME)
        self.assertEqual(plan["command"][1:], ["--gtest_filter=HvfExecutor.Native*",
                                             "--gtest_repeat=1000", "--gtest_break_on_failure"])
        self.assertEqual(plan["timeout_seconds"], 600)
        self.assertEqual(plan["native_requirements"], {
            "NEVERD_REQUIRE_HVF": "1", "NEVERD_HVF_INTEL_PROBE": "1",
            "NEVERD_HVF_INTEL_REUSE_EXECUTOR": "1", "NEVERD_HVF_INTEL_RECREATE_VCPU": "1"})
        self.assertTrue(plan["executor_reuse"] and plan["vcpu_recreate"])
        self.assertFalse(plan["vm_recreate"] or plan["owner_recreate"] or plan["required_for_acceptance"])
        self.assertIsNone(plan["vcpu_generations"])
        self.assertEqual(plan["vcpu_boundary_generations"], 1001)
        # Reuse alone, VM resets or missing final retirement cannot pass this
        # CPU-only experiment, even if every iteration otherwise reports OK.
        for reuse_only, vm, retire in ((True, False, False), (False, True, True), (False, False, False)):
            text = "".join(iteration(i, reuse=True, recreate=not reuse_only, recreate_vm=vm)
                           for i in range(1, 1001))
            if retire:
                text += retirement(1000)
            self.assertIsNotNone(diagnostic.read_repetitions(text, NAME, 1000, True, True)["error"])

    def test_failure_controls_reject_wrong_budget_or_unwitnessed_success(self):
        name = diagnostic.EXPERIMENTS["owner-failure-controls"]
        self.add_probe(name)
        with self.assertRaisesRegex(ValueError, "100 repetitions"):
            self.prepare(1000, "owner-failure-controls")
        self.prepare(experiment="owner-failure-controls")
        log = "".join(iteration(i, name) for i in range(1, 101))
        self.binary.write_text(f"#!{sys.executable}\nprint({log!r}, end='', flush=True)\n")
        self.binary.chmod(0o755)
        self.assertEqual(diagnostic.execute(self.root, self.evidence), 1)
        self.assertIn("missing owner failure control", json.loads((self.evidence / "result.json").read_text())["repetitions"]["error"])

    def test_retained_owner_source_cannot_pass_owner_recreation(self):
        name = diagnostic.EXPERIMENTS["instruction-owner-recreate"]
        self.add_probe(name)
        self.prepare(experiment="instruction-owner-recreate")
        log = "".join(iteration(i, name, reuse=True, recreate=True) for i in range(1, 101)) + retirement(100)
        self.binary.write_text(f"#!{sys.executable}\nprint({log!r}, end='', flush=True)\n")
        self.binary.chmod(0o755)
        self.assertEqual(diagnostic.execute(self.root, self.evidence), 1)
        self.assertFalse(json.loads((self.evidence / "result.json").read_text())["passed"])

    def test_reuse_only_source_cannot_pass_recreation_experiment(self):
        name = diagnostic.EXPERIMENTS["instruction-vcpu-recreate"]
        self.add_probe(name)
        self.prepare(experiment="instruction-vcpu-recreate")
        log = "".join(iteration(i, name, reuse=True) for i in range(1, 101))
        self.binary.write_text(f"#!{sys.executable}\nprint({log!r}, end='', flush=True)\n")
        self.binary.chmod(0o755)
        self.assertEqual(diagnostic.execute(self.root, self.evidence), 1)
        result = json.loads((self.evidence / "result.json").read_text())
        self.assertIn("missing vCPU recreation", result["repetitions"]["error"])

    def test_vcpu_only_source_cannot_pass_vm_recreation_experiment(self):
        name = diagnostic.EXPERIMENTS["instruction-vm-recreate"]
        self.add_probe(name)
        self.prepare(experiment="instruction-vm-recreate")
        log = "".join(iteration(i, name, reuse=True, recreate=True) for i in range(1, 101)) + retirement(100)
        self.binary.write_text(f"#!{sys.executable}\nprint({log!r}, end='', flush=True)\n")
        self.binary.chmod(0o755)
        self.assertEqual(diagnostic.execute(self.root, self.evidence), 1)
        self.assertFalse(json.loads((self.evidence / "result.json").read_text())["passed"])

    def test_old_source_without_reuse_support_cannot_pass_reuse_experiment(self):
        self.prepare(experiment="recovery-reuse")
        self.executable()
        self.assertEqual(diagnostic.execute(self.root, self.evidence), 1)
        result = json.loads((self.evidence / "result.json").read_text())
        self.assertFalse(result["passed"])
        self.assertIn("missing executor reuse marker", result["repetitions"]["error"])

    def test_no_host_kick_probe_requires_exact_inventory_and_runtime_witness(self):
        mode = 'recovery-vm-no-host-kick'
        with self.assertRaisesRegex(ValueError, 'exactly one'):
            self.prepare(experiment=mode)
        name = diagnostic.EXPERIMENTS[mode]
        self.add_probe(name)
        plan = self.prepare(experiment=mode)
        self.assertFalse(plan['required_for_acceptance'])
        self.assertFalse(plan['unsolicited_host_kick'])
        self.assertEqual(plan['expected_host_kick_omissions'], 100)
        self.assertEqual(plan['command'][1], '--gtest_filter=' + name)
        self.assertTrue(plan['vm_recreate'] and plan['vcpu_recreate'] and plan['executor_reuse'])
        self.assertEqual(plan['vcpu_boundary_generations'], 101)
        self.assertIsNone(plan['vcpu_generations'])
        self.assertEqual(plan['timeout_seconds'], 180)
        # Renaming old successful recovery output cannot forge the new probe.
        log = ''.join(iteration(i, name, True, True, True) for i in range(1, 101)) + retirement(100)
        self.binary.write_text(f'#!{sys.executable}\nprint({log!r}, end="", flush=True)\n')
        self.binary.chmod(0o755)
        self.assertEqual(diagnostic.execute(self.root, self.evidence), 1)
        result = json.loads((self.evidence / 'result.json').read_text())
        self.assertFalse(result['passed'])
        self.assertIn('missing host-kick omission', result['repetitions']['error'])

    def test_experiment_missing_owner_or_unknown_mode_is_rejected(self):
        for mode in ("lifecycle", "instruction", "finite-deadline", "arbitrary"):
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                self.prepare(experiment=mode)
        self.add_probe(diagnostic.EXPERIMENTS["lifecycle"])
        self.document["tests"][-1]["command"][0] = str(self.root / "wrong-owner")
        with self.assertRaisesRegex(ValueError, "owner"):
            self.prepare(experiment="lifecycle")
        self.assertFalse(self.evidence.exists())

    def test_incomplete_probe_cannot_pass_or_publish_an_acceptance_result(self):
        name = diagnostic.EXPERIMENTS["finite-deadline"]
        self.add_probe(name)
        plan = self.prepare(experiment="finite-deadline")
        self.assertFalse(plan["required_for_acceptance"])
        self.binary.write_text(f"#!{sys.executable}\nimport os\n"
            "assert os.environ['NEVERD_HVF_INTEL_PROBE'] == '1'\n"
            f"print({iteration(1, name)!r}, end='', flush=True)\n")
        self.binary.chmod(0o755)
        self.assertEqual(diagnostic.execute(self.root, self.evidence), 1)
        result = json.loads((self.evidence / "result.json").read_text())
        self.assertFalse(result["passed"])
        self.assertFalse(result["complete_inventory"])

    def test_all_ok_then_abnormal_exit_is_failure(self):
        self.prepare()
        self.executable("sys.exit(3)")
        self.assertEqual(diagnostic.execute(self.root, self.evidence), 1)
        result = json.loads((self.evidence / "result.json").read_text())
        self.assertEqual(result["repetitions"]["completed_repetitions"], 100)
        self.assertFalse(result["passed"])

    def test_configuration_and_wrong_architecture_fail_before_execution(self):
        with mock.patch.object(diagnostic.platform, "machine", return_value="arm64"):
            with self.assertRaisesRegex(ValueError, "Intel"):
                self.prepare()
        cache = self.build / "CMakeCache.txt"
        original = cache.read_text()
        for before, after in [("Release", "Debug"), ("HVF:BOOL=ON", "HVF:BOOL=OFF"),
                              ("UNICORN:BOOL=OFF", "UNICORN:BOOL=ON"),
                              ("PREBUILT:BOOL=OFF", "PREBUILT:BOOL=ON"),
                              (str(self.root), "/different/source")]:
            cache.write_text(original.replace(before, after))
            with self.assertRaises(ValueError):
                self.prepare()
        cache.write_text(original)
        for count in (0, 1, 101, True):
            with self.assertRaises(ValueError):
                self.prepare(count)
        self.assertFalse(self.evidence.exists())

    def test_missing_ambiguous_or_wrong_native_identity_rejected(self):
        original = copy.deepcopy(self.document)
        self.document["tests"] = []
        with self.assertRaisesRegex(ValueError, "zero tests"):
            self.prepare()
        self.document = copy.deepcopy(original)
        extra = copy.deepcopy(self.document["tests"][0])
        extra["name"] += "Other"
        extra["command"][1] += "Other"
        self.document["tests"].append(extra)
        with self.assertRaisesRegex(ValueError, "exactly one"):
            self.prepare()
        self.document = copy.deepcopy(original)
        self.document["tests"][0]["command"][0] = str(self.root / "different-binary")
        with self.assertRaisesRegex(ValueError, "owner"):
            self.prepare()

    def test_changed_source_or_prepared_command_cannot_execute(self):
        self.prepare()
        with mock.patch.object(diagnostic.shared, "source_identity", return_value="b" * 40):
            with self.assertRaisesRegex(ValueError, "changed"):
                diagnostic.execute(self.root, self.evidence)
        with mock.patch.object(diagnostic.shared, "source_identity", side_effect=ValueError("dirty source")):
            with self.assertRaisesRegex(ValueError, "dirty"):
                diagnostic.execute(self.root, self.evidence)
        plan = json.loads((self.evidence / "plan.json").read_text())
        plan["command"].append("--gtest_filter=*")
        diagnostic.shared.write_json(self.evidence / "plan.json", plan)
        with self.assertRaisesRegex(ValueError, "contract"):
            diagnostic.execute(self.root, self.evidence)
        self.assertFalse((self.evidence / "children.json").exists())

    def test_actual_detached_child_is_retired_on_timeout(self):
        with diagnostic.shared.NativeChildren(self.evidence_parent()):
            status = runner.execute([sys.executable, "-c", "import time; time.sleep(60)"],
                str(self.root), {}, 0.1, self.evidence / "execution")
        self.assertTrue(status["timed_out"])
        self.assertTrue(status["child_retired"])
        self.assertEqual(status["status"], -signal.SIGKILL)
        self.assertTrue(json.loads((self.evidence / "retirement.json").read_text())[0]["retired"])

    def evidence_parent(self):
        self.evidence.mkdir()
        return self.evidence

    def test_actual_controller_cancellation_retires_native_group(self):
        self.evidence_parent()
        program = (
            "import os, sys\nfrom pathlib import Path\n"
            "from scripts.diagnose_hvf_methods import NativeChildren\n"
            "from scripts.run_native_cpu_methods import execute\n"
            "p = Path(sys.argv[1])\nwith NativeChildren(p):\n"
            " execute([sys.executable, '-c', 'import time; time.sleep(60)'], str(p), {}, 60, p / 'execution')\n")
        child = subprocess.Popen([sys.executable, "-c", program, str(self.evidence)])
        try:
            deadline = time.monotonic() + 5
            while not (self.evidence / "children.json").exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue((self.evidence / "children.json").exists())
            child.terminate()
            self.assertEqual(child.wait(timeout=10), 128 + signal.SIGTERM)
            retirement = json.loads((self.evidence / "retirement.json").read_text())
            self.assertTrue(retirement[0]["retired"])
            with self.assertRaises(ProcessLookupError):
                os.killpg(retirement[0]["pid"], 0)
        finally:
            if child.poll() is None:
                child.kill()
                child.wait()


if __name__ == "__main__":
    unittest.main()
