"""Execute the complete CTest inventory in bounded GoogleTest method processes.

Only the native suites' explicit GoogleTest contract is supported. Unknown
commands or CTest properties fail before execution instead of being ignored.
Original child XML, logs, commands and process status remain the evidence.
"""

from collections import Counter
import json
import math
import os
from pathlib import Path
import re
import signal
import subprocess
import xml.etree.ElementTree as ET

if __package__:
    from .audit_ci_test_inventory import parse_inventory
    from .audit_ci_test_results import TestOutcome
else:
    from audit_ci_test_inventory import parse_inventory
    from audit_ci_test_results import TestOutcome


def google_test_command(command):
    # CMake 4.3+ launches discovered GoogleTests through LaunchTest.cmake.
    # Only its plain native form is equivalent to our direct invocation:
    # https://github.com/Kitware/CMake/blob/v4.3.0/Modules/GoogleTest/LaunchTest.cmake
    if (len(command) == 13 and Path(command[0]).name == "cmake"
            and command[-2] == "-P"
            and Path(command[-1]).parts[-3:] == ("Modules", "GoogleTest", "LaunchTest.cmake")
            and command[1:11:2] == ["-D"] * 5):
        fields = [value.partition("=") for value in command[2:11:2]]
        variables = {name: value for name, _, value in fields}
        required = {"TEST_EXECUTABLE", "TEST_EXECUTOR", "TEST_FILTER",
                    "TEST_XML_OUTPUT", "TEST_EXTRA_ARGS"}
        if (set(variables) != required or any(separator != "=" for _, separator, _ in fields)
                or any(variables[name] for name in
                       ("TEST_EXECUTOR", "TEST_XML_OUTPUT", "TEST_EXTRA_ARGS"))):
            raise ValueError("unsupported CMake GoogleTest launcher configuration")
        command = [variables["TEST_EXECUTABLE"],
                   "--gtest_filter=" + variables["TEST_FILTER"],
                   "--gtest_also_run_disabled_tests"]
    if (len(command) != 3 or not Path(command[0]).is_absolute()
            or not command[1].startswith("--gtest_filter=")
            or command[2] != "--gtest_also_run_disabled_tests"):
        raise ValueError("unsupported GoogleTest command")
    return command


def method_inventory(document):
    records = parse_inventory(document)
    if len(set(records)) != len(records):
        raise ValueError("duplicate CTest identity")
    methods, identities = {}, set()
    allowed = {"ENVIRONMENT", "LABELS", "SKIP_REGULAR_EXPRESSION",
               "TIMEOUT", "WORKING_DIRECTORY"}
    for raw, record in zip(document["tests"], records, strict=True):
        command = google_test_command(raw.get("command", []))
        name = command[1].removeprefix("--gtest_filter=")
        if not re.fullmatch(r"[A-Za-z0-9_./]+\.[A-Za-z0-9_/]+", name):
            raise ValueError("GoogleTest filter must name one exact case")
        identity = (command[0], name)
        if identity in identities:
            raise ValueError("duplicate GoogleTest command identity")
        identities.add(identity)
        properties = {p["name"]: p["value"] for p in raw["properties"]}
        if len(properties) != len(raw["properties"]):
            raise ValueError("duplicate CTest properties: " + record.name)
        # CMake 4.3 discovery also publishes the diagnostic source location.
        # This metadata changes neither the command nor its execution policy.
        if "DEF_SOURCE_LINE" in properties:
            source = properties.pop("DEF_SOURCE_LINE")
            if not isinstance(source, str) or not re.fullmatch(r".+:[1-9][0-9]*", source):
                raise ValueError("invalid CTest definition source location")
        if set(properties) != allowed:
            raise ValueError("unsupported CTest properties: " + record.name)
        if properties["SKIP_REGULAR_EXPRESSION"] != [r"\[  SKIPPED \]"]:
            raise ValueError("unsupported CTest skip policy")
        timeout = properties["TIMEOUT"]
        if not isinstance(timeout, (int, float)) or not math.isfinite(timeout) or timeout <= 0:
            raise ValueError("invalid CTest timeout")
        directory = properties["WORKING_DIRECTORY"]
        if not isinstance(directory, str) or not Path(directory).is_absolute():
            raise ValueError("invalid CTest working directory")
        environment = properties["ENVIRONMENT"]
        if (not isinstance(environment, list)
                or any(not isinstance(item, str) or "=" not in item or "\0" in item
                       or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", item.split("=", 1)[0])
                       for item in environment)
                or len({item.split("=", 1)[0] for item in environment}) != len(environment)):
            raise ValueError("invalid CTest environment")
        suite, case = name.split(".", 1)
        family = suite + "." + case.split("/", 1)[0]
        key = (command[0], family, directory, tuple(environment), timeout)
        methods.setdefault(key, {})[name] = record
    return methods


