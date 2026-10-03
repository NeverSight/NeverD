#!/usr/bin/env python3
"""Temporary Intel HVF diagnosis; retain bounded per-family execution evidence."""

from collections import Counter
import argparse
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
OWNERS = {"fp": "NeverDX64FPTests", "darwin": "NeverDDarwinProcessTests"}


def invoke(command, directory, timeout):
    directory.mkdir(parents=True, exist_ok=True)
    log = directory / "output.log"
    status, timed_out, retired = None, False, True
    with log.open("w") as output:
        child = subprocess.Popen(command, stdin=subprocess.DEVNULL,
                                 stdout=output, stderr=subprocess.STDOUT,
                                 close_fds=True, start_new_session=True)
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
        output.flush()
    result = {"command": command, "status": status, "timed_out": timed_out,
              "child_retired": retired}
    (directory / "status.json").write_text(json.dumps(result, indent=2) + "\n")
    print(log.read_text()[-262144:], end="", flush=True)
    print(json.dumps(result), flush=True)
    return int(timed_out or not retired or status != 0)


def inventory(binary, directory):
    status = invoke([str(binary), "--gtest_list_tests"], directory, 30)
    if status:
        return status
    suite, names = None, []
    for line in (directory / "output.log").read_text().splitlines():
        label = line.split("#", 1)[0].strip()
        if not line[:1].isspace() and label.endswith("."):
            suite = label[:-1]
        elif line[:1].isspace() and suite and label:
            names.append(suite + "." + label)
    if not names or len(set(names)) != len(names):
        raise ValueError("missing or duplicate GoogleTest identities")
    families = sorted({name.split(".", 1)[0] + "." +
                       name.split(".", 1)[1].split("/", 1)[0] for name in names})
    (directory / "inventory.json").write_text(json.dumps(
        {"names": names, "families": families}, indent=2) + "\n")
    print(f"Registered {len(names)} cases in {len(families)} families", flush=True)
    return 0


def audit(kind, directory):
    declared = json.loads((directory / "inventory/inventory.json").read_text())
    expected, actual, passed, errors = set(declared["names"]), set(), set(), []
    counts = Counter()
    for index, family in enumerate(declared["families"]):
        part = directory / f"family-{index:02d}"
        try:
            status = json.loads((part / "status.json").read_text())
            if status["status"] != 0 or status["timed_out"] or not status["child_retired"]:
                errors.append(family + ": failed or incomplete process")
            for case in ET.parse(part / "results.xml").getroot().iter("testcase"):
                name = case.attrib["classname"] + "." + case.attrib["name"]
                if name in actual:
                    errors.append("duplicate result: " + name)
                actual.add(name)
                outcome = "failed" if case.find("failure") is not None else (
                    "skipped" if case.find("skipped") is not None else (
                        "passed" if case.get("status") == "run" and
                        case.get("result") == "completed" else "not_run"))
                counts[outcome] += 1
                if outcome == "passed":
                    passed.add(name)
        except (OSError, ValueError, KeyError, ET.ParseError) as error:
            errors.append(family + ": " + str(error))
    required = set()
    if kind == "darwin":
        from run_native_cpu_ci import darwin_inventory
        _, required = darwin_inventory(ROOT, "hvf", platform.machine())
    summary = {
        "commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "source_dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT, text=True).strip()),
        "owner": OWNERS[kind], "host_architecture": platform.machine(),
        "execution": "one GoogleTest process per method with all parameters",
        "complete_cpu_gate": False, "registered": len(expected),
        "total": sum(counts.values()), "counts": dict(counts),
        "missing": sorted(expected - actual), "unexpected": sorted(actual - expected),
        "required_native_tests": len(required),
        "required_native_unexecuted": sorted(required - passed), "errors": errors,
    }
    (directory / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2), flush=True)
    return int(bool(errors or expected != actual or counts["failed"] or
                    counts["not_run"] or required - passed))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("inventory", "run", "audit"))
    parser.add_argument("kind", choices=OWNERS)
    parser.add_argument("--family")
    args = parser.parse_args()
    binary = ROOT / "build-hvf/bin" / OWNERS[args.kind]
    evidence = ROOT / "build-hvf/family-evidence" / args.kind
    if args.action == "inventory":
        return inventory(binary, evidence / "inventory")
    if args.action == "audit":
        return audit(args.kind, evidence)
    declared = json.loads((evidence / "inventory/inventory.json").read_text())
    index = declared["families"].index(args.family)
    directory = evidence / f"family-{index:02d}"
    directory.mkdir(parents=True, exist_ok=True)
    return invoke([str(binary), "--gtest_filter=" + args.family + ":" + args.family + "/*",
                   "--gtest_output=xml:" + str(directory / "results.xml")], directory, 60)


if __name__ == "__main__":
    sys.exit(main())
