#!/usr/bin/env python3
"""Freeze authored x64-to-native Linux AArch64 portable x87 translation.

Record actual JITLink input objects, full serialized architectural states,
production-session agreement and strict regression results. This is execution
qualification for the explicit profile, not arbitrary-program equivalence.
"""

import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import time
import xml.etree.ElementTree as ET

from check_x87_native_regression import git, linked_libraries, sha256, source_snapshot, ROOT


EXPECTED_TESTS = {
    "X87Translation.EveryControlFlowBlockCanBePublishedWithoutExecution",
    "X87Translation.ObjectAndGraphBindExplicitProfileAndFirstInstructionBoundary",
    "X87Translation.RefusesMissingStateUnsupportedFormAndForgedAddressSpace",
    "X87Translation.RefusesUnqualifiedHostPolicyAndProfile",
    "X87Translation.PureIntegerArtifactsStillHaveDifferentRuntimeAndCacheIdentity",
    "X87Translation.ResetObjectsRetainTheInstructionAndOwnedProfile",
    "X87TranslationNative.IntegerInvocationCannotEnterLargerProfileEvenWithoutX87Calls",
    "X87TranslationNative.EveryTopAndPrecisionRoundingRetainsExactMemoryAndState",
    "X87TranslationNative.RefusalsAndMemoryFaultRetainCommittedPrefix",
    "X87TranslationNative.ExecutableStoreCommitsOnlyUnderInvalidatePolicy",
    "X87TranslationNative.MixedUnsupportedFormStopsAfterTheLastCommittedBlock",
    "X87TranslationNative.BranchesAndBackedgesPreserveTheOwnedStack",
    "X87TranslationNative.BudgetStopAndRestoreKeepExplicitState",
    "X87TranslationNative.FrozenObjectsMatchProductionSessionsAcrossControls",
    "X87TranslationNative.FnopObjectsAndSessionsRetainStateAndRefusedPrefix",
    "X87TranslationNative.EmmsObjectsAndSessionsRetainStateAndRefusedPrefix",
    "X87TranslationNative.FemmsObjectsAndSessionsRetainStateAndRefusedPrefix",
}


def audit_tests(path):
    seen = set()
    for case in ET.parse(path).iter("testcase"):
        name = case.get("classname") + "." + case.get("name")
        if name in seen or name not in EXPECTED_TESTS:
            raise ValueError("duplicate or unexpected translation test")
        if (case.get("status") != "run" or case.get("result") != "completed" or
                case.findall("failure") or case.findall("skipped")):
            raise ValueError("translation test did not execute successfully")
        seen.add(name)
    if seen != EXPECTED_TESTS:
        raise ValueError("missing translation test")
    return len(seen)


def audit_observations(observations):
    expected = {(f"exact-{top}-{pc}-{rc}", level)
                for top in range(8) for pc in (0, 2, 3) for rc in range(4)
                for level in (0, 2)}
    seen, objects, canonical = set(), set(), {}
    guest_bytes = ["48C7C000000000DB2F", "DB2E", "DEC1", "DB3A", "DFE0", "C3"]
    for row in observations:
        key = row["case"], row["level"]
        if key not in expected or key in seen:
            raise ValueError("unexpected or duplicate observation")
        seen.add(key)
        if (row["exit_reason"] != 1 or row["guest_instructions"] != 7 or
                row["blocks_translated"] != 6 or row["generated_bytes"] <= 0):
            raise ValueError("unexpected session completion")
        if row["after"] != row["session_after"] or not bytes.fromhex(row["before"]):
            raise ValueError("direct invocation and session state disagree")
        if len(row["blocks"]) != 6:
            raise ValueError("missing native block observation")
        states = [row["before"], row["after"]]
        for index, block in enumerate(row["blocks"]):
            name = f'{row["case"]}-O{row["level"]}-block-{index}.o'
            if block["object"] != name or name in objects:
                raise ValueError("duplicate or unexpected object")
            objects.add(name)
            if (block["guest_bytes"] != guest_bytes[index] or
                    block["status"] != (260 if index == 5 else 256) or
                    block["audit_stages"] != 511 or not block["registry"] or
                    not block["artifact_identity"] or
                    block["attempted"] is not (index != 5) or
                    block["completed"] is not (index != 5) or
                    block["portable_status"] != (1 if index == 5 else 0)):
                raise ValueError("incomplete audit or wrong terminal block protocol")
            if not bytes.fromhex(block["after"]):
                raise ValueError("empty architectural state")
            states.append(block["after"])
        if states[-1] != row["after"]:
            raise ValueError("final block state differs from observation")
        if row["case"] in canonical and canonical[row["case"]] != states:
            raise ValueError("LLVM optimization levels disagree")
        canonical[row["case"]] = states
    if seen != expected:
        raise ValueError("missing native control observation")
    return {"cases": len(canonical), "observations": len(seen), "objects": len(objects),
            "session_and_block_agreement": True, "optimization_agreement": True}, objects


