#!/usr/bin/env python3
"""Qualify native Apple PAC before running the signed language install matrix."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import struct
import subprocess
import tempfile
import xml.etree.ElementTree as ET

from arm64e_artifacts import OwnedProducts

ROOT = Path(__file__).resolve().parents[1]
POLICY = json.loads(Path(__file__).with_suffix(".def").read_text())


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def definition(text, kind, name):
    matches = re.findall(POLICY["patterns"][kind].replace(
        POLICY["symbol_placeholder"], re.escape(name)), text, re.DOTALL)
    if len(matches) != 1:
        raise ValueError(POLICY["errors"]["definition"].format(name=name))
    value = matches[0]
    if kind == "raw":
        return value
    if kind == "args":
        return json.loads("[" + value + "]")
    return json.loads(value)


def fixture_contract():
    text = (ROOT / POLICY["fixture"]).read_text()
    return {name: definition(text, entry["kind"], entry["symbol"])
            for name, entry in POLICY["definitions"].items()}


def command(products, output, label, argv, timeout, env=None):
    result = products.run(argv, capture_output=True, text=True,
                          timeout=timeout, env=env)
    save(output / (label + POLICY["command_suffix"]), {
        "argv": list(map(str, argv)), "returncode": result.returncode,
        "stdout": result.stdout, "stderr": result.stderr})
    return result


def capability_status(result, contract):
    started = contract["started"] in result.stdout.splitlines()
    if result.returncode < 0 and not started:
        return POLICY["unavailable"]
    if not started:
        return POLICY["failed"]
    if result.returncode == contract["unavailable_code"]:
        return POLICY["unavailable"]
    if result.returncode or contract["completed"] not in result.stdout.splitlines():
        return POLICY["failed"]
    return POLICY["passed"]


def provenance():
    revision = subprocess.check_output(
        POLICY["revision_command"], cwd=ROOT, text=True).strip()
    return {"source_revision": revision,
            "source_sha256": {name: digest(ROOT / name)
                              for name in POLICY["source_files"]}}


def check_binary(binary):
    raw = binary.read_bytes()
    fields = struct.unpack_from(POLICY["header_format"], raw)
    if (fields[0] != POLICY["macho_magic"] or
            fields[1] != POLICY["cpu_type"] or
            fields[2] & POLICY["subtype_mask"] != POLICY["cpu_subtype"]):
        raise ValueError(POLICY["errors"]["binary"])
    return {"sha256": digest(binary), "bytes": len(raw),
            "cpu_subtype": fields[2]}


def probe(output):
    report = {**provenance(), "status": POLICY["failed"],
              "native_pac_qualified": False,
              "compiler_original_call_unwind_qualified": False,
              "whole_image_publication_qualified": False,
              "platform": platform.platform(), "machine": platform.machine(),
              "controls": []}
    try:
        if platform.system() != POLICY["host_os"] or platform.machine() not in POLICY["host_arches"]:
            report["status"] = POLICY["unavailable"]
            return report
        contract = fixture_contract()
        with OwnedProducts(output, False) as products:
            report["environment"] = {}
            for name, argv in POLICY["environment_commands"].items():
                result = command(products, output, name, argv, POLICY["tool_timeout"])
                report["environment"][name] = {
                    "returncode": result.returncode,
                    "stdout": result.stdout, "stderr": result.stderr}
            sources = {}
            for name, filename in POLICY["source_names"].items():
                path = output / filename
                path.write_text(contract[name])
                sources[name] = path

            def compile_control(label, args):
                binary = output / (label + POLICY["binary_suffix"])
                products.append(binary)
                result = command(products, output, label + POLICY["compile_suffix"],
                                 [contract["compiler"], *contract["compile_args"],
                                  *map(str, args), POLICY["output_flag"], str(binary)],
                                 POLICY["tool_timeout"])
                if result.returncode:
                    raise RuntimeError(POLICY["errors"]["compile"].format(label=label))
                identity = check_binary(binary)
                signed = command(products, output, label + POLICY["signature_suffix"],
                                 [*POLICY["signature_command"], str(binary)],
                                 POLICY["tool_timeout"])
                if signed.returncode:
                    raise RuntimeError(POLICY["errors"]["signature"].format(label=label))
                run = command(products, output, label, [str(binary)], POLICY["tool_timeout"])
                row = {"label": label, **identity, "returncode": run.returncode,
                       "stdout": run.stdout, "stderr": run.stderr}
                report["controls"].append(row)
                return run, row

            for optimization in POLICY["optimizations"]:
                label = POLICY["capability_label"].format(optimization=optimization)
                run, row = compile_control(label, [optimization, sources["capability"]])
                row["status"] = capability_status(run, contract)
                if row["status"] != POLICY["passed"]:
                    report["status"] = row["status"]
                    return report
            report["native_pac_qualified"] = True
            for optimization in POLICY["optimizations"]:
                for dwarf in POLICY["dwarf_modes"]:
                    label = POLICY["runtime_label"].format(optimization=optimization, dwarf=dwarf)
                    args = [optimization, contract["control_option"]]
                    if dwarf:
                        args.append(contract["dwarf_option"])
                    args.extend(sources[name] for name in POLICY["runtime_sources"])
                    run, row = compile_control(label, args)
                    row["status"] = POLICY["passed"] if (
                        run.returncode == 0 and contract["runtime_completed"] in run.stdout.splitlines()
                    ) else POLICY["failed"]
                    if row["status"] != POLICY["passed"]:
                        return report
            report["compiler_original_call_unwind_qualified"] = True
            report["status"] = POLICY["passed"]
        return report
    finally:
        save(output / POLICY["report_name"], report)


def expected_cases():
    return {POLICY["case_format"].format(family=family, index=index)
            for family in POLICY["families"] for index in POLICY["indices"]}


def check_runtime_report(xml):
    cases = list(ET.fromstring(xml).iter(POLICY["testcase_tag"]))
    names = [case.attrib["name"].split(POLICY["parameter_separator"], 1)[0].rstrip()
             for case in cases]
    if len(names) != len(expected_cases()) or set(names) != expected_cases():
        raise ValueError(POLICY["errors"]["cases"])
    for case in cases:
        if (case.attrib.get("status") != POLICY["run_status"] or
                any(case.find(tag) is not None for tag in POLICY["failure_tags"])):
            raise ValueError(POLICY["errors"]["runtime"].format(name=case.attrib["name"]))
    return names


def run_installation(output, build, probe_report):
    report = {**provenance(), "status": POLICY["failed"],
              "native_installer_call_unwind_qualified": False,
              "whole_image_publication_qualified": False}
    try:
        control = json.loads(probe_report.read_text())
        if (control.get("status") != POLICY["passed"] or
                not control.get("native_pac_qualified") or
                not control.get("compiler_original_call_unwind_qualified") or
                any(control.get(key) != report[key] for key in POLICY["provenance_keys"])):
            raise ValueError(POLICY["errors"]["probe"])
        report["probe_report_sha256"] = digest(probe_report)
        argv = [*POLICY["ctest_command"], POLICY["build_dir_flag"], str(build),
                POLICY["test_regex_flag"], POLICY["test_regex"]]
        with OwnedProducts(output, False) as products:
            selected = command(products, output, POLICY["selection_label"],
                               [*argv, POLICY["discovery_flag"]], POLICY["discovery_timeout"])
            if selected.returncode:
                raise RuntimeError(POLICY["errors"]["discovery"])
            cases = json.loads(selected.stdout)["tests"]
            filters = []
            binaries = set()
            for case in cases:
                cmd = case.get("command", [])
                if not cmd or Path(cmd[0]).name != POLICY["test_binary"]:
                    raise ValueError(POLICY["errors"]["discovery"])
                binaries.add(Path(cmd[0]).resolve())
                filters.extend(arg.removeprefix(POLICY["gtest_prefix"])
                               for arg in cmd if arg.startswith(POLICY["gtest_prefix"]))
            if len(filters) != len(expected_cases()) or set(filters) != expected_cases() or len(binaries) != 1:
                raise ValueError(POLICY["errors"]["cases"])
            report["test_binary_sha256"] = digest(next(iter(binaries)))
            xml = output / POLICY["xml_name"]
            with tempfile.TemporaryDirectory(prefix=POLICY["temporary_prefix"], dir=output) as temporary:
                env = dict(os.environ, **POLICY["test_environment"])
                env.pop(POLICY["artifact_environment"], None)
                env[POLICY["temporary_environment"]] = temporary
                result = command(products, output, POLICY["runtime_label_name"],
                                 [*argv, *POLICY["ctest_run_flags"], str(xml)],
                                 POLICY["suite_timeout"], env)
            report["tests"] = check_runtime_report(xml.read_text())
            if result.returncode:
                raise RuntimeError(POLICY["errors"]["ctest"])
            report["runtime_report_sha256"] = digest(xml)
            report["native_installer_call_unwind_qualified"] = True
            report["status"] = POLICY["passed"]
        return report
    finally:
        save(output / POLICY["report_name"], report)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(POLICY["mode_argument"], choices=POLICY["modes"])
    parser.add_argument(POLICY["output_argument"], type=Path, required=True)
    parser.add_argument(POLICY["build_argument"], type=Path)
    parser.add_argument(POLICY["probe_argument"], type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    try:
        if args.mode == POLICY["probe_mode"]:
            report = probe(output)
        else:
            if not args.build_dir or not args.probe_report:
                raise ValueError(POLICY["errors"]["arguments"])
            report = run_installation(output, args.build_dir.resolve(), args.probe_report.resolve())
        print(json.dumps({"status": report["status"], "report": str(output / POLICY["report_name"])}))
        return POLICY["exit_codes"][report["status"]]
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        save(output / POLICY["error_name"], {"error": str(error)})
        print(str(error))
        return POLICY["exit_codes"][POLICY["failed"]]


if __name__ == "__main__":
    raise SystemExit(main())
