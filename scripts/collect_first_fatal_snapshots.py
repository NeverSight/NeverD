#!/usr/bin/env python3
"""Retain original outcomes and graph snapshots for six already-known failures."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import time
import subprocess
import sys
import xml.etree.ElementTree as ET


if __package__:
    from .audit_ci_test_results import parse_junit
else:
    from audit_ci_test_results import parse_junit


CASES = {
    "Linux": (
        ("OptStress105/X64OptStress105RT.Verify/x64o105_kmp", "NeverDSemanticTests"),
        ("OptStress105/X86OptStress105RT.Verify/x86o105_kmp", "NeverDSemanticTests"),
    ),
    "macOS": (
        ("SwXform4/X86SwXformRT.Verify/x86sww_deepnest", "NeverDSwitchXformTests"),
        ("SwXform4/X86SwXformRT.Verify/x86sww_chained", "NeverDSwitchXformTests"),
    ),
    "Windows": (
        ("SwXform4/X86SwXformRT.Verify/x86sww_deepnest", "NeverDSwitchXformTests"),
        ("SwXform4/X86SwXformRT.Verify/x86sww_chained", "NeverDSwitchXformTests"),
    ),
}


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")


def read_json(path: Path) -> object:
    return json.loads(path.read_bytes())


def file_record(path: Path) -> dict:
    data = path.read_bytes()
    return {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def properties(test: dict) -> dict:
    values = test.get("properties", [])
    result = {item["name"]: item["value"] for item in values}
    if len(result) != len(values):
        raise ValueError("duplicate CTest properties")
    return result


def command_filter(test: dict, owner: str) -> str | None:
    # Accept the two observed CTest launch forms without replacing either one.
    # The executable and the sole filter must both belong to the selected test.
    command = test.get("command", [])
    if not isinstance(command, list) or not command or not all(isinstance(arg, str) for arg in command):
        return None

    def filename(path: str) -> str:
        return path.replace("\\", "/").rsplit("/", 1)[-1]

    def owns_executable(path: str) -> bool:
        return filename(path) in (owner, owner + ".exe")

    if owns_executable(command[0]):
        filters = [arg.removeprefix("--gtest_filter=") for arg in command[1:]
                   if arg.startswith("--gtest_filter=")]
        remaining = [arg for arg in command[1:] if not arg.startswith("--gtest_filter=")]
        if len(filters) == 1 and filters[0] and remaining in ([], ["--gtest_also_run_disabled_tests"]):
            return filters[0]
        return None

    # CMake 4.4 on the actual Windows runner uses GoogleTest/LaunchTest.cmake
    # with separate -D argument pairs. Unknown/duplicate definitions, alternate
    # executors or extra arguments are not evidence of this observed contract.
    if (filename(command[0]) not in ("cmake", "cmake.exe") or len(command) != 13 or
            command[-2] != "-P" or
            not command[-1].replace("\\", "/").endswith("/Modules/GoogleTest/LaunchTest.cmake")):
        return None
    definitions = {}
    for index in range(1, len(command) - 2, 2):
        if command[index] != "-D" or "=" not in command[index + 1]:
            return None
        key, value = command[index + 1].split("=", 1)
        if key in definitions:
            return None
        definitions[key] = value
    if set(definitions) != {"TEST_EXECUTABLE", "TEST_EXECUTOR", "TEST_FILTER",
                            "TEST_XML_OUTPUT", "TEST_EXTRA_ARGS"}:
        return None
    if (not owns_executable(definitions["TEST_EXECUTABLE"]) or
            any(definitions[key] for key in ("TEST_EXECUTOR", "TEST_XML_OUTPUT", "TEST_EXTRA_ARGS"))):
        return None
    return definitions["TEST_FILTER"] or None


def check_capture(folder: Path, function: str, outcome: str) -> tuple[list, list]:
    errors = []
    snapshots = []
    fixture = folder / "fixture"
    retained = read_json(fixture / "retention.json")
    if retained != {
        "schema": 1, "compile_succeeded": True, "source_written": True,
        "object_written": True, "function_written": True, "command_written": True,
    }:
        errors.append("fixture retention is incomplete")
    if (fixture / "function.txt").read_bytes() != function.encode("ascii"):
        errors.append("fixture function identity differs")
    original = file_record(fixture / "input.o")
    write_json(folder / "fixture-files.json", {
        name: file_record(fixture / name)
        for name in ("input.o", "source.c", "function.txt", "compile-command.txt", "retention.json")
    })
    for directory in sorted(folder.glob("snapshot-*")):
        item = {"directory": directory.name, "errors": []}
        snapshots.append(item)
        try:
            manifest = read_json(directory / "capture.json")
            item["manifest"] = manifest
            if manifest["schema"] != 1 or manifest["function"] != function:
                raise ValueError("snapshot schema/function identity differs")
            if manifest["scope"] != "first-fatal-post-abi-graph":
                raise ValueError("unexpected snapshot scope")
            if any(manifest[key] is not False for key in ("complete_ir_model", "proof_history", "low_ir", "operation_locator")):
                raise ValueError("snapshot overstated its evidence scope")
            for key in ("graph_complete", "graph_written", "raw_written"):
                if manifest[key] is not True:
                    item["errors"].append(f"{key} is not true")
            loaded = file_record(directory / "loaded-image.raw")
            item["loaded_image"] = loaded
            item["fixture_object"] = original
            item["loaded_raw_equals_fixture_object"] = (
                (directory / "loaded-image.raw").read_bytes() == (fixture / "input.o").read_bytes()
            )
            if not item["loaded_raw_equals_fixture_object"]:
                item["errors"].append("loaded Raw differs from the retained original object")
            if loaded["bytes"] != manifest["raw_bytes"]:
                item["errors"].append("loaded Raw size differs from its manifest")
            graph_path = directory / "post-abi-graph.jsonl"
            item["graph_file"] = file_record(graph_path)
            if item["graph_file"]["bytes"] != manifest["graph_bytes"]:
                item["errors"].append("graph size differs from its manifest")
            rows = []
            for index, line in enumerate(graph_path.read_bytes().splitlines()):
                try:
                    rows.append(json.loads(line))
                except (ValueError, UnicodeError):
                    item["errors"].append(f"incomplete/invalid graph record at line {index + 1}")
                    break
            first = [r for r in rows if r.get("record") == "first-fatal"]
            funcs = [r for r in rows if r.get("record") == "backend-input-function"]
            if len(first) != 1 or len(funcs) != 1:
                raise ValueError("missing or duplicate first-fatal/function records")
            if first[0]["branch"] != manifest["branch"]:
                raise ValueError("first-fatal branch differs from manifest")
            if any(r["entry"] != manifest["entry"] for r in first + funcs):
                raise ValueError("selected function entry differs")
            if first[0]["function"] != function or funcs[0]["name"] != function:
                raise ValueError("selected graph function name differs")
            symbols = [r for r in rows if r.get("record") == "symbol" and r.get("function") == 1
                       and r.get("address") == manifest["entry"] and r.get("name") == function]
            item["matching_function_symbols"] = len(symbols)
            if len(symbols) != 1:
                item["errors"].append("selected function does not have one exact loaded symbol")
            item["parsed_graph_records"] = len(rows)
            item["files"] = {p.name: file_record(p) for p in directory.iterdir() if p.is_file()}
        except (OSError, ValueError, KeyError, TypeError) as error:
            item["errors"].append(str(error))
        errors.extend(f"{directory.name}: {error}" for error in item["errors"])
    if outcome == "failed" and not snapshots:
        errors.append("failed test produced no first-fatal snapshot")
    if outcome == "passed" and snapshots:
        errors.append("test passed despite a captured first-fatal branch")
    return snapshots, errors


def audit_execution(xml: ET.Element, raw: str, test: dict, gtest: str,
                    owner: str, exit_code: int) -> dict:
    # Reuse the existing authoritative CTest status/child/count contract.
    results = parse_junit(xml)
    if len(results) != 1 or results[0].test.name != test["name"]:
        raise ValueError("CTest XML did not report exactly the selected registration")
    result = results[0]
    if result.test.labels != frozenset((owner,)):
        raise ValueError("CTest XML owner differs")
    starts = re.findall(r"^(\d+): \[ RUN      \] (\S+)\r?$", raw, re.M)
    ends = re.findall(
        r"^(\d+): \[(       OK |  FAILED  |  SKIPPED )\] (\S+?)"
        r"(?:, where GetParam\(\) = (.*?))? \((\d+) ms\)\r?$", raw, re.M)
    errors = []
    if type(exit_code) is not int or not 0 <= exit_code <= 255:
        errors.append("CTest did not return a normal exit status")
    if result.outcome in ("passed", "failed", "skipped"):
        expected_mark = {"passed": "OK", "failed": "FAILED", "skipped": "SKIPPED"}[result.outcome]
        if len(starts) != 1 or starts[0][1] != gtest:
            errors.append("raw GTest start identity is not exact")
        if (len(ends) != 1 or ends[0][2] != gtest or
                ends[0][1].strip() != expected_mark or
                len(starts) != 1 or ends[0][0] != starts[0][0]):
            errors.append("raw GTest completion identity/state differs from XML")
    elif starts or ends:
        errors.append("nonexecuted XML state contradicts raw GTest execution")
    if result.outcome not in ("passed", "failed"):
        errors.append("selected registration was " + result.outcome)
    if result.outcome == "passed" and exit_code != 0:
        errors.append("passed XML and CTest exit status differ")
    if result.outcome == "failed" and exit_code == 0:
        errors.append("failed XML and CTest exit status differ")
    return {"semantic_outcome": result.outcome, "ctest_message": result.message, "errors": errors,
            "raw_starts": starts, "raw_completions": ends}


def terminate_group(process: subprocess.Popen) -> list[str]:
    errors = []
    if os.name == "nt":
        try:
            killed = subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                    timeout=10, check=False)
            if killed.returncode != 0 and process.poll() is None:
                errors.append("taskkill could not confirm process-tree termination")
        except (OSError, subprocess.TimeoutExpired) as error:
            errors.append(type(error).__name__ + " while terminating the process tree")
    else:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        except OSError as error:
            errors.append(str(error))
    if process.poll() is None:
        try:
            process.kill()
        except OSError as error:
            errors.append(str(error))
    return errors


def run_bounded(command: list[str], stdout, stderr, *, timeout: float,
                env: dict | None = None) -> dict:
    started = time.monotonic()
    result = {"timeout_seconds": timeout, "timed_out": False, "cleanup_errors": []}
    kwargs = {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == "nt" else {"start_new_session": True}
    process = subprocess.Popen(command, stdout=stdout, stderr=stderr, stdin=subprocess.DEVNULL,
                               env=env, **kwargs)
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        result["timed_out"] = True
        result["cleanup_errors"] = terminate_group(process)
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            result["cleanup_errors"].append("process did not terminate within cleanup deadline")
    result["returncode"] = process.returncode
    result["elapsed_seconds"] = time.monotonic() - started
    return result


def execution_timeout(test: dict) -> float:
    timeout = properties(test).get("TIMEOUT")
    if type(timeout) not in (int, float) or not 0 < timeout <= 600:
        raise ValueError("selected known registration lacks its bounded CTest timeout")
    # CTest retains its native timeout. The outer allowance only covers CTest
    # discovery/startup/reporting/cleanup if its own supervision stalls.
    return float(timeout) + 60


def collect_case(build: Path, folder: Path, test: dict, gtest: str, owner: str) -> dict:
    folder.mkdir()
    function = gtest.rsplit("/", 1)[1]
    record = {"ctest_name": test["name"], "gtest": gtest, "owner": owner,
              "function": function, "selected_test": test, "semantic_outcome": "unavailable",
              "commands": [], "errors": []}
    env = dict(os.environ, NEVERD_CI_FAILURE_SNAPSHOT_DIR=str(folder),
               NEVERD_CI_FAILURE_SNAPSHOT_FUNCTION=function)
    command = ["ctest", "--test-dir", str(build), "--build-config", "Release",
               "--tests-regex", "^" + re.escape(test["name"]) + "$",
               "--label-regex", "^" + re.escape(owner) + "$", "--no-tests=error"]
    write_json(folder / "selection.json", record)
    try:
        discovery = command + ["--show-only=json-v1"]
        record["commands"].append(discovery)
        with (folder / "discovery.json").open("xb") as stdout, (folder / "discovery.stderr").open("xb") as stderr:
            result = run_bounded(discovery, stdout, stderr, timeout=60)
        record["discovery_supervision"] = result
        record["discovery_exit"] = result["returncode"]
        if result["timed_out"] or result["returncode"] != 0:
            raise ValueError("CTest discovery failed")
        selected = read_json(folder / "discovery.json")["tests"]
        if (len(selected) != 1 or
                any(selected[0].get(key) != test.get(key) for key in ("name", "command", "config")) or
                properties(selected[0]) != properties(test)):
            raise ValueError("CTest selection does not exactly match the retained inventory row")
        execution = command + ["--parallel", "1", "--verbose", "--output-on-failure",
                               "--output-junit", str(folder / "results.xml"),
                               "--output-log", str(folder / "ctest.log")]
        record["commands"].append(execution)
        with (folder / "console.log").open("xb") as output:
            result = run_bounded(execution, output, subprocess.STDOUT,
                                 env=env, timeout=execution_timeout(test))
        record["execution_supervision"] = result
        record["ctest_exit"] = result["returncode"]
        (folder / "ctest-exit-status.txt").write_text(
            (str(result["returncode"]) if result["returncode"] is not None else "unavailable") + "\n",
            encoding="ascii")
        if result["timed_out"]:
            record["semantic_outcome"] = "supervisor_timeout"
            raise ValueError("CTest exceeded its bounded outer supervisor; native outcome incomplete")
        xml = ET.parse(folder / "results.xml").getroot()
        raw = (folder / "ctest.log").read_text(encoding="utf-8", errors="replace")
        execution_result = audit_execution(xml, raw, test, gtest, owner, result["returncode"])
        outcome = execution_result["semantic_outcome"]
        record["semantic_outcome"] = outcome
        record["execution_audit"] = execution_result
        record["errors"].extend(execution_result["errors"])
        snapshots, errors = check_capture(folder, function, outcome)
        record["snapshots"] = snapshots
        record["errors"].extend(errors)
    except (OSError, ValueError, KeyError, TypeError, ET.ParseError) as error:
        record["errors"].append(str(error))
    write_json(folder / "outcome.json", record)
    return record


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    runner = os.environ.get("RUNNER_OS", "")
    report = {"schema": 1, "head_sha": os.environ.get("GITHUB_SHA"),
              "run_id": os.environ.get("GITHUB_RUN_ID"),
              "run_attempt": os.environ.get("GITHUB_RUN_ATTEMPT"),
              "runner_os": runner, "cases": [], "errors": []}
    try:
        inventory = read_json(args.inventory)
        write_json(output / "inventory.json", inventory)
        report["inventory_file"] = file_record(args.inventory)
        for gtest, owner in CASES[runner]:
            candidates = [t for t in inventory["tests"] if command_filter(t, owner) == gtest
                          and properties(t).get("LABELS") == [owner] and t.get("config") == "Release"]
            if len(candidates) != 1:
                report["errors"].append(f"{gtest}: expected exactly one owned Release registration")
                continue
            report["cases"].append(collect_case(args.build_dir.resolve(),
                                               output / gtest.rsplit("/", 1)[1],
                                               candidates[0], gtest, owner))
    except (OSError, ValueError, KeyError, TypeError) as error:
        report["errors"].append(str(error))
    complete = len(report["cases"]) == 2 and not report["errors"] and all(
        not case["errors"] for case in report["cases"])
    report["capture_review_complete"] = complete
    report["semantic_all_passed"] = len(report["cases"]) == 2 and all(
        case["semantic_outcome"] == "passed" for case in report["cases"])
    write_json(output / "summary.json", report)
    print(json.dumps({key: report[key] for key in
                      ("runner_os", "capture_review_complete", "semantic_all_passed", "errors")}))
    # Preserved native failures keep the Actions step failed, even when their
    # diagnostics were captured successfully. Capture is never semantic repair.
    codes = [case.get("ctest_exit", 1) for case in report["cases"]]
    return max([code if type(code) is int and 0 <= code <= 255 else 1 for code in codes] +
               [0 if complete and report["semantic_all_passed"] else 1])


if __name__ == "__main__":
    sys.exit(main())