def audit_reset_observations(observations):
    expected = {(kind, case, level) for kind in range(3)
                for case in range(75) for level in (0, 2)}
    seen, objects, canonical = set(), set(), {}
    for row in observations:
        key = row["kind"], row["case"], row["level"]
        if key not in expected or key in seen:
            raise ValueError("unexpected or duplicate reset observation")
        seen.add(key)
        kind, case, level = key
        refused = case >= 72
        block_count = 1 if refused else 2
        if (row["exit_reason"] != (6 if refused else 1) or
                row["guest_instructions"] != (2 if refused else 3) or
                row["blocks_translated"] != block_count or row["generated_bytes"] <= 0):
            raise ValueError("unexpected reset session completion")
        if row["after"] != row["session_after"] or not bytes.fromhex(row["before"]):
            raise ValueError("reset direct invocation and session state disagree")
        if len(row["blocks"]) != block_count:
            raise ValueError("missing reset native block observation")
        states = [row["before"], row["after"]]
        for index, block in enumerate(row["blocks"]):
            name = f"reset-{kind}-{case}-O{level}-block-{index}.o"
            if block["object"] != name or name in objects:
                raise ValueError("duplicate or unexpected reset object")
            objects.add(name)
            code = "C3" if index else "48C7C022110000" + ("D9D0", "0F77", "0F0E")[kind]
            portable_status = 1 if index else (2, 6, 5)[case - 72] if refused else 0
            if (block["guest_bytes"] != code or
                    block["entry_pc"] != ("4009" if index else "4000") or
                    block["status"] != (261 if refused else 260 if index else 256) or
                    block["audit_stages"] != 511 or not block["registry"] or
                    not block["artifact_identity"] or
                    block["attempted"] is not (index == 0) or
                    block["completed"] is not (index == 0 and not refused) or
                    block["portable_status"] != portable_status):
                raise ValueError("incomplete reset audit or wrong terminal block protocol")
            if not bytes.fromhex(block["after"]):
                raise ValueError("empty reset architectural state")
            states.append(block["after"])
        if states[-1] != row["after"]:
            raise ValueError("final reset block state differs from observation")
        identity = kind, case
        if identity in canonical and canonical[identity] != states:
            raise ValueError("reset LLVM optimization levels disagree")
        canonical[identity] = states
    if seen != expected:
        raise ValueError("missing reset state observation")
    return {"cases": len(canonical), "observations": len(seen), "objects": len(objects),
            "refusals": 18, "session_and_block_agreement": True,
            "optimization_agreement": True}, objects


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    if platform.system() != "Linux" or platform.machine() != "aarch64":
        parser.error("native Linux AArch64 is required; emulated or skipped execution does not qualify")
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    driver, build = args.driver.resolve(strict=True), args.build_dir.resolve(strict=True)
    record = {"protocol": "neverd-x87-linux-aarch64-translation-v1", "passed": False,
              "scope": "authored source-x64 owned portable state and Linux AArch64 ELF translation",
              "exclusions": ["i386 translation", "non-Linux execution", "source floating ABI",
                             "general environment/MMX operations", "formal equivalence", "speed claims"],
              "environment": {"platform": platform.platform(), "machine": platform.machine()},
              "script_sha256": sha256(Path(__file__))}
    started = time.monotonic()
    try:
        record["cpu"] = json.loads(subprocess.check_output(["lscpu", "--json"]))
        record["git_head"] = git("rev-parse", "HEAD").decode().strip()
        record["submodules"] = git("submodule", "status").decode().splitlines()
        patch, untracked = source_snapshot()
        (output / "source.patch").write_bytes(patch)
        record["untracked_source_sha256"] = untracked
        for name in untracked:
            destination = output / "untracked-source" / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, destination)
        receipt = json.loads((build / "neverd-build-receipt.json").read_text())
        if receipt["status"] != "success":
            raise ValueError("normal build wrapper has no successful receipt")
        bindings = {str(driver): sha256(driver), **linked_libraries(driver)}
        for name in ("neverd-build-receipt.json", "neverd-dependency-inputs.json", "CMakeCache.txt"):
            path = build / name
            bindings[str(path)] = sha256(path)
            shutil.copyfile(path, output / name)
        record["input_sha256"] = bindings
        xml, artifacts = output / "results.xml", output / "artifacts"
        command = [str(driver), "--gtest_output=xml:" + str(xml)]
        record["command"] = command
        with (output / "test.log").open("wb") as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=1800,
                env=dict(os.environ, NEVERD_X87_TRANSLATION_ARTIFACT_DIR=str(artifacts)), check=False)
        record["returncode"] = result.returncode
        if result.returncode:
            raise ValueError("native translation fixture driver failed")
        record["tests"] = audit_tests(xml)
        observations = [json.loads(line) for line in (artifacts / "observations.jsonl").read_text().splitlines()]
        record["coverage"], objects = audit_observations(observations)
        resets = [json.loads(line) for kind in range(3)
                  for line in (artifacts / f"reset-observations-{kind}.jsonl").read_text().splitlines()]
        record["reset_coverage"], reset_objects = audit_reset_observations(resets)
        objects |= reset_objects
        if {path.name for path in artifacts.glob("*.o")} != objects:
            raise ValueError("missing or unexpected native object")
        for name in objects:
            data = (artifacts / name).read_bytes()
            if len(data) < 64 or data[:6] != b"\x7fELF\x02\x01" or int.from_bytes(data[18:20], "little") != 183:
                raise ValueError("executed object is not AArch64 ELF64 little endian")
        record["stable_inputs"] = (
            all(sha256(Path(name)) == digest for name, digest in bindings.items()) and
            git("rev-parse", "HEAD").decode().strip() == record["git_head"] and
            git("submodule", "status").decode().splitlines() == record["submodules"] and
            source_snapshot() == (patch, untracked))
        if not record["stable_inputs"]:
            raise ValueError("source, runtime or build inputs changed during recording")
        record["passed"] = True
    except (OSError, subprocess.SubprocessError, ValueError, KeyError, TypeError, ET.ParseError) as error:
        record["error"] = str(error)
    record["elapsed_seconds"] = time.monotonic() - started
    record["files"] = {str(path.relative_to(output)): {"bytes": path.stat().st_size, "sha256": sha256(path)}
                       for path in sorted(output.rglob("*")) if path.is_file()}
    (output / "record.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps({key: record.get(key) for key in ("passed", "coverage", "error")}, indent=2))
    return 0 if record["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
