#!/usr/bin/env python3
"""Validate reconstructed entry IR with an explicitly selected native compiler."""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import xml.etree.ElementTree as ET

from arm64e_artifacts import OwnedProducts
import verify_arm64e_native as native

POLICY = json.loads(Path(__file__).with_suffix(".def").read_text())


def check_export(xml):
    cases = list(ET.fromstring(xml).iter(native.POLICY["testcase_tag"]))
    if (len(cases) != 1 or
            cases[0].attrib.get("name") != POLICY["export_case"] or
            cases[0].attrib.get("classname") != POLICY["export_suite"] or
            cases[0].attrib.get("status") != native.POLICY["run_status"] or
            any(cases[0].find(tag) is not None for tag in native.POLICY["failure_tags"])):
        raise ValueError(POLICY["errors"]["export"])


def check_probe(control, provenance):
    if (control.get("status") != native.POLICY["passed"] or
            not control.get("native_pac_qualified") or
            not control.get("compiler_original_call_unwind_qualified") or
            control.get("control_loading") != native.POLICY["control_loading"] or
            control.get(native.POLICY["fixup_loader_key"]) != native.POLICY["system_fixup_loader"] or
            any(control.get(key) != provenance[key] for key in native.POLICY["provenance_keys"])):
        raise ValueError(POLICY["errors"]["probe"])


def exception_frames():
    return [name for entry in POLICY["exception_entries"]
            for name in (entry, entry + POLICY["carrier_suffix"])]


def check_disassembly(text, key):
    functions = {}
    current = None
    for line in text.splitlines():
        match = re.fullmatch(POLICY["function_pattern"], line)
        if match:
            current = match.group(1)
            functions[current] = []
        elif current:
            functions[current].append(line)
    for name in exception_frames():
        body = "\n".join(functions.get(name, []))
        if not all(re.search(pattern, body) for pattern in POLICY["signed_return_patterns"]):
            raise ValueError(POLICY["errors"]["signed_frames"].format(function=name))
    if (not re.search(POLICY["authenticated_call_pattern"], text) or
            not re.search(POLICY["key_call_patterns"][str(key)], text)):
        raise ValueError(POLICY["errors"]["authenticated_call"])


def check_sections(text, dwarf):
    sections = set(re.findall(POLICY["section_pattern"], text, re.MULTILINE))
    if ((dwarf and POLICY["dwarf_section"] not in sections) or
            POLICY["compact_section"] not in sections):
        raise ValueError(POLICY["errors"]["unwind_mode"])


def check_unwind(text, dwarf):
    symbols = {name: int(address, POLICY["hex_radix"])
               for address, name in re.findall(POLICY["symbol_pattern"], text, re.MULTILINE)}
    records = {int(address, POLICY["hex_radix"]): int(encoding, POLICY["hex_radix"])
               for address, encoding in re.findall(POLICY["unwind_pattern"], text)}
    modes = (POLICY["dwarf_unwind_modes"] if dwarf else POLICY["compact_unwind_modes"])
    for name in exception_frames() + POLICY["native_exception_functions"]:
        # Adjacent compact entries with the same encoding may be coalesced.
        starts = [address for address in records if address <= symbols.get(name, -1)]
        if (not starts or (dwarf and symbols[name] not in records) or
                records[max(starts)] & POLICY["unwind_mode_mask"] not in modes):
            raise ValueError(POLICY["errors"]["unwind_records"].format(function=name))


def check_classic(binary):
    identity = native.check_binary(binary)
    if identity["cpu_subtype"] != POLICY["classic_subtype"]:
        raise ValueError(POLICY["errors"]["subtype"])
    return identity


