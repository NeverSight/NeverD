#!/usr/bin/env python3
"""Qualify native Apple PAC before running the signed language install matrix."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import plistlib
import re
import struct
import subprocess
import tempfile
import xml.etree.ElementTree as ET
from xml.parsers.expat import ExpatError

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
    if kind in ("raw", "asm"):
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


def cursor_observation(result, recipe):
    policy = POLICY["cursor_probe"]
    lines = result.stdout.splitlines()
    if not lines and result.returncode < 0:
        return {"status": POLICY["unavailable"]}
    if (lines == [policy["started"]] and
            result.returncode == policy["unavailable_code"] and
            recipe == policy["recipe_explicit"]):
        return {"status": POLICY["unavailable"]}
    if result.returncode or len(lines) != 2 or lines[0] != policy["started"]:
        raise ValueError(policy["invalid_report"])
    data = json.loads(lines[1])
    if (not isinstance(data, dict) or set(data) != {"checks", *policy["counts"]} or
            type(data["checks"]) is not int or data["checks"] <= 0):
        raise ValueError(policy["invalid_report"])
    for name in policy["counts"]:
        values = data[name]
        if (not isinstance(values, list) or len(values) != 2 or
                any(type(v) is not int or v < 0 or v > policy["samples"] for v in values)):
            raise ValueError(policy["invalid_report"])
    required = policy["required"] + (
        policy["explicit_required"] if recipe == policy["recipe_explicit"] else [])
    if any(not all(data[name]) for name in required):
        raise ValueError(policy["invalid_report"])
    return {"status": POLICY["passed"], **data}


def probe_signal_cursor(output):
    policy = POLICY["cursor_probe"]
    source = ROOT / policy["source"]
    report = {**provenance(), "platform": platform.platform(), "profiles": {},
              "status": POLICY["failed"], policy["baseline_key"]: False,
              policy["explicit_key"]: False, policy["pacm_pc_key"]: False}
    report["source_sha256"].update(
        {name: digest(ROOT / name) for name in [policy["source"], *policy["source_files"]]})
    try:
        if platform.system() != POLICY["host_os"] or platform.machine() not in POLICY["host_arches"]:
            report["status"] = POLICY["unavailable"]
            return report
        with OwnedProducts(output, False) as products:
            for optimization in policy["optimizations"]:
                label = policy["binary_label"].format(optimization=optimization)
                binary = output / label
                products.append(binary)
                argv = [arg.format(optimization=optimization, include=ROOT / policy["include"],
                                   source=source, binary=binary) for arg in policy["compile"]]
                built = command(products, output, label + POLICY["compile_suffix"],
                                argv, POLICY["tool_timeout"])
                if built.returncode:
                    return report
                identity = check_binary(binary)
                for recipe in policy["recipes"]:
                    label = policy["profile_label"].format(optimization=optimization, recipe=recipe)
                    result = command(products, output, label, [binary, recipe], POLICY["tool_timeout"])
                    report["profiles"][label] = {**identity, **cursor_observation(result, recipe)}
            def observations(recipe):
                return [report["profiles"][policy["profile_label"].format(
                    optimization=optimization, recipe=recipe)] for optimization in policy["optimizations"]]
            pacm = observations(policy["recipe_pacm"])
            report[policy["baseline_key"]] = all(p["status"] == POLICY["passed"] for p in pacm)
            report[policy["pacm_pc_key"]] = all(p["status"] == POLICY["passed"] and
                                               all(p["pc_rejected"]) for p in pacm)
            report[policy["explicit_key"]] = all(
                p["status"] == POLICY["passed"] for p in observations(policy["recipe_explicit"]))
            report["status"] = POLICY["passed"] if report[policy["baseline_key"]] else POLICY["unavailable"]
        return report
    finally:
        save(output / POLICY["report_name"], report)


def sign_cpta_slice(products, output, binary, policy):
    entitlements = output / policy["entitlements_file"]
    products.append(entitlements)
    entitlements.write_bytes(plistlib.dumps(policy["entitlements"]))
    signed = command(products, output, policy["sign_label"],
                     [v.format(plist=entitlements, binary=binary) for v in policy["sign_command"]],
                     POLICY["tool_timeout"])
    if signed.returncode:
        return None
    verified = command(products, output, policy["x1"] + POLICY["signature_suffix"],
                       [*POLICY["signature_command"], binary], POLICY["tool_timeout"])
    if verified.returncode:
        return None
    embedded = command(products, output, policy["read_label"],
                       [v.format(binary=binary) for v in policy["read_command"]], POLICY["tool_timeout"])
    if embedded.returncode:
        return None
    try:
        actual = plistlib.loads(embedded.stdout.encode())
    except (ValueError, plistlib.InvalidFileException, ExpatError):
        return None
    # Compare the plist types too. Python treats booleans as integers when comparing dictionaries.
    if plistlib.dumps(actual) == entitlements.read_bytes():
        return actual
    return None


def probe_cpta(output):
    policy = POLICY["cpta_probe"]
    source = ROOT / policy["source"]
    report = {**provenance(), "platform": platform.platform(), "profiles": {},
              "status": POLICY["failed"], policy["observed_key"]: False,
              policy["qualified_key"]: False, policy["writeback_qualified_key"]: False}
    report["source_sha256"][policy["source"]] = digest(source)
    try:
        if platform.system() != POLICY["host_os"] or platform.machine() not in POLICY["host_arches"]:
            report["status"] = POLICY["unavailable"]
            return report
        with OwnedProducts(output, False) as products:
            for name, argv in POLICY["environment_commands"].items():
                command(products, output, name, argv, POLICY["tool_timeout"])
            for arch, subtype in policy["profiles"].items():
                binary = output / (arch + POLICY["binary_suffix"])
                products.append(binary)
                argv = [arg.format(arch=arch, source=source, binary=binary) for arg in policy["compile"]]
                built = command(products, output, arch + POLICY["compile_suffix"], argv, POLICY["tool_timeout"])
                profile = report["profiles"][arch] = {"outcome": policy["unexpected"]}
                if built.returncode:
                    if (arch == policy["x1"] and arch in built.stderr and
                            any(t in built.stderr.lower() for t in policy["unsupported_compiler"])):
                        profile["outcome"] = policy["slice_unavailable"]
                    continue
                if arch == policy["x1"]:
                    signed = sign_cpta_slice(products, output, binary, policy)
                    if signed is None:
                        continue
                    profile[policy["entitlements_key"]] = signed
                raw = binary.read_bytes()
                magic, cpu, actual = struct.unpack_from(POLICY["header_format"], raw)
                profile.update(sha256=digest(binary), cpu_subtype=actual)
                if (magic, cpu, actual & POLICY["subtype_mask"]) != (POLICY["macho_magic"], POLICY["cpu_type"], subtype):
                    continue
                vectors = policy["vectors"]
                arguments = [binary, *(v for row in vectors for v in row[:2])]
                try:
                    result = command(products, output, arch, arguments, POLICY["tool_timeout"])
                except OSError as error:
                    if error.errno != policy["bad_cpu_type_errno"]:
                        raise
                    profile.update(outcome=policy["loader_unavailable"], errno=error.errno)
                    continue
                lines = result.stdout.splitlines()
                if not lines:
                    if result.returncode < 0:
                        profile["outcome"] = policy["loader_unavailable"]
                    continue
                if lines[0] != policy["started"]:
                    continue
                profile["results"] = lines[1:]
                if result.returncode == policy["undefined_instruction"] and len(lines) == 1:
                    profile["outcome"] = policy["isa_unavailable"]
                elif result.returncode == 0:
                    instructions = policy["instructions"]
                    if len(lines) == 1 + len(vectors) * len(instructions):
                        by_instruction = {name: lines[index + 1::len(instructions)]
                                          for index, name in enumerate(instructions)}
                        profile[policy["scalar_results_key"]] = by_instruction
                        if all(values == [row[3] for row in vectors] for values in by_instruction.values()):
                            profile["outcome"] = policy["checked"]
                        elif all(values == [row[2] for row in vectors] for values in by_instruction.values()):
                            profile["outcome"] = policy["unchecked"]
                if profile["outcome"] in (policy["checked"], policy["unchecked"], policy["isa_unavailable"]):
                    writeback = command(products, output, arch + policy["writeback_suffix"],
                                        [binary, policy["writeback_mode"]], POLICY["tool_timeout"])
                    state = policy["unexpected"]
                    if writeback.returncode == 0:
                        state = {policy["writeback_checked"]: policy["checked"],
                                 policy["writeback_unchecked"]: policy["unchecked"]}.get(
                                     writeback.stdout.strip(), policy["unexpected"])
                    profile[policy["writeback_key"]] = state
            outcomes = {name: profile["outcome"] for name, profile in report["profiles"].items()}
            report[policy["observed_key"]] = policy["checked"] in outcomes.values()
            writebacks = {arch: p.get(policy["writeback_key"]) for arch, p in report["profiles"].items()}
            report[policy["writeback_qualified_key"]] = writebacks[policy["x1"]] == policy["checked"]
            report[policy["qualified_key"]] = (outcomes[policy["x1"]] == policy["checked"] and
                                                report[policy["writeback_qualified_key"]])
            if policy["unexpected"] in outcomes.values() or policy["unexpected"] in writebacks.values():
                report["status"] = POLICY["failed"]
            else:
                report["status"] = POLICY["passed"] if report[policy["qualified_key"]] else POLICY["unavailable"]
        return report
    finally:
        save(output / POLICY["report_name"], report)


def legacy_cases(contract):
    return [{"optimization": optimization, "dwarf": dwarf,
             "name": optimization + contract[
                 "legacy_dwarf_suffix" if dwarf else "legacy_compact_suffix"]}
            for optimization in POLICY["optimizations"] for dwarf in POLICY["dwarf_modes"]]


def build_legacy_fixtures(output):
    """Produce owned legacy images without executing arm64e on the build host."""
    report = {**provenance(), "status": POLICY["failed"], "images": []}
    try:
        contract = fixture_contract()
        with OwnedProducts(output, True) as products:
            lookup = command(products, output, POLICY["legacy_linker_label"],
                             [contract["xcrun"], contract["find_tool"],
                              contract["classic_linker"]], POLICY["tool_timeout"])
            linker = Path(lookup.stdout.strip())
            if lookup.returncode or not lookup.stdout.strip() or not linker.is_file():
                raise RuntimeError(POLICY["errors"]["legacy_linker"])
            report["linker_sha256"] = digest(linker)
            version = command(products, output, POLICY["legacy_compiler_label"],
                              [contract["compiler"], *POLICY["legacy_version_args"]],
                              POLICY["tool_timeout"])
            version.check_returncode()
            report["compiler"] = version.stdout
            source, assembly = output / POLICY["legacy_source"], output / POLICY["legacy_assembly"]
            source.write_text(contract["source"])
            assembly.write_text(contract["legacy_assembly"])
            for case in legacy_cases(contract):
                binary = output / case["name"]
                products.append(binary)
                argv = [contract["compiler"], *contract["legacy_args"],
                        contract["linker_option"] + str(linker),
                        contract["legacy_deployment"], contract["threaded_option"],
                        case["optimization"]]
                if case["dwarf"]:
                    argv.append(contract["dwarf_option"])
                argv.extend([str(source), str(assembly), POLICY["output_flag"], str(binary)])
                result = command(products, output, case["name"] + POLICY["compile_suffix"],
                                 argv, POLICY["tool_timeout"])
                result.check_returncode()
                signed = command(products, output, case["name"] + POLICY["signature_suffix"],
                                 [*POLICY["signature_command"], str(binary)], POLICY["tool_timeout"])
                signed.check_returncode()
                info = command(products, output, case["name"] + POLICY["legacy_info_suffix"],
                               [contract["xcrun"], *contract["threaded_info_args"], str(binary)],
                               POLICY["tool_timeout"])
                info.check_returncode()
                report["images"].append({**case, **check_binary(binary)})
            report["status"] = POLICY["passed"]
        return report
    finally:
        save(output / POLICY["report_name"], report)


def verify_legacy_fixtures(directory):
    report = json.loads((directory / POLICY["report_name"]).read_text())
    expected = provenance()
    if (report.get("status") != POLICY["passed"] or
            any(report.get(key) != expected[key] for key in POLICY["provenance_keys"])):
        raise ValueError(POLICY["errors"]["legacy"])
    cases = legacy_cases(fixture_contract())
    images = report.get("images", [])
    if len(images) != len(cases):
        raise ValueError(POLICY["errors"]["legacy"])
    for case, image in zip(cases, images):
        if image != {**case, **check_binary(directory / case["name"])}:
            raise ValueError(POLICY["errors"]["legacy"])
    return report


def startup_profile(products, output):
    macros = command(products, output, POLICY["syscall_label"],
                     POLICY["syscall_command"], POLICY["tool_timeout"])
    macros.check_returncode()
    numbers = re.findall(POLICY["syscall_pattern"], macros.stdout, re.MULTILINE)
    if len(numbers) != 1:
        raise ValueError(POLICY["errors"]["syscall"])
    profile = POLICY["startup_sandbox_profile"].format(number=numbers[0])
    return {"syscall_number": int(numbers[0]), "profile": profile,
            "launcher": [arg.format(profile=profile)
                         for arg in POLICY["startup_launcher"]]}


def probe(output, *, startup=False, userspace_fixups=False):
    report = {**provenance(), "status": POLICY["failed"],
              "native_pac_qualified": False,
              "compiler_original_call_unwind_qualified": False,
              "control_loading": POLICY["startup_loading"] if startup else POLICY["control_loading"],
              POLICY["fixup_loader_key"]: POLICY[
                  "userspace_fixup_loader" if userspace_fixups else "system_fixup_loader"],
              POLICY["startup_qualification_key"]: False,
              POLICY["cpta_qualification_key"]: False,
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
            execution_prefix = []
            if userspace_fixups:
                profile = startup_profile(products, output)
                execution_prefix = profile["launcher"]
                report[POLICY["startup_profile_key"]] = profile
            sources = {}
            for name, filename in POLICY["source_names"].items():
                path = output / filename
                path.write_text(contract[name])
                sources[name] = path

            def compile_binary(label, args, suffix):
                binary = output / (label + suffix)
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
                return binary, identity

            def compile_control(label, args, library=False):
                suffix = POLICY["library_suffix"] if library else POLICY["binary_suffix"]
                binary, identity = compile_binary(label, args, suffix)
                command(products, output, label + POLICY["disassembly_suffix"],
                        [*POLICY["disassembly_command"], str(binary)],
                        POLICY["tool_timeout"])
                command(products, output, label + POLICY["fixup_suffix"],
                        [*POLICY["fixup_command"], str(binary)],
                        POLICY["tool_timeout"])
                argv = [str(binary)]
                if library:
                    driver, driver_identity = compile_binary(
                        label + POLICY["loader_suffix"], [args[0], sources["loader"]],
                        POLICY["binary_suffix"])
                    identity["loader"] = driver_identity
                    argv.insert(0, str(driver))
                run = command(products, output, label, execution_prefix + argv,
                              POLICY["tool_timeout"])
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
                    args = [optimization]
                    if not startup:
                        args.extend(contract["library_args"])
                    args.append(contract["control_option"])
                    if dwarf:
                        args.append(contract["dwarf_option"])
                    args.extend(sources[name] for name in POLICY["runtime_sources"])
                    run, row = compile_control(label, args, library=not startup)
                    row["status"] = POLICY["passed"] if (
                        run.returncode == 0 and contract["runtime_completed"] in run.stdout.splitlines()
                    ) else POLICY["failed"]
                    if row["status"] != POLICY["passed"]:
                        return report
            report["compiler_original_call_unwind_qualified"] = True
            report[POLICY["startup_qualification_key"]] = startup
            report["status"] = POLICY["passed"]
        return report
    finally:
        save(output / POLICY["report_name"], report)


def expected_cases(executable=False, generated=False):
    key = ("generated_" if generated else "") + ("executable_" if executable else "") + "families"
    return {POLICY["case_format"].format(family=family, index=index)
            for family in POLICY[key]
            for index in POLICY["indices"]}


def check_runtime_report(xml, executable=False, generated=False):
    cases = list(ET.fromstring(xml).iter(POLICY["testcase_tag"]))
    names = [case.attrib["name"].split(POLICY["parameter_separator"], 1)[0].rstrip()
             for case in cases]
    if len(names) != len(expected_cases(executable, generated)) or set(names) != expected_cases(executable, generated):
        raise ValueError(POLICY["errors"]["cases"])
    for case in cases:
        if (case.attrib.get("status") != POLICY["run_status"] or
                any(case.find(tag) is not None for tag in POLICY["failure_tags"])):
            raise ValueError(POLICY["errors"]["runtime"].format(name=case.attrib["name"]))
    return names


def run_installation(output, build, probe_report, legacy_fixtures=None, *,
                     executable=False, system_startup=False, ir_compiler=None):
    generated = ir_compiler is not None
    userspace = executable and not system_startup
    report = {**provenance(), "status": POLICY["failed"],
              "native_installer_call_unwind_qualified": False,
              POLICY["publication_qualification_key"]: False,
              POLICY["executable_qualification_key"]: False,
              POLICY["default_executable_qualification_key"]: False,
              POLICY["generated_qualification_key"]: False,
              POLICY["generated_executable_qualification_key"]: False,
              POLICY["cpta_qualification_key"]: False,
              "whole_image_publication_qualified": False}
    try:
        control = json.loads(probe_report.read_text())
        if (control.get("status") != POLICY["passed"] or
                control.get("control_loading") != POLICY[
                    "startup_loading" if userspace else "control_loading"] or
                control.get(POLICY["fixup_loader_key"]) != POLICY[
                    "userspace_fixup_loader" if userspace else "system_fixup_loader"] or
                (userspace and not control.get(POLICY["startup_qualification_key"])) or
                not control.get("native_pac_qualified") or
                not control.get("compiler_original_call_unwind_qualified") or
                any(control.get(key) != report[key] for key in POLICY["provenance_keys"])):
            raise ValueError(POLICY["errors"]["probe"])
        report["probe_report_sha256"] = digest(probe_report)
        if legacy_fixtures is not None:
            report["legacy_fixtures"] = verify_legacy_fixtures(legacy_fixtures)
        regex_key = (("generated_" if generated else "") +
                     ("executable_" if executable else "") + "test_regex")
        argv = [*POLICY["ctest_command"], POLICY["build_dir_flag"], str(build),
                POLICY["test_regex_flag"], POLICY[regex_key]]
        with OwnedProducts(output, False) as products:
            if generated:
                ir_compiler = ir_compiler.resolve()
                if not ir_compiler.is_file() or not os.access(ir_compiler, os.X_OK):
                    raise ValueError(POLICY["errors"]["ir_compiler"])
                version = command(products, output, POLICY["ir_compiler_label"],
                                  [str(ir_compiler), *POLICY["ir_compiler_version_args"]],
                                  POLICY["tool_timeout"])
                if version.returncode:
                    raise ValueError(POLICY["errors"]["ir_compiler"])
                report[POLICY["ir_compiler_key"]] = {"path": str(ir_compiler), "sha256": digest(ir_compiler),
                                         "version": version.stdout}
            execution_prefix = []
            report[POLICY["fixup_loader_key"]] = POLICY["system_fixup_loader"]
            if userspace:
                profile = startup_profile(products, output)
                if control.get(POLICY["startup_profile_key"]) != profile:
                    raise ValueError(POLICY["errors"]["startup_profile"])
                report[POLICY["startup_profile_key"]] = profile
                report[POLICY["fixup_loader_key"]] = POLICY["userspace_fixup_loader"]
                execution_prefix = profile["launcher"]
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
            if len(filters) != len(expected_cases(executable, generated)) or set(filters) != expected_cases(executable, generated) or len(binaries) != 1:
                raise ValueError(POLICY["errors"]["cases"])
            report["test_binary_sha256"] = digest(next(iter(binaries)))
            xml = output / POLICY["xml_name"]
            with tempfile.TemporaryDirectory(prefix=POLICY["temporary_prefix"], dir=output) as temporary:
                env = dict(os.environ, **POLICY["test_environment"])
                env.pop(POLICY["artifact_environment"], None)
                env.pop(POLICY["executable_environment"], None)
                contract = fixture_contract()
                env.pop(contract["ir_compiler_environment"], None)
                if generated:
                    env[contract["ir_compiler_environment"]] = str(ir_compiler)
                if executable:
                    env[POLICY["executable_environment"]] = POLICY[
                        "executable_environment_value" if userspace else "system_fixup_loader"]
                legacy_environment = contract["legacy_environment"]
                env.pop(legacy_environment, None)
                if legacy_fixtures is not None:
                    env[legacy_environment] = str(legacy_fixtures.resolve())
                env[POLICY["temporary_environment"]] = temporary
                result = command(products, output, POLICY["runtime_label_name"],
                                 [*execution_prefix, *argv, *POLICY["ctest_run_flags"], str(xml)],
                                 POLICY["suite_timeout"], env)
            report["tests"] = check_runtime_report(xml.read_text(), executable, generated)
            if result.returncode:
                raise RuntimeError(POLICY["errors"]["ctest"])
            report["runtime_report_sha256"] = digest(xml)
            report["native_installer_call_unwind_qualified"] = not executable
            report[POLICY["publication_qualification_key"]] = not executable and not generated
            report[POLICY["executable_qualification_key"]] = executable and not generated
            report[POLICY["default_executable_qualification_key"]] = executable and system_startup and not generated
            report[POLICY["generated_qualification_key"]] = generated and not executable
            report[POLICY["generated_executable_qualification_key"]] = generated and executable
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
    parser.add_argument(POLICY["legacy_argument"], type=Path)
    parser.add_argument(POLICY["ir_compiler_argument"], type=Path)
    parser.add_argument(POLICY["startup_fixups_argument"],
                        choices=POLICY["startup_fixup_loaders"],
                        default=POLICY["system_fixup_loader"])
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    try:
        userspace = args.startup_fixups == POLICY["userspace_fixup_loader"]
        if userspace and args.mode != POLICY["startup_mode"]:
            raise ValueError(POLICY["errors"]["startup_fixups"])
        if args.mode == POLICY["probe_mode"]:
            report = probe(output)
        elif args.mode == POLICY["cpta_probe"]["mode"]:
            report = probe_cpta(output)
        elif args.mode == POLICY["cursor_probe"]["mode"]:
            report = probe_signal_cursor(output)
        elif args.mode == POLICY["startup_mode"]:
            report = probe(output, startup=True, userspace_fixups=userspace)
        elif args.mode == POLICY["legacy_mode"]:
            report = build_legacy_fixtures(output)
        else:
            if not args.build_dir or not args.probe_report:
                raise ValueError(POLICY["errors"]["arguments"])
            generated = args.mode in (POLICY["generated_mode"], POLICY["generated_executable_mode"])
            if generated != (args.ir_compiler is not None):
                raise ValueError(POLICY["errors"]["ir_compiler"])
            report = run_installation(output, args.build_dir.resolve(), args.probe_report.resolve(),
                                      args.legacy_fixtures,
                                      executable=args.mode in (POLICY["executable_mode"],
                                                               POLICY["default_executable_mode"],
                                                               POLICY["generated_executable_mode"]),
                                      system_startup=args.mode in (POLICY["default_executable_mode"],
                                                                   POLICY["generated_executable_mode"]),
                                      ir_compiler=args.ir_compiler)
        print(json.dumps({"status": report["status"], "report": str(output / POLICY["report_name"])}))
        return POLICY["exit_codes"][report["status"]]
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        save(output / POLICY["error_name"], {"error": str(error)})
        print(str(error))
        return POLICY["exit_codes"][POLICY["failed"]]


if __name__ == "__main__":
    raise SystemExit(main())