def read_results(path, expected):
    root = ET.parse(path).getroot()
    if root.tag != "testsuites":
        raise ValueError("expected original GoogleTest XML")
    actual, outcomes = set(), []
    for case in root.iter("testcase"):
        name = case.attrib["classname"] + "." + case.attrib["name"]
        if name in actual or name not in expected:
            raise ValueError("unexpected or duplicate GoogleTest result: " + name)
        actual.add(name)
        failures, skips = case.findall("failure"), case.findall("skipped")
        if failures and skips or case.find("error") is not None:
            raise ValueError("contradictory GoogleTest result: " + name)
        status, result = case.get("status"), case.get("result")
        if status == "run" and result == "completed" and not skips:
            outcome = "failed" if failures else "passed"
        elif status == "run" and result == "skipped" and len(skips) == 1 and not failures:
            outcome = "skipped"
        else:
            raise ValueError("incomplete GoogleTest result: " + name)
        message = "\n".join(e.get("message", "") for e in failures + skips)
        outcomes.append(TestOutcome(expected[name], outcome, message))
    counts = Counter(case.outcome for case in outcomes)
    if actual != set(expected):
        raise ValueError("missing GoogleTest results: " + str(sorted(set(expected) - actual)))
    for attribute, total in {"tests": len(actual), "failures": counts["failed"],
                             "disabled": 0, "errors": 0}.items():
        if root.get(attribute) != str(total):
            raise ValueError("GoogleTest XML count disagrees: " + attribute)
    return outcomes


def execute(command, directory, environment, timeout, evidence):
    evidence.mkdir(parents=True, exist_ok=False)
    status, timed_out, retired = None, False, True
    with (evidence / "output.log").open("w") as output:
        child = subprocess.Popen(command, cwd=directory, env=environment,
                                 stdin=subprocess.DEVNULL, stdout=output,
                                 stderr=subprocess.STDOUT, close_fds=True,
                                 start_new_session=True)
        try:
            status = child.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            try:
                os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                status = child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                retired = False
    result = {"command": command, "working_directory": directory,
              "timeout_seconds": timeout, "status": status,
              "timed_out": timed_out, "child_retired": retired}
    (evidence / "status.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def run_methods(document, evidence, environment):
    if os.name != "posix":
        raise ValueError("method process isolation requires POSIX process groups")
    methods = method_inventory(document)
    outcomes, errors = [], []
    destination = evidence / "methods"
    destination.mkdir(parents=True, exist_ok=False)
    for index, (key, expected) in enumerate(methods.items()):
        binary, family, directory, variables, case_timeout = key
        child_environment = dict(environment)
        child_environment.update(item.split("=", 1) for item in variables)
        for required in ("NEVERD_REQUIRE_HVF", "NEVERD_REQUIRE_NATIVE_WHP"):
            if environment.get(required) == "1" and child_environment.get(required) != "1":
                raise ValueError("test environment disables required native coverage")
        part = destination / f"{index:04d}"
        command = [binary, "--gtest_filter=" + ":".join(expected),
                   "--gtest_also_run_disabled_tests",
                   "--gtest_output=xml:" + str(part / "results.xml")]
        print(f"[{index + 1}/{len(methods)}] {Path(binary).name} {family} ({len(expected)} cases)",
              flush=True)
        # Bound the entire method by its aggregate CTest allowance and 120 seconds.
        # GoogleTest does not enforce separate per-case timeouts in this mode.
        status = execute(command, directory, child_environment,
                         min(120, case_timeout * len(expected)), part)
        try:
            cases = read_results(part / "results.xml", expected)
            outcomes.extend(cases)
            print(dict(Counter(case.outcome for case in cases)), flush=True)
        except (OSError, ValueError, KeyError, ET.ParseError) as error:
            errors.append(family + ": " + str(error))
        if status["status"] != 0 or status["timed_out"] or not status["child_retired"]:
            errors.append(family + ": child execution failed or incomplete")
        (part / "identities.json").write_text(json.dumps({
            name: {"name": record.name, "labels": sorted(record.labels)}
            for name, record in expected.items()}, indent=2) + "\n")
        if not status["child_retired"]:
            break
    report = {"execution": "gtest-methods", "errors": errors, "results": [
        {"name": case.test.name, "labels": sorted(case.test.labels),
         "outcome": case.outcome, "message": case.message} for case in outcomes]}
    (evidence / "method-results.json").write_text(json.dumps(report, indent=2) + "\n")
    return outcomes, int(bool(errors))