def run(output, binary, compiler, probe_report=None, *, compile_only=False):
    provenance = native.provenance()
    report = {**provenance, "status": native.POLICY["failed"],
              "entry_source_sha256": {name: native.digest(native.ROOT / name)
                                      for name in POLICY["source_files"]},
              "native_reconstructed_entry_qualified": False,
              "neverd_codegen_qualified": False,
              "native_apple_cpta_qualified": False,
              "whole_image_publication_qualified": False,
              "compile_only": compile_only, "cases": []}
    try:
        if compile_only == (probe_report is not None):
            raise ValueError(POLICY["errors"]["arguments"])
        if probe_report:
            control = json.loads(probe_report.read_text())
            check_probe(control, provenance)
            report["probe_report_sha256"] = native.digest(probe_report)
        report["test_binary_sha256"] = native.digest(binary)
        with OwnedProducts(output, False) as products:
            def command(label, argv, timeout=POLICY["tool_timeout"], env=None):
                result = native.command(products, output, label, list(map(str, argv)), timeout, env)
                result.check_returncode()
                return result

            version = command(POLICY["version_label"], [compiler, *POLICY["version_args"]])
            report["compiler"] = {"path": str(compiler), "sha256": native.digest(compiler),
                                  "version": version.stdout}
            sdk = command(POLICY["sdk_label"], POLICY["sdk_command"])
            report["sdk_path"] = sdk.stdout.strip()
            compile_args = [*POLICY["compile_args"], POLICY["sysroot_option"], report["sdk_path"]]
            xml = output / POLICY["export_xml"]
            command(POLICY["export_label"], [binary, POLICY["export_filter"],
                                            POLICY["export_xml_option"] + str(xml)],
                    POLICY["export_timeout"], dict(os.environ, **{POLICY["export_environment"]: str(output)}))
            check_export(xml.read_text())
            report["export_xml_sha256"] = native.digest(xml)
            report["export_sources_sha256"] = {name: native.digest(output / name)
                                                for name in POLICY["export_sources"]}
            contract = native.fixture_contract()
            loader_source, loader = output / POLICY["loader_source"], output / POLICY["loader_binary"]
            loader_source.write_text(contract["loader"])
            products.append(loader)
            command(POLICY["loader_binary"], [compiler, *compile_args, *POLICY["cxx_args"],
                                              loader_source, POLICY["output_option"], loader])
            report["loader"] = native.check_binary(loader)
            for optimization in POLICY["optimizations"]:
                for dwarf in POLICY["dwarf_modes"]:
                    obj = output / POLICY["object_name"].format(optimization=optimization, dwarf=dwarf)
                    products.append(obj)
                    source = POLICY["dwarf_entry_source"] if dwarf else POLICY["entry_source"]
                    command(obj.name, [compiler, *compile_args, optimization,
                                       *POLICY["object_args"], output / source,
                                       POLICY["output_option"], obj])
                    object_identity = check_classic(obj)
                    for key in POLICY["keys"]:
                        fields = dict(optimization=optimization, key=key, dwarf=dwarf)
                        label = POLICY["case_name"].format(**fields)
                        library = output / POLICY["library_name"].format(**fields)
                        products.append(library)
                        args = [compiler, *compile_args, *POLICY["cxx_args"], optimization,
                                *POLICY["library_args"], POLICY["key_option"].format(key=key)]
                        if dwarf:
                            args.append(POLICY["dwarf_option"])
                        command(label + native.POLICY["compile_suffix"],
                                [*args, output / POLICY["caller_source"], obj,
                                 POLICY["output_option"], library])
                        command(label + native.POLICY["signature_suffix"],
                                [*native.POLICY["signature_command"], library])
                        assembly = command(label + native.POLICY["disassembly_suffix"],
                                           [*native.POLICY["disassembly_command"], library])
                        check_disassembly(assembly.stdout, key)
                        sections = command(label + POLICY["sections_suffix"],
                                           [*POLICY["sections_command"], library])
                        check_sections(sections.stdout, dwarf)
                        unwind = command(label + POLICY["unwind_suffix"],
                                         [*POLICY["unwind_command"], library])
                        check_unwind(unwind.stdout, dwarf)
                        row = {**fields, **check_classic(library),
                               "object": object_identity, "executed": False}
                        report["cases"].append(row)
                        if not compile_only:
                            command(label, [loader, library], POLICY["execute_timeout"])
                            row["executed"] = True
            if not compile_only and not all(row["executed"] for row in report["cases"]):
                raise ValueError(POLICY["errors"]["matrix"])
            report["native_reconstructed_entry_qualified"] = not compile_only
            report["status"] = native.POLICY["passed"]
        return report
    finally:
        native.save(output / POLICY["report_name"], report)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(POLICY["output_argument"], type=Path, required=True)
    parser.add_argument(POLICY["binary_argument"], type=Path, required=True)
    parser.add_argument(POLICY["compiler_argument"], type=Path, required=True)
    parser.add_argument(POLICY["probe_argument"], type=Path)
    parser.add_argument(POLICY["offline_argument"], action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    try:
        # clang++ may be a symlink to clang; argv[0] selects C++ driver mode.
        report = run(output, args.test_binary.resolve(), args.compiler.absolute(),
                     args.probe_report.resolve() if args.probe_report else None,
                     compile_only=args.compile_only)
        print(json.dumps({"status": report["status"], "report": str(output / POLICY["report_name"])}))
        return native.POLICY["exit_codes"][report["status"]]
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        native.save(output / native.POLICY["error_name"], {"error": str(error)})
        print(error)
        return native.POLICY["exit_codes"][native.POLICY["failed"]]


if __name__ == "__main__":
    raise SystemExit(main())
