#!/usr/bin/env python3
"""Reconcile CTest outcomes with the audited CI inventory and execution policy.

The unit of evidence is a CTest registration, not each child test hidden inside
a Python or other aggregate runner. JUnit and the CTest exit status are both
required: CTest encodes some infrastructure errors as JUnit skipped elements.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

if __package__:
    from . import audit_ci_test_inventory as inventory
else:
    import audit_ci_test_inventory as inventory


# These contracts are already mandatory in CI or have no optional host backend.
# Requiring their labels also prevents a deleted suite from passing vacuously.
COMMON_REQUIRED_LABELS = frozenset(
    inventory.CORPUS_LABELS
    + inventory.SAFETY_LABELS
    + inventory.PLUGIN_LABELS
    + inventory.CONCOLIC_LABELS
    + inventory.INTEGRITY_LABELS
    + inventory.SOURCE_DIALECT_LABELS
)
# The Linux workflow installs these exact source/corpus/oracle dependencies.
LINUX_REQUIRED_LABELS = frozenset(
    {
        "NeverDSBFExternalOracleTests",
        "NeverDSBFUpstreamConformanceTests",
        "NeverDSBFAgaveConformanceTests",
    }
)
# Only these two corpus tests execute native Mach-O probes instead of reading
# portable corpus bytes. Match identity, host profile, and the original reason.
NON_APPLE_CORPUS_SKIPS = {
    inventory.TestRecord(
        "ObjCEHCorpus.HonorsHostMachORewriteContractForEveryVariant",
        frozenset({"NeverDObjCEHCorpusTests"}),
    ): "the committed Objective-C probes require their host ISA",
    inventory.TestRecord(
        "CxxItaniumEHCorpus.HonorsHostMachORewriteContractForEveryProbeVariant",
        frozenset({"NeverDCxxItaniumEHCorpusTests"}),
    ): "the committed Mach-O probes only run on their host ISA",
}
# These tests inspect a running ELF image or compile Linux-specific loader,
# glibc and System V inputs. Linux CI must execute them; other native profiles
# retain an explicit skip rather than claiming execution of a foreign runtime.
NON_LINUX_SESSION_SKIPS = {
    inventory.TestRecord("SessionCAPITest." + name,
                         frozenset({"NeverDSessionCAPITests"})): reason
    for name, reason in (
        ("BinaryFilesReadTheirProcessorFromTheirBytes",
         "the test reads code from a little-endian ELF64 host"),
        ("EveryFunctionOfALazilyBoundProgramShowsAsLLVMC", "builds a glibc program"),
        ("TLSDescriptorCallsShowAsCallsThroughTheDescriptor",
         "builds an x86-64 ELF shared object"),
        ("DebugParametersNameTheRegistersTheyArriveIn",
         "builds an x86-64 System V program"),
        ("StubsOfVariadicImportsPassTheirArgumentsOn", "builds a glibc program"),
        ("BothCBackendsAgreeWhichFunctionsReturnAValue", "builds a glibc program"),
        ("StartAloneCallsTheBoundStartupImportWithMain", "builds a glibc program"),
        ("SourcePagesNameTheSymbolsOfRenamedFunctions", "builds a glibc program"),
        ("FunctionsThatShareANameAreNumberedAsIDANumbersThem", "builds a glibc program"),
        ("AnalyzedLLVMPagesEmitTheirFunctionAlone", "builds a glibc program"),
        ("LLVMViewShowsTheFunctionInAModuleOfItsOwn", "builds an x86-64 glibc program"),
        ("LLVMViewMapsEachSpellingOfAMirrorToItsOwnOffset",
         "reads the engine's internal view of an LLVM function"),
        ("APLTEntryOfALibrarysOwnVariadicRoutineIsTheRoutine", "builds a glibc shared object"),
    )
}
# These four opt-in checks read developer-supplied Windows kernel/PDB files.
# Keep exact identities and reasons; a configured input cannot use this exception.
OPTIONAL_KERNEL_INPUT_SKIPS = {
    inventory.TestRecord(
        "SessionCAPIKernelInputs." + name,
        frozenset({"NeverDSessionCAPITests"}),
    ): (frozenset(inputs), reason)
    for name, inputs, reason in (
        ("TheKernelsTypesJoinTheHeadersOfItsDrivers",
         ("NEVERD_TYPES_NTOSKRNL", "NEVERD_TYPES_NTKRNLMP"),
         "set NEVERD_TYPES_NTOSKRNL and NEVERD_TYPES_NTKRNLMP to ntoskrnl.exe and ntkrnlmp.pdb"),
        ("KernelFunctionsDecompileWithTheirPrototypes",
         ("NEVERD_TYPES_NTOSKRNL", "NEVERD_TYPES_NTKRNLMP"),
         "set NEVERD_TYPES_NTOSKRNL and NEVERD_TYPES_NTKRNLMP to ntoskrnl.exe and ntkrnlmp.pdb"),
        ("KernelExportsTakeTheirPrototypesWithoutThePDB",
         ("NEVERD_TYPES_NTOSKRNL",), "set NEVERD_TYPES_NTOSKRNL to ntoskrnl.exe"),
        ("KernelFunctionsHoldTheRecordsTheyName",
         ("NEVERD_TYPES_NTOSKRNL", "NEVERD_TYPES_NTKRNLMP"),
         "set NEVERD_TYPES_NTOSKRNL and NEVERD_TYPES_NTKRNLMP to ntoskrnl.exe and ntkrnlmp.pdb"),
    )
}
OUTCOME_NAMES = ("passed", "failed", "skipped", "disabled", "not_run")


class ResultError(ValueError):
    """Missing, malformed, or contradictory execution evidence."""


@dataclass(frozen=True)
class TestOutcome:
    test: inventory.TestRecord
    outcome: str
    message: str = ""
    output: str = ""


def required_labels(profile: str) -> frozenset[str]:
    if profile not in inventory.PROFILE_HEAVY_OWNERS:
        raise ResultError(f"unknown CI test profile: {profile}")
    labels = COMMON_REQUIRED_LABELS | inventory.PROFILE_HEAVY_OWNERS[profile]
    if profile == "linux-semantic":
        labels |= LINUX_REQUIRED_LABELS
    return labels


def _count(element: ET.Element, attribute: str) -> int:
    value = element.get(attribute)
    if value is None or not re.fullmatch(r"0|[1-9][0-9]*", value):
        raise ResultError(f"JUnit {attribute!r} is not a nonnegative integer")
    return int(value)


def _labels(case: ET.Element) -> frozenset[str]:
    properties = case.findall("properties")
    if len(properties) > 1:
        raise ResultError("testcase has multiple properties elements")
    values = case.findall("./properties/property[@name='cmake_labels']")
    if not values:
        return frozenset()
    if len(values) != 1 or not values[0].get("value"):
        raise ResultError("testcase has malformed or duplicate cmake_labels")
    # CTest writes the sorted labels as a CMake list. NeverD target labels are
    # plain names; reject ambiguous escaping instead of guessing an identity.
    raw = values[0].get("value", "")
    if any(character in raw for character in "\\[]"):
        raise ResultError("cmake_labels contains unsupported list escaping")
    labels = raw.split(";")
    if any(not label for label in labels) or len(set(labels)) != len(labels):
        raise ResultError("testcase has empty or duplicate cmake_labels")
    return frozenset(labels)


def _parse_case(case: ET.Element) -> TestOutcome:
    name = case.get("name")
    if not name:
        raise ResultError("JUnit testcase has no non-empty name")
    allowed_children = {"properties", "system-out", "system-err", "failure", "skipped"}
    if any(child.tag not in allowed_children for child in case):
        raise ResultError(f"test {name!r} has an unsupported result element")
    failures, skips = case.findall("failure"), case.findall("skipped")
    if len(failures) > 1 or len(skips) > 1 or (failures and skips):
        raise ResultError(f"test {name!r} has contradictory result elements")
    status = case.get("status")
    expected_children = {
        "run": (0, 0),
        "fail": (1, 0),
        "notrun": (0, 1),
        "disabled": (0, 0),
    }
    if expected_children.get(status) != (len(failures), len(skips)):
        raise ResultError(
            f"test {name!r} has unknown or contradictory status {status!r}"
        )
    message = (failures or skips)[0].get("message", "") if failures or skips else ""
    outcome = {"run": "passed", "fail": "failed", "disabled": "disabled"}.get(status)
    if status == "notrun":
        explicit_skip = message == "SKIP_REGULAR_EXPRESSION_MATCHED" or re.fullmatch(
            r"SKIP_RETURN_CODE=(?:0|[1-9][0-9]{0,2})", message
        )
        if explicit_skip and message.startswith("SKIP_RETURN_CODE="):
            explicit_skip = int(message.split("=", 1)[1]) <= 255
        outcome = "skipped" if explicit_skip else "not_run"
    output = "\n".join(element.text or "" for element in case.findall("system-out"))
    return TestOutcome(
        inventory.TestRecord(name, _labels(case)), outcome, message, output
    )


def parse_junit(document: ET.Element) -> tuple[TestOutcome, ...]:
    if document.tag != "testsuite" or any(
        child.tag != "testcase" for child in document
    ):
        raise ResultError("input is not a CTest JUnit testsuite")
    outcomes = tuple(_parse_case(case) for case in document)
    identities = Counter(outcome.test for outcome in outcomes)
    if any(count != 1 for count in identities.values()):
        raise ResultError("JUnit contains duplicate testcase identities (name, labels)")
    actual = Counter(outcome.outcome for outcome in outcomes)
    expected = {
        "tests": len(outcomes),
        "failures": actual["failed"],
        "disabled": actual["disabled"],
        # CTest's XML total intentionally combines skips and infrastructure
        # not-run results. The report below keeps those separate.
        "skipped": actual["skipped"] + actual["not_run"],
    }
    for attribute, count in expected.items():
        if _count(document, attribute) != count:
            raise ResultError(
                f"JUnit {attribute} count disagrees with testcase outcomes"
            )
    if document.get("errors") is not None and _count(document, "errors") != 0:
        raise ResultError("CTest JUnit reports errors")
    return outcomes


def _identity(test: inventory.TestRecord) -> str:
    return f"{test.name} [{', '.join(sorted(test.labels))}]"


def prior_passes(
    records: Sequence[inventory.TestRecord],
    selected: Sequence[inventory.TestRecord],
    documents: Sequence[ET.Element],
) -> ET.Element:
    """Reuse only matching, consistently passed registrations from this job."""
    known = set(records)
    observations: dict[inventory.TestRecord, list[TestOutcome]] = {}
    cases: dict[inventory.TestRecord, ET.Element] = {}
    for document in documents:
        for result, case in zip(parse_junit(document), document, strict=True):
            if result.test not in known:
                raise ResultError(
                    "completed result has an unknown inventory identity: "
                    + _identity(result.test)
                )
            observations.setdefault(result.test, []).append(result)
            cases.setdefault(result.test, case)
    eligible = {test for test in selected if test in observations and all(
        result.outcome == "passed" for result in observations[test]
    )}
    # A focused binary and its aggregate can register the same name with
    # different owners. CTest excludes by name, so retain every selected owner
    # of that name together or leave all of them for the remaining execution.
    remaining_names = {test.name for test in selected if test not in eligible}
    reused = [test for test in selected
              if test in eligible and test.name not in remaining_names]
    root = ET.Element("testsuite", tests=str(len(reused)), failures="0",
                      disabled="0", skipped="0")
    for test in reused:
        if any(character in test.name for character in "\r\n"):
            raise ResultError("test name cannot be represented in a CTest name file")
        root.append(copy.deepcopy(cases[test]))
    return root


def combine_prior_passes(
    current: Sequence[TestOutcome], previous: Sequence[TestOutcome]
) -> tuple[TestOutcome, ...]:
    if any(result.outcome != "passed" for result in previous):
        raise ResultError("reused evidence contains an outcome other than passed")
    identities = [result.test for result in (*current, *previous)]
    if len(set(identities)) != len(identities):
        raise ResultError("fresh and reused results contain duplicate identities")
    return (*current, *previous)


def _preview(tests: Sequence[inventory.TestRecord]) -> str:
    return "; ".join(_identity(test) for test in tests[:10]) + (
        "; ..." if len(tests) > 10 else ""
    )


def audit_results(
    selected: Sequence[inventory.TestRecord],
    outcomes: Sequence[TestOutcome],
    profile: str,
    exit_status: int,
    configured_kernel_inputs: frozenset[str] = frozenset(),
) -> dict:
    mandatory = required_labels(profile)
    errors: list[str] = []
    expected = set(selected)
    actual = {outcome.test: outcome for outcome in outcomes}
    if not selected or len(expected) != len(selected):
        raise ResultError(
            "selected inventory is empty or contains duplicate identities"
        )
    if len(actual) != len(outcomes):
        raise ResultError("outcomes contain duplicate identities")
    if type(exit_status) is not int or not 0 <= exit_status <= 255:
        raise ResultError("CTest exit status is not an integer in 0..255")
    if exit_status:
        errors.append(f"CTest exited with status {exit_status}")
    absent_labels = sorted(
        mandatory - {label for test in selected for label in test.labels}
    )
    if absent_labels:
        errors.append(
            "required execution labels are absent: " + ", ".join(absent_labels)
        )
    missing = [test for test in selected if test not in actual]
    unexpected = [outcome.test for outcome in outcomes if outcome.test not in expected]
    if missing:
        errors.append(
            f"{len(missing)} selected tests have no result: {_preview(missing)}"
        )
    if unexpected:
        errors.append(f"{len(unexpected)} unexpected results: {_preview(unexpected)}")

    rows = []
    counts = Counter({name: 0 for name in OUTCOME_NAMES})
    required_skips: list[inventory.TestRecord] = []
    invalid: list[inventory.TestRecord] = []
    platform_skips = 0
    external_input_skips = 0
    for test in selected:
        result = actual.get(test)
        required = bool(test.labels & mandatory)
        state = result.outcome if result else "missing"
        counts[state] += 1
        permitted_platform_skip = False
        permitted_external_input_skip = False
        if result and state == "skipped" and required:
            reason = NON_APPLE_CORPUS_SKIPS.get(test)
            permitted_platform_skip = (
                profile in {"linux-semantic", "windows-focused"}
                and reason is not None
                and result.message == "SKIP_REGULAR_EXPRESSION_MATCHED"
                and reason in result.output
            )
            linux_reason = NON_LINUX_SESSION_SKIPS.get(test)
            permitted_platform_skip |= (
                profile in {"macos-patch", "windows-focused"}
                and linux_reason is not None
                and result.message == "SKIP_REGULAR_EXPRESSION_MATCHED"
                and linux_reason in result.output
            )
            if permitted_platform_skip:
                platform_skips += 1
            external = OPTIONAL_KERNEL_INPUT_SKIPS.get(test)
            if external is not None:
                inputs, reason = external
                permitted_external_input_skip = (
                    not inputs <= configured_kernel_inputs
                    and result.message == "SKIP_REGULAR_EXPRESSION_MATCHED"
                    and reason in result.output.splitlines()
                )
                if permitted_external_input_skip:
                    external_input_skips += 1
            if not permitted_platform_skip and not permitted_external_input_skip:
                required_skips.append(test)
        if state in {"failed", "disabled", "not_run"}:
            invalid.append(test)
        row = {
            "name": test.name,
            "labels": sorted(test.labels),
            "outcome": state,
            "required": required,
            "permitted_platform_skip": permitted_platform_skip,
            "permitted_external_input_skip": permitted_external_input_skip,
        }
        if result and state != "passed":
            row.update(message=result.message, output=result.output[:4000])
            if len(result.output) > 4000:
                row["output_truncated"] = True
        rows.append(row)
    if invalid:
        errors.append(
            f"{len(invalid)} tests failed, were disabled, or could not run: {_preview(invalid)}"
        )
    if required_skips:
        errors.append(
            f"{len(required_skips)} required tests were skipped; execution evidence "
            f"was not obtained: {_preview(required_skips)}"
        )
    return {
        "schema": 1,
        "ok": not errors,
        "evidence_unit": "CTest registration; child-runner coverage is not inferred",
        "profile": profile,
        "ctest_exit_status": exit_status,
        "counts": {
            "selected": len(selected),
            "reported": len(outcomes),
            **{name: counts[name] for name in (*OUTCOME_NAMES, "missing")},
            "unexpected": len(unexpected),
            "required_skips": len(required_skips),
            "permitted_platform_skips": platform_skips,
            "permitted_external_input_skips": external_input_skips,
        },
        "errors": errors,
        "tests": rows,
        "unexpected_results": [
            {
                "name": test.name,
                "labels": sorted(test.labels),
                "outcome": actual[test].outcome,
            }
            for test in unexpected
        ],
    }


def read_exit_status(path: Path) -> int:
    text = path.read_text(encoding="utf-8").strip()
    if not re.fullmatch(r"0|[1-9][0-9]{0,2}", text) or int(text) > 255:
        raise ResultError("CTest exit status file must contain one integer in 0..255")
    return int(text)


def check_ctest_version(output: str) -> None:
    match = re.search(r"^ctest version (\d+)\.(\d+)\.(\d+)", output, re.MULTILINE)
    if not match or tuple(map(int, match.groups())) < (3, 29, 0):
        raise ResultError(
            "CI outcome auditing requires CTest >= 3.29 for JUnit labels "
            "and --tests-from-file"
        )


def format_summary(report: dict, matrix_name: str) -> str:
    lines = [f"### CTest outcomes: {matrix_name}", "", report["evidence_unit"], ""]
    counts = report.get("counts")
    if counts is not None:
        lines += ["| Outcome | Registrations |", "|---|---:|"]
        lines += [f"| {name} | {value} |" for name, value in counts.items()]
        lines.append("")
    if report["errors"]:
        lines += ["Audit failed:", ""]
        lines += [f"- {error}" for error in report["errors"]]
    else:
        lines.append(
            "Inventory and outcomes reconcile; required execution evidence is present."
        )
    detail = (
        "Every skipped registration remains listed in the outcome JSON and JUnit artifacts."
        if counts is not None
        else "Execution totals are unavailable because the evidence is incomplete or invalid."
    )
    lines += ["", detail, ""]
    return "\n".join(lines)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser(
        "check-tool", help="check the CI-only CTest version requirement"
    )
    audit = commands.add_parser("audit", help="audit one complete selected CI profile")
    for argument in ("inventory", "junit", "ctest-status", "output"):
        audit.add_argument(f"--{argument}", type=Path, required=True)
    audit.add_argument(
        "--profile", choices=sorted(inventory.PROFILE_EXCLUSIONS), required=True
    )
    audit.add_argument("--exclude-label-regex", required=True)
    audit.add_argument("--matrix-name", required=True)
    audit.add_argument("--prior-passes", type=Path)
    reuse = commands.add_parser(
        "reuse", help="preserve passed focused tests before executing the remaining profile"
    )
    for argument in ("inventory", "exclude-tests", "prior-passes", "output"):
        reuse.add_argument(f"--{argument}", type=Path, required=True)
    reuse.add_argument("--completed-junit", type=Path, action="append", required=True)
    reuse.add_argument(
        "--profile", choices=sorted(inventory.PROFILE_EXCLUSIONS), required=True
    )
    reuse.add_argument("--exclude-label-regex", required=True)
    arguments = parser.parse_args(argv)
    if arguments.command == "check-tool":
        try:
            completed = subprocess.run(
                ["ctest", "--version"], capture_output=True, text=True, check=True
            )
            check_ctest_version(completed.stdout)
        except (OSError, subprocess.SubprocessError, ResultError) as error:
            print(f"CTest outcome tooling check failed: {error}", file=sys.stderr)
            return 1
        print(completed.stdout.splitlines()[0] + ": JUnit outcome auditing available")
        return 0

    if arguments.command == "reuse":
        try:
            document = json.loads(arguments.inventory.read_text(encoding="utf-8"))
            discovery = inventory.audit_inventory(
                document, arguments.profile, arguments.exclude_label_regex
            )
            completed = [ET.parse(path).getroot() for path in arguments.completed_junit]
            retained = prior_passes(
                inventory.parse_inventory(document), discovery.selected_records, completed
            )
            reused = parse_junit(retained)
            ET.ElementTree(retained).write(
                arguments.prior_passes, encoding="utf-8", xml_declaration=True
            )
            arguments.exclude_tests.write_text(
                "".join(name + "\n" for name in sorted({result.test.name for result in reused})),
                encoding="utf-8", newline="\n",
            )
            report = {
                "ok": True,
                "head": os.environ.get("GITHUB_SHA"),
                "run_id": os.environ.get("GITHUB_RUN_ID"),
                "run_attempt": os.environ.get("GITHUB_RUN_ATTEMPT"),
                "selected_count": discovery.selected_count,
                "reused_passed": len(reused),
                "remaining_count": discovery.selected_count - len(reused),
                "inventory_sha256": hashlib.sha256(arguments.inventory.read_bytes()).hexdigest(),
                "completed_junit": [
                    {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                    for path in arguments.completed_junit
                ],
                "prior_passes_sha256": hashlib.sha256(arguments.prior_passes.read_bytes()).hexdigest(),
            }
        except (OSError, ValueError, ET.ParseError) as error:
            report = {"ok": False, "errors": [str(error)]}
        arguments.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(report, indent=2))
        return 0 if report["ok"] else 1

    report = {
        "schema": 1,
        "ok": False,
        "evidence_unit": "CTest registration; no execution claim when evidence is invalid",
        "profile": arguments.profile,
        "errors": [],
    }
    try:
        discovery = inventory.audit_inventory(
            json.loads(arguments.inventory.read_text(encoding="utf-8")),
            arguments.profile,
            arguments.exclude_label_regex,
        )
        exit_status = read_exit_status(arguments.ctest_status)
        outcomes = parse_junit(ET.parse(arguments.junit).getroot())
        reused = ()
        fresh_count = len(outcomes)
        if arguments.prior_passes is not None:
            reused = parse_junit(ET.parse(arguments.prior_passes).getroot())
            outcomes = combine_prior_passes(outcomes, reused)
        report = audit_results(
            discovery.selected_records, outcomes, discovery.profile, exit_status,
            frozenset(name for name in ("NEVERD_TYPES_NTOSKRNL", "NEVERD_TYPES_NTKRNLMP")
                      if name in os.environ),
        )
        if arguments.prior_passes is not None:
            report["reuse"] = {
                "fresh_reported": fresh_count,
                "prior_passed": len(reused),
                "prior_passes_sha256": hashlib.sha256(arguments.prior_passes.read_bytes()).hexdigest(),
            }
    except (OSError, ValueError, ET.ParseError) as error:
        report["errors"] = [str(error)]
    arguments.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    summary = format_summary(report, arguments.matrix_name)
    print(summary)
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary_path:
        with Path(summary_path).open("a", encoding="utf-8", newline="\n") as stream:
            stream.write(summary)
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
