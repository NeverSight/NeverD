#!/usr/bin/env python3
"""Validate reconstructed entries through native compilation and image publication."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import xml.etree.ElementTree as ET

from arm64e_artifacts import OwnedProducts
import verify_arm64e_native as native

POLICY = json.loads(Path(__file__).with_suffix(".def").read_text())


def check_export(xml, case=None):
    cases = list(ET.fromstring(xml).iter(native.POLICY["testcase_tag"]))
    if (len(cases) != 1 or
            cases[0].attrib.get("name") != (case or POLICY["export_case"]) or
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


def publication_layout(optimization, dwarf, zero_base=False):
    if zero_base:
        return POLICY["tight_publication_layout"]
    for rule in POLICY["publication_layout_overrides"]:
        if rule["optimization"] == optimization and rule["dwarf"] == dwarf:
            return rule["layout"]
    return POLICY["tight_publication_layout"]


def exception_frames():
    return [name for entry in POLICY["exception_entries"]
            for name in (entry, entry + POLICY["carrier_suffix"])]


def check_disassembly(text, key, installed=None):
    functions = {}
    current = None
    for line in text.splitlines():
        match = re.fullmatch(POLICY["function_pattern"], line)
        if match:
            current = match.group(1)
            functions[current] = []
        elif current:
            functions[current].append(line)
    if installed is not None:
        functions = {}
        addresses = {address: name for name, address in installed.items()}
        current = None
        for line in text.splitlines():
            match = re.fullmatch(POLICY["addressed_instruction_pattern"], line)
            if not match:
                continue
            address = int(match.group(1), POLICY["hex_radix"])
            if address in addresses:
                current = addresses[address]
                functions[current] = []
            if current:
                functions[current].append(match.group(2))
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


def image_base(text):
    addresses = re.findall(POLICY["text_base_pattern"], text, re.MULTILINE)
    if len(addresses) != 1:
        raise ValueError(POLICY["errors"]["image_base"])
    return int(addresses[0], POLICY["hex_radix"])


def check_unwind(text, dwarf, installed=None, base=0):
    symbols = {name: int(address, POLICY["hex_radix"])
               for address, name in re.findall(POLICY["symbol_pattern"], text, re.MULTILINE)}
    if installed is not None:
        symbols.update(installed)
    records = {base + int(address, POLICY["hex_radix"]): int(encoding, POLICY["hex_radix"])
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


def check_receipt(receipt):
    entries = receipt.get("entries", [])
    functions = receipt.get("functions", {})
    text = receipt.get("text", {})
    if (any(type(text.get(key)) is not int for key in ("address", "offset", "size")) or
            text["address"] <= 0 or text["offset"] < 0 or text["size"] <= 0):
        raise ValueError(POLICY["errors"]["receipt"])
    if (len(entries) != POLICY["public_entry_count"] or
            set(row.get("symbol") for row in entries) != set(POLICY["public_entries"]) or
            len({row.get("original") for row in entries}) != len(entries) or
            len({row.get("installed") for row in entries}) != len(entries) or
            not all(type(address) is int and text["address"] <= address < text["address"] + text["size"]
                    for address in functions.values()) or
            not set(exception_frames()).issubset(functions) or
            any(type(row.get("original")) is not int or row["original"] <= 0 or
                row.get("original") == row.get("installed") or
                functions.get(row.get("symbol")) != row.get("installed") for row in entries)):
        raise ValueError(POLICY["errors"]["receipt"])
    return functions


def check_execution(result, *, original=False):
    if (result.returncode not in (POLICY["baseline_returncodes"] if original else [0]) or
            POLICY["caller_entry_marker"] not in result.stdout or
            (POLICY["caller_pass_marker"] in result.stdout) == original):
        raise ValueError(POLICY["errors"]["baseline" if original else "execution"])


def inspect_installed_text(output, library, receipt, compiler, compile_args, command, products, label):
    # Installed code is an RX segment without a section descriptor. Package
    # its exact final bytes in an inspection-only object for Apple's disassembler;
    # neither the published image nor its instructions are modified.
    region = receipt["text"]
    contents = library.read_bytes()[region["offset"]:region["offset"] + region["size"]]
    if len(contents) != region["size"]:
        raise ValueError(POLICY["errors"]["inspection"])
    source = output / (label + POLICY["inspection_source_suffix"])
    rows = [POLICY["inspection_prefix"]]
    width = POLICY["inspection_row_bytes"]
    for offset in range(0, len(contents), width):
        data = ",".join(POLICY["inspection_byte"].format(byte=byte) for byte in contents[offset:offset + width])
        rows.append(POLICY["inspection_row"].format(data=data))
    source.write_text("".join(rows))
    obj = output / (label + POLICY["inspection_object_suffix"])
    products.append(obj)
    command(label + POLICY["inspection_suffix"] + native.POLICY["compile_suffix"],
            [compiler, *compile_args, *POLICY["object_args"], source, POLICY["output_option"], obj])
    assembly = command(label + POLICY["inspection_suffix"] + native.POLICY["disassembly_suffix"],
                       [*POLICY["publication_disassembly_command"], obj])
    return assembly.stdout, hashlib.sha256(contents).hexdigest()


def run(output, binary, compiler, probe_report=None, *, compile_only=False, producer=None, publish=False, zero_base=False):
    producer = producer or POLICY["producers"][0]
    backend = producer == POLICY["backend_producer"]
    provenance = native.provenance()
    report = {**provenance, "status": native.POLICY["failed"],
              "entry_source_sha256": {name: native.digest(native.ROOT / name)
                                      for name in POLICY["source_files"]},
              "native_reconstructed_entry_qualified": False,
              "neverd_codegen_qualified": False,
              "native_apple_cpta_qualified": False,
              "whole_image_publication_qualified": False,
              "compile_only": compile_only, "producer": producer, "publish": publish,
              "optimization_scope": POLICY["optimization_scopes"].get(producer),
              "publication_layout": POLICY["publication_layout"] if publish and not zero_base else
                  POLICY["tight_publication_layout"]["name"] if publish else None,
              "reconstructed_entry_publication_qualified": False, "cases": []}
    try:
        if producer not in POLICY["producers"] or (publish and not backend) or (zero_base and not publish):
            raise ValueError(POLICY["errors"]["producer"])
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
            environment = {**os.environ, POLICY["export_environment"]: str(output)}
            # The host exports objects only when explicitly requested. Register
            # them before export so a failed CFI check also cleans partial output.
            environment.pop(POLICY["backend_environment"], None)
            if backend:
                environment[POLICY["backend_environment"]] = POLICY["enabled_environment_value"]
                for name in (POLICY["backend_object"], POLICY["backend_dwarf_object"]):
                    products.append(output / name)
            command(POLICY["export_label"], [binary, POLICY["export_filter"],
                                            POLICY["export_xml_option"] + str(xml)],
                    POLICY["export_timeout"], environment)
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
                    source = POLICY["dwarf_entry_source"] if dwarf else POLICY["entry_source"]
                    if backend:
                        obj = output / POLICY["backend_dwarf_object" if dwarf else "backend_object"]
                    else:
                        obj = output / POLICY["object_name"].format(optimization=optimization, dwarf=dwarf)
                        products.append(obj)
                        command(obj.name, [compiler, *compile_args, optimization,
                                           *POLICY["object_args"], output / source,
                                           POLICY["output_option"], obj])
                    original_obj = None
                    if publish:
                        original_obj = output / POLICY["original_object_name"].format(dwarf=dwarf)
                        products.append(original_obj)
                        original_ir = output / POLICY["original_dwarf_ir" if dwarf else "original_ir"]
                        command(original_obj.name, [compiler, *compile_args, optimization,
                                                    *POLICY["object_args"], original_ir,
                                                    POLICY["output_option"], original_obj])
                    object_identity = check_classic(obj)
                    for key in POLICY["keys"]:
                        fields = dict(optimization=optimization, key=key, dwarf=dwarf)
                        layout = publication_layout(optimization, dwarf, zero_base)
                        label = POLICY["case_name"].format(**fields)
                        library = output / POLICY["library_name"].format(**fields)
                        products.append(library)
                        args = [compiler, *compile_args, *POLICY["cxx_args"], optimization,
                                *POLICY["library_args"], POLICY["key_option"].format(key=key)]
                        if dwarf:
                            args.append(POLICY["dwarf_option"])
                        if not publish:
                            command(label + native.POLICY["compile_suffix"],
                                    [*args, output / POLICY["caller_source"], obj,
                                     POLICY["output_option"], library])
                        installed = None
                        receipt = None
                        baseline = None
                        original_base = None
                        if publish:
                            library = output / POLICY["installed_library_name"].format(**fields)
                            products.append(library)
                            receipt_path = output / (label + POLICY["receipt_suffix"])
                            publication_xml = output / (label + POLICY["publication_xml_suffix"])
                            env = {**os.environ, POLICY["publication_output_environment"]: str(library),
                                   POLICY["publication_receipt_environment"]: str(receipt_path)}
                            env.pop(POLICY["publication_dwarf_environment"], None)
                            if dwarf:
                                env[POLICY["publication_dwarf_environment"]] = POLICY["enabled_environment_value"]
                            occupied = output / POLICY["occupied_library_name"].format(**fields)
                            products.append(occupied)
                            command(label + POLICY["occupied_suffix"] + native.POLICY["compile_suffix"],
                                    [*args, *POLICY["occupied_header_args"], output / POLICY["caller_source"], original_obj,
                                     POLICY["output_option"], occupied])
                            refusal_xml = output / (label + POLICY["occupied_suffix"] + POLICY["publication_xml_suffix"])
                            env[POLICY["publication_input_environment"]] = str(occupied)
                            command(label + POLICY["occupied_suffix"],
                                    [binary, POLICY["occupied_filter"],
                                     POLICY["export_xml_option"] + str(refusal_xml)],
                                    POLICY["export_timeout"], env)
                            check_export(refusal_xml.read_text(), POLICY["occupied_case"])
                            original = output / POLICY["original_library_name"].format(**fields)
                            products.append(original)
                            command(label + POLICY["baseline_suffix"] + native.POLICY["compile_suffix"],
                                    [*args, *layout["header_args"],
                                     output / POLICY["caller_source"], original_obj,
                                     POLICY["output_option"], original])
                            before = command(label + POLICY["baseline_suffix"] + POLICY["sections_suffix"],
                                             [*POLICY["sections_command"], original])
                            original_base = image_base(before.stdout)
                            if not compile_only:
                                control = native.command(products, output, label + POLICY["baseline_suffix"],
                                                         [str(loader), str(original)], POLICY["execute_timeout"])
                                baseline = control.returncode
                                check_execution(control, original=True)
                            env[POLICY["publication_input_environment"]] = str(original)
                            command(label + POLICY["publication_suffix"],
                                    [binary, POLICY["publication_filter"],
                                     POLICY["export_xml_option"] + str(publication_xml)],
                                    POLICY["export_timeout"], env)
                            check_export(publication_xml.read_text(), POLICY["publication_case"])
                            receipt = json.loads(receipt_path.read_text())
                            installed = check_receipt(receipt)
                        command(label + native.POLICY["signature_suffix"],
                                [*native.POLICY["signature_command"], library])
                        assembly = command(label + native.POLICY["disassembly_suffix"],
                                           [*native.POLICY["disassembly_command"], library])
                        text_hash = None
                        if publish:
                            text_assembly, text_hash = inspect_installed_text(
                                output, library, receipt, compiler, compile_args, command, products, label)
                            relative = {name: address - receipt["text"]["address"]
                                        for name, address in installed.items()}
                            check_disassembly(text_assembly + assembly.stdout, key, relative)
                        else:
                            check_disassembly(assembly.stdout, key)
                        sections = command(label + POLICY["sections_suffix"],
                                           [*POLICY["sections_command"], library])
                        check_sections(sections.stdout, dwarf)
                        unwind = command(label + POLICY["unwind_suffix"],
                                         [*POLICY["unwind_command"], library])
                        base = image_base(sections.stdout)
                        if zero_base and (original_base != 0 or base != 0):
                            raise ValueError(POLICY["errors"]["zero_base"])
                        if publish and layout.get("require_prefix") and base >= original_base:
                            raise ValueError(POLICY["errors"]["prefix"])
                        check_unwind(unwind.stdout, dwarf, installed, base)
                        row = {**fields, **check_classic(library),
                               "object": object_identity, "executed": False,
                               "publication_layout": layout if publish else None,
                               "publication": receipt, "baseline_returncode": baseline,
                               "image_base": base, "original_image_base": original_base,
                               "occupied_original_rejected": publish, "installed_text_sha256": text_hash}
                        report["cases"].append(row)
                        if not compile_only:
                            execution = command(label, [loader, library], POLICY["execute_timeout"])
                            check_execution(execution)
                            row["executed"] = True
            if not compile_only and not all(row["executed"] for row in report["cases"]):
                raise ValueError(POLICY["errors"]["matrix"])
            report["native_reconstructed_entry_qualified"] = not compile_only
            report["neverd_codegen_qualified"] = backend and not compile_only
            report["reconstructed_entry_publication_qualified"] = publish and not compile_only
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
    parser.add_argument(POLICY["producer_argument"], choices=POLICY["producers"],
                        default=POLICY["producers"][0])
    parser.add_argument(POLICY["publication_argument"], action="store_true")
    parser.add_argument(POLICY["zero_base_argument"], action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    try:
        # clang++ may be a symlink to clang; argv[0] selects C++ driver mode.
        report = run(output, args.test_binary.resolve(), args.compiler.absolute(),
                     args.probe_report.resolve() if args.probe_report else None,
                     compile_only=args.compile_only, producer=args.producer, publish=args.publish,
                     zero_base=args.zero_base)
        print(json.dumps({"status": report["status"], "report": str(output / POLICY["report_name"])}))
        return native.POLICY["exit_codes"][report["status"]]
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        native.save(output / native.POLICY["error_name"], {"error": str(error)})
        print(error)
        return native.POLICY["exit_codes"][native.POLICY["failed"]]


if __name__ == "__main__":
    raise SystemExit(main())
