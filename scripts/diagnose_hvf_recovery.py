#!/usr/bin/env python3
"""Instrumented, partial evidence for the original one-process HVF recovery loop."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import re
import sys
import subprocess

try:
    from . import diagnose_hvf_methods as shared
except ImportError:
    import diagnose_hvf_methods as shared


def repetition_budget(repetitions):
    if type(repetitions) is not int or repetitions not in (100, 1000):
        raise ValueError("recovery diagnosis requires 100 or 1000 repetitions")
    return 180 if repetitions == 100 else 600


EXPERIMENTS = {
    "lifecycle": "HvfIntelProbe.LifecycleOnly",
    "instruction": "HvfIntelProbe.InstructionOnly",
    "instruction-reuse": "HvfIntelProbe.InstructionOnly",
    "instruction-vcpu-recreate": "HvfIntelProbe.InstructionOnly",
    "instruction-vm-recreate": "HvfIntelProbe.InstructionOnly",
    "instruction-owner-recreate": "HvfIntelProbe.InstructionOnly",
    "owner-failure-controls": "HvfIntelHandoff.FailureControls",
    "recovery-reuse": "HvfExecutor.NativeIntelCancellationAndCompletionFailureAllowRetry",
    "recovery-vcpu-recreate": "HvfExecutor.NativeIntelCancellationAndCompletionFailureAllowRetry",
    "recovery-vm-recreate": "HvfExecutor.NativeIntelCancellationAndCompletionFailureAllowRetry",
    "recovery-vm-no-host-kick": "HvfIntelRecoveryProbe.WithoutHostKick",
    "finite-deadline": "HvfIntelProbe.FiniteDeadline",
    "finite-vcpu-recreate": "HvfIntelProbe.FiniteDeadline",
    "finite-vm-recreate": "HvfIntelProbe.FiniteDeadline",
}


def recovery_contract(source, build, document, required, runner, repetitions, experiment="recovery"):
    if experiment not in ("recovery", *EXPERIMENTS):
        raise ValueError("unknown Intel experiment")
    recovery = experiment in ("recovery", "recovery-reuse", "recovery-vcpu-recreate", "recovery-vm-recreate")
    kick_control = experiment == "recovery-vm-no-host-kick"
    recovery_workload = recovery or kick_control
    finite_lifecycle = experiment in ("finite-vcpu-recreate", "finite-vm-recreate")
    owner_controls = experiment == "owner-failure-controls"
    if owner_controls and repetitions != 100:
        raise ValueError("owner failure controls require 100 repetitions")
    recreate_owner = experiment == "instruction-owner-recreate"
    recreate_vm = kick_control or experiment in ("instruction-vm-recreate", "recovery-vm-recreate", "finite-vm-recreate")
    recreate = recreate_owner or recreate_vm or experiment in ("instruction-vcpu-recreate", "recovery-vcpu-recreate", "finite-vcpu-recreate")
    reuse = recreate or experiment in ("instruction-reuse", "recovery-reuse")
    methods = runner.method_inventory(document)
    selected = [(key, expected) for key, expected in methods.items()
                if (key[1].startswith("HvfExecutor.Native") if recovery
                    else key[1] == EXPERIMENTS[experiment])]
    if len(selected) != 1 or len(selected[0][1]) != 1:
        raise ValueError("selected filter must select exactly one native recovery test")
    key, expected = selected[0]
    name, record = next(iter(expected.items()))
    binary = (build / "bin/NeverDHvfTests").resolve()
    if (Path(key[0]).resolve() != binary or (recovery and record.name not in required)
            or "NeverDHvfTests" not in record.labels):
        raise ValueError("recovery test is not the selected source's required native owner")
    native_filter = "HvfExecutor.Native*" if recovery else EXPERIMENTS[experiment]
    environment = {"NEVERD_REQUIRE_HVF": "1"}
    if experiment != "recovery":
        environment["NEVERD_HVF_INTEL_PROBE"] = "1"
    if reuse:
        environment["NEVERD_HVF_INTEL_REUSE_EXECUTOR"] = "1"
    if recreate:
        environment["NEVERD_HVF_INTEL_RECREATE_VCPU"] = "1"
    if recreate_vm:
        environment["NEVERD_HVF_INTEL_RECREATE_VM"] = "1"
    if recreate_owner:
        environment["NEVERD_HVF_INTEL_RECREATE_OWNER"] = "1"
    guest = experiment not in ("lifecycle", "owner-failure-controls")
    contract = {
        "experiment": experiment,
        "native_execution": guest,
        "guest_execution": guest,
        "native_hvf_calls": True,
        "owner_recreate": recreate_owner,
        "owner_generations": repetitions + 1 if recreate_owner else 1 if finite_lifecycle else None,
        "owner_failure_controls": owner_controls,
        "expected_fault_checks": 3 * repetitions if owner_controls else None,
        "required_for_acceptance": experiment == "recovery",
        "executor_reuse": reuse,
        "vcpu_recreate": recreate,
        # Recovery recreates vCPUs inside the test too. Lifecycle markers count
        # only fixture boundaries, so they cannot establish its total creations.
        "vcpu_generations": repetitions + 1 if recreate and not recovery_workload else None,
        "vcpu_boundary_generations": repetitions + 1 if recreate and recovery_workload else None,
        "vm_recreate": recreate_vm,
        "vm_generations": repetitions + 1 if recreate_vm else 1 if finite_lifecycle else None,
        "native_name": name, "required_ctest_name": record.name,
        "command": [str(binary), "--gtest_filter=" + native_filter,
                    f"--gtest_repeat={repetitions}", "--gtest_break_on_failure"],
        "working_directory": str(source), "repetitions": repetitions,
        "timeout_seconds": repetition_budget(repetitions),
        "native_requirements": environment,
    }
    if kick_control:
        contract.update(unsolicited_host_kick=False, expected_host_kick_omissions=repetitions)
    if finite_lifecycle:
        # A native repetition/lifecycle result is still partial evidence. The
        # separate transcript audit must also reconcile every raw finite call.
        contract.update(finite_observation_audit_required=True, finite_slice_ns=5000000,
                        finite_witness_loop_budget_ns=2000000000,
                        finite_witness_loop_max_calls=4096,
                        finite_mtf_observations_per_iteration=2)
    return contract


def prepare(source, build, evidence, repetitions, experiment="recovery"):
    repetition_budget(repetitions)
    commit = shared.source_identity(source)
    if platform.system() != "Darwin" or platform.machine() != "x86_64":
        raise ValueError("recovery diagnosis requires a native Intel macOS host")
    cache = shared.build_configuration(source, build)
    if cache.get("NEVERD_LLVM_PREBUILT") != "OFF":
        raise ValueError("Intel recovery diagnosis requires the pinned LLVM source build")
    ci, runner = shared.source_modules(source)
    owners, required = ci.hvf_inventory(source, platform.machine(), True)
    if owners != ["NeverDHvfTests"]:
        raise ValueError("unexpected native transport owner")
    document = json.loads(subprocess.check_output([
        "ctest", "--test-dir", str(build / "unittests/emulation"),
        "--build-config", "Release", "-L", "^NeverDHvfTests$",
        "--show-only=json-v1"], text=True))
    records = runner.parse_inventory(document)
    if required - {record.name for record in records}:
        raise ValueError("transport inventory is missing required native cases")
    contract = recovery_contract(source, build, document, required, runner, repetitions, experiment)
    evidence.mkdir(parents=True, exist_ok=False)
    shared.write_json(evidence / "inventory.json", document)
    plan = {
        "kind": "instrumented-partial-hvf-recovery-plan", "complete_inventory": False,
        "executed": False, "commit": commit, "source_dirty": False,
        "source": str(source), "build": str(build),
        "controller_commit": os.environ.get("GITHUB_SHA"),
        "host_system": platform.system(), "host_architecture": platform.machine(),
        "host_release": platform.release(), "llvm_prebuilt": cache["NEVERD_LLVM_PREBUILT"],
        "inventory_sha256": shared.fingerprint(evidence / "inventory.json"),
        **contract,
    }
    shared.write_json(evidence / "plan.json", plan)
    return plan


OWNER_PHASES = ("replacement_parked", "vcpu_destroy_begin", "vcpu_destroy_end",
                "lease_release_end", "owner_join_begin", "owner_join_end",
                "owner_activate", "lease_acquire_end", "vcpu_create_begin", "vcpu_create_end")
OWNER_CONTROLS = tuple((case, phase) for case in ("replacement", "old_destroy", "new_create")
                       for phase in ("begin", "state_checked", "cleanup_end"))


def read_repetitions(log, name, repetitions, executor_reuse=False, vcpu_recreate=False,
                     vm_recreate=False, owner_recreate=False, owner_failure_controls=False,
                     host_kick_omitted=False):
    """Require every complete iteration in order; totals or overwritten XML cannot prove this."""
    if (vcpu_recreate and not executor_reuse) or (vm_recreate and not vcpu_recreate):
        raise ValueError("VM recreation requires vCPU recreation and retained Executor")
    if owner_recreate and (not vcpu_recreate or vm_recreate):
        raise ValueError("owner recreation requires vCPU recreation and a retained VM")
    if owner_failure_controls and (executor_reuse or vcpu_recreate or vm_recreate or owner_recreate):
        raise ValueError("failure controls require independent Executors")
    if host_kick_omitted and (not vm_recreate or owner_recreate or owner_failure_controls
                             or name != EXPERIMENTS['recovery-vm-no-host-kick']):
        raise ValueError("host-kick control requires the exact VM-recreation probe")
    completed, started, stage = 0, 0, "iteration"
    error = None
    retained = False
    omitted = False
    owner, next_owner, lifecycle, retired, controls = 0, 0, 0, 0, 0
    phases = ("vcpu_destroy_begin", "vcpu_destroy_end", "vcpu_create_begin", "vcpu_create_end")
    if vm_recreate:
        phases = phases[:2] + ("vm_destroy_begin", "vm_destroy_end", "vm_create_begin", "vm_create_end") + phases[2:]
    if owner_recreate:
        phases = OWNER_PHASES
    retirement = ("executor_retire_begin", "executor_retire_end")
    for line in log.splitlines():
        iteration = re.fullmatch(r"Repeating all tests \(iteration ([0-9]+)\) \. \. \.", line)
        run = re.fullmatch(r"\[ RUN      \] (.+)", line)
        passed = re.fullmatch(r"\[       OK \] (.+) \([0-9]+ ms\)", line)
        summary = re.fullmatch(r"\[  PASSED  \] ([0-9]+) tests?\.", line)
        if iteration:
            if stage != "iteration" or int(iteration[1]) != completed + 1 or completed >= repetitions:
                error = "missing, repeated or out-of-order iteration"
                break
            started += 1
            stage = "run"
            retained = False
            omitted = False
            lifecycle = 0
            controls = 0
            next_owner = 0
        elif run:
            if stage != "run" or run[1] != name:
                error = "unexpected or duplicate native RUN"
                break
            stage = "ok"
        elif line == "INTEL_PROBE phase=executor_retained":
            if not executor_reuse or stage != "ok" or retained:
                error = "unexpected or duplicate executor reuse marker"
                break
            retained = True
        elif line.startswith("INTEL_RECOVERY"):
            if (not host_kick_omitted or line != "INTEL_RECOVERY host_kick=omitted"
                    or stage != "ok" or not retained or lifecycle or omitted):
                error = "unexpected, duplicate or misplaced host-kick omission"
                break
            omitted = True
        elif line.startswith("INTEL_HANDOFF_CONTROL"):
            event = re.fullmatch(r"INTEL_HANDOFF_CONTROL case=([a-z_]+) phase=([a-z_]+)", line)
            if (not owner_failure_controls or stage != "ok" or not event
                    or controls >= len(OWNER_CONTROLS) or tuple(event.groups()) != OWNER_CONTROLS[controls]):
                error = "missing, repeated or out-of-order owner failure control"
                break
            controls += 1
        elif line.startswith("INTEL_OWNER"):
            event = re.fullmatch(r"INTEL_OWNER phase=([a-z_]+) vcpu_generation=([0-9]+) owner_generation=([0-9]+) owner=([0-9]+) vm_generation=1", line)
            if not owner_recreate or not event or int(event[4]) == 0:
                error = "unexpected or malformed owner lifecycle marker"
                break
            phase, cpu_generation, generation, current = event[1], int(event[2]), int(event[3]), int(event[4])
            if cpu_generation != generation:
                error = "owner and vCPU generations disagree"
                break
            if stage == "ok" and retained and lifecycle < len(phases):
                next_generation = lifecycle == 0 or lifecycle >= 6
                if phase != phases[lifecycle] or generation != started + int(next_generation):
                    error = "missing, repeated or out-of-order owner lifecycle event"
                    break
                if lifecycle == 0:
                    next_owner = current
                elif lifecycle < 6:
                    # The replacement is already parked while the old owner
                    # lives. Only nonadjacent generations may reuse an ID.
                    if current == next_owner:
                        error = "parked replacement and live owner have the same identity"
                        break
                    if owner and owner != current:
                        error = "previous owner identity changed"
                        break
                    owner = current
                elif current != next_owner:
                    error = "replacement owner identity changed"
                    break
                lifecycle += 1
                if lifecycle == len(phases):
                    owner = next_owner
            elif (stage == "iteration" and completed == repetitions
                    and retired < len(retirement) and phase == retirement[retired]
                    and generation == completed + 1 and current == owner):
                retired += 1
            else:
                error = "unexpected owner lifecycle or retirement event"
                break
        elif line.startswith("INTEL_LIFECYCLE"):
            if host_kick_omitted and stage == "ok" and not omitted:
                error = "missing host-kick omission before VM recreation"
                break
            event = re.fullmatch(r"INTEL_LIFECYCLE phase=([a-z_]+) generation=([0-9]+) owner=([0-9]+)", line)
            if owner_recreate or not vcpu_recreate or not executor_reuse or not event or int(event[3]) == 0:
                error = "unexpected or malformed lifecycle marker"
                break
            phase, generation, current = event[1], int(event[2]), int(event[3])
            if owner and owner != current:
                error = "lifecycle owner changed"
                break
            owner = current
            if (stage == "ok" and retained and lifecycle < len(phases)
                    and phase == phases[lifecycle]
                    and generation == started + int("_create_" in phase)):
                lifecycle += 1
            elif (stage == "iteration" and completed == repetitions
                    and retired < len(retirement) and phase == retirement[retired]
                    and generation == completed + 1):
                retired += 1
            else:
                error = "missing, repeated or out-of-order lifecycle event"
                break
        elif passed:
            if stage != "ok" or passed[1] != name:
                error = "unexpected or duplicate native OK"
                break
            if executor_reuse and not retained:
                error = "missing executor reuse marker"
                break
            if host_kick_omitted and not omitted:
                error = "missing host-kick omission evidence"
                break
            if vcpu_recreate and lifecycle != len(phases):
                error = ("missing owner recreation evidence" if owner_recreate else
                         "missing VM recreation evidence" if vm_recreate else "missing vCPU recreation evidence")
                break
            if owner_failure_controls and controls != len(OWNER_CONTROLS):
                error = "missing owner failure control evidence"
                break
            stage = "summary"
        elif summary:
            if stage != "summary" or summary[1] != "1":
                error = "unexpected or duplicate repetition summary"
                break
            completed += 1
            stage = "iteration"
        elif re.search(r"\[\s*(?:FAILED|SKIPPED)\s*\]|Failure$", line):
            error = "native assertion failure or skip"
            break
    if error is None and (completed != repetitions or stage != "iteration"):
        error = "incomplete repetition evidence"
    if error is None and vcpu_recreate and retired != len(retirement):
        error = "missing final executor retirement evidence"
    return {"started_repetitions": started, "completed_repetitions": completed,
            "next_expected_event": stage, "error": error}


def execute(source, evidence):
    plan = json.loads((evidence / "plan.json").read_text())
    if (plan["source"] != str(source) or plan["commit"] != shared.source_identity(source)
            or plan["complete_inventory"] is not False
            or shared.fingerprint(evidence / "inventory.json") != plan["inventory_sha256"]):
        raise ValueError("recovery source or inventory changed after preparation")
    build = Path(plan["build"])
    cache = shared.build_configuration(source, build)
    if (cache.get("NEVERD_LLVM_PREBUILT") != "OFF" or platform.system() != "Darwin"
            or platform.machine() != "x86_64"):
        raise ValueError("recovery build or host changed after preparation")
    ci, runner = shared.source_modules(source)
    _, required = ci.hvf_inventory(source, platform.machine(), True)
    contract = recovery_contract(source, build,
        json.loads((evidence / "inventory.json").read_text()), required, runner, plan["repetitions"], plan.get("experiment", "recovery"))
    if any(plan.get(key) != value for key, value in contract.items()):
        raise ValueError("recovery command differs from its prepared contract")
    environment = shared.test_environment(os.environ)
    for key in ("NEVERD_HVF_INTEL_PROBE", "NEVERD_HVF_INTEL_REUSE_EXECUTOR",
                "NEVERD_HVF_INTEL_RECREATE_VCPU", "NEVERD_HVF_INTEL_RECREATE_VM",
                "NEVERD_HVF_INTEL_RECREATE_OWNER"):
        environment.pop(key, None)
    environment.update(contract["native_requirements"])
    with shared.NativeChildren(evidence):
        status = runner.execute(contract["command"], contract["working_directory"],
            environment, contract["timeout_seconds"], evidence / "execution")
    repeats = read_repetitions((evidence / "execution/output.log").read_text(),
                              contract["native_name"], contract["repetitions"],
                              contract["executor_reuse"], contract["vcpu_recreate"], contract["vm_recreate"],
                              contract["owner_recreate"], contract["owner_failure_controls"],
                              contract.get("expected_host_kick_omissions", 0) == contract["repetitions"])
    retirement = json.loads((evidence / "retirement.json").read_text())
    success = (not repeats["error"] and status["status"] == 0 and not status["timed_out"]
               and status["child_retired"] and len(retirement) == 1
               and retirement[0]["retired"] and retirement[0]["status"] == 0)
    shared.write_json(evidence / "result.json", {
        "kind": "instrumented-partial-hvf-recovery-result", "complete_inventory": False,
        "commit": plan["commit"], "passed": success, "repetitions": repeats,
        "output_sha256": shared.fingerprint(evidence / "execution/output.log"),
        "diagnostic_status": 0 if success else 1,
    })
    return 0 if success else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("prepare", "execute"))
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--build", type=Path)
    parser.add_argument("--experiment", choices=("recovery", *EXPERIMENTS), default="recovery")
    parser.add_argument("--repetitions", type=int, choices=(100, 1000), default=1000)
    args = parser.parse_args()
    if args.mode == "execute":
        return execute(args.source.resolve(), args.evidence.resolve())
    if args.build is None:
        parser.error("prepare requires --build")
    prepare(args.source.resolve(), args.build.resolve(), args.evidence.resolve(), args.repetitions, args.experiment)
    return 0


if __name__ == "__main__":
    sys.exit(main())
