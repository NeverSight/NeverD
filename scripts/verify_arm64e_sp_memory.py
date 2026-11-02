#!/usr/bin/env python3
"""Verify actual raw-SP outputs against native exceptions and GPR controls."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import time


ROOT = Path(__file__).resolve().parent
CONFIG = json.loads(Path(__file__).with_suffix(".def").read_text())


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_report(report, kind):
    expected = CONFIG["test_reports"][kind]
    cases = []
    exports = []
    valid = (report.get("tests") == len(expected["cases"])
             and report.get("failures") == 0 and report.get("errors") == 0)
    for suite in report.get("testsuites", []):
        for case in suite.get("testsuite", []):
            cases.append(suite["name"] + "." + case["name"])
            valid = (valid and case.get("status") == "RUN"
                     and case.get("result") == "COMPLETED"
                     and not case.get("failures"))
            if expected["artifact_property"] in case:
                exports.append(case[expected["artifact_property"]])
    if (not valid or sorted(cases) != sorted(expected["cases"])
            or len(exports) != 1 or not isinstance(exports[0], str)
            or not exports[0]):
        raise ValueError(CONFIG["errors"]["report"])
    return exports[0]


def execute(command, directory, name, timeout, env):
    (directory / (name + "-command.json")).write_text(
        json.dumps(command, indent=2) + "\n")
    try:
        result = subprocess.run(command, env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=timeout, check=False)
    except subprocess.TimeoutExpired as error:
        output = error.stdout or b""
        if isinstance(output, bytes):
            output = output.decode(errors="replace")
        (directory / (name + ".log")).write_text(output)
        raise
    (directory / (name + ".log")).write_text(result.stdout)
    result.check_returncode()
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stack-artifacts", required=True, type=Path)
    parser.add_argument("--control-artifacts", type=Path)
    parser.add_argument("--stack-report", required=True, type=Path)
    parser.add_argument("--control-report", type=Path)
    parser.add_argument("--neverd-revision", required=True)
    parser.add_argument("--sdk", required=True, type=Path)
    parser.add_argument("--compiler", default=CONFIG["compiler"])
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--profile", default=CONFIG["default_profile"],
                        choices=CONFIG["profiles"])
    args = parser.parse_args()
    profile = CONFIG["profiles"][args.profile]
    if ("control" in profile["test_reports"]
            and (args.control_artifacts is None or args.control_report is None)):
        parser.error(CONFIG["errors"]["report"])
    if [platform.system(), platform.machine()] != CONFIG["required_host"]:
        parser.error(CONFIG["errors"]["host"])
    if (len(args.neverd_revision) != CONFIG["revision_hex_length"] or
            any(c not in CONFIG["revision_hex_digits"] for c in args.neverd_revision)):
        parser.error(CONFIG["errors"]["revision"])
    output = args.output.resolve()
    output.mkdir()
    env = dict(os.environ, SDKROOT=str(args.sdk.resolve()))
    env.pop("CCC_OVERRIDE_OPTIONS", None)
    compiler = shutil.which(args.compiler)
    if compiler is None:
        raise FileNotFoundError(args.compiler)
    flags = [*CONFIG["flags"], *profile["flags"],
             CONFIG["sdk_flag"], str(args.sdk.resolve())]
    summary = {"passed": False, "neverd_revision": args.neverd_revision,
               "scope": profile["scope"], "profile": args.profile,
               "native_apple_cpta_qualified": False,
               "stack_abi_qualified": False, "inputs": {}, "results": []}
    summary["verifier_sha256"] = {
        path.name: digest(path) for path in (
            Path(__file__), Path(__file__).with_suffix(".def"),
            ROOT / CONFIG["fault_source"], ROOT / CONFIG["fault_header"])
    }
    summary["sdk"] = str(args.sdk.resolve())
    summaries = output / "summary.json"

    def save():
        summaries.write_text(json.dumps(summary, indent=2) + "\n")

    save()
    execute([compiler, "--version"], output, "compiler-version",
            CONFIG["compile_timeout_seconds"], env)
    inputs_to_check = [("stack", args.stack_artifacts, args.stack_report)]
    if "control" in profile["test_reports"]:
        inputs_to_check.append(("control", args.control_artifacts, args.control_report))
    for kind, artifacts, test_report in inputs_to_check:
        origin = validate_report(json.loads(test_report.read_text()),
                                 profile["test_reports"][kind])
        work = output / kind
        inputs = work / "inputs"
        inputs.mkdir(parents=True)
        shutil.copy2(test_report, inputs / "tests.json")
        for name in CONFIG["input_files"]:
            shutil.copy2(artifacts / name, inputs / name)
        summary["inputs"][kind] = {
            "export_origin": origin,
            "sha256": {p.name: digest(p) for p in inputs.iterdir()},
        }
        harness = (inputs / CONFIG["harness_file"]).read_text()
        if harness.count(CONFIG["main_token"]) != 1:
            raise ValueError(CONFIG["errors"]["harness"])
        if kind == "stack":
            fault = ROOT / CONFIG["fault_source"]
            header = ROOT / CONFIG["fault_header"]
            shutil.copy2(header, work / header.name)
            harness = harness[:harness.index(CONFIG["main_token"])] + fault.read_text()
        else:
            header = ROOT / CONFIG["fault_header"]
            shutil.copy2(header, work / header.name)
            harness = harness.replace(CONFIG["main_token"],
                                      CONFIG["control_inventory_check"] + CONFIG["main_token"])
            for original, replacement in CONFIG["control_replacements"]:
                if harness.count(original) != 1:
                    raise ValueError(CONFIG["errors"]["harness"])
                harness = harness.replace(original, replacement)
        source = work / CONFIG["harness_file"]
        source.write_text(harness)
        harness_object = work / "harness.o"
        execute([compiler, *flags, CONFIG["harness_optimization"], "-c",
                 str(source), "-o", str(harness_object)],
                work, "harness-compile", CONFIG["compile_timeout_seconds"], env)
        for route in CONFIG["routes"]:
            start = time.monotonic()
            directory = work / route["name"]
            directory.mkdir()
            projection = directory / "projection.o"
            if route["compile_c"]:
                execute([compiler, *flags, route["optimization"], "-c",
                         str(inputs / route["file"]), "-o", str(projection)],
                        directory, "projection-compile", CONFIG["compile_timeout_seconds"], env)
            else:
                shutil.copy2(inputs / route["file"], projection)
            executable = directory / "verify"
            execute([compiler, *flags, str(projection), str(harness_object),
                     "-o", str(executable)], directory, "link",
                    CONFIG["compile_timeout_seconds"], env)
            result = execute([str(executable)], directory, "execute",
                             CONFIG["execution_timeout_seconds"], env)
            observation = json.loads(result.stdout) if kind == "stack" else {}
            if kind == "stack" and observation != profile["fault_observations"]:
                raise ValueError(CONFIG["errors"]["observations"])
            summary["results"].append({"kind": kind, "route": route["name"],
                                       "passed": True, "observation": observation,
                                       "seconds": round(time.monotonic() - start, 3)})
            save()
    summary["passed"] = True
    save()
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
