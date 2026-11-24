#!/usr/bin/env python3
"""Link the actual arm64e oracle objects on Darwin and execute them on Arm FVP."""

import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent
CONFIG = json.loads(Path(__file__).with_suffix(".def").read_text())


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def cases():
    for family in CONFIG["cases"]:
        for route in CONFIG["routes"]:
            for optimization in CONFIG["optimizations"]:
                yield family, route, optimization


def check_macho(path, kind):
    header = struct.unpack_from(CONFIG["macho_header"], path.read_bytes())
    magic, cpu, subtype, filetype = header
    if (magic != CONFIG["macho_magic"] or cpu != CONFIG["arm64_cpu"] or
            subtype & CONFIG["subtype_mask"] != CONFIG["arm64e_subtype"] or filetype != kind):
        raise ValueError(CONFIG["bad_header"].format(path=path, header=header))


def export_objects(binary, scratch, fixtures):
    report = scratch / CONFIG["report"]
    objects = scratch / CONFIG["test_object_directory"]
    names = {family: spec["test"] for family, spec in CONFIG["cases"].items()}
    subprocess.run([binary, CONFIG["test_filter_flag"] + ":".join(names.values()),
                    CONFIG["test_report_flag"] + str(report)], check=True,
                   env={**os.environ, CONFIG["artifact_environment"]: str(objects),
                        CONFIG["fixture_environment"]: str(fixtures)})
    exported = {}
    for suite in json.loads(report.read_text())["testsuites"]:
        for case in suite["testsuite"]:
            name = suite["name"] + "." + case["name"]
            if case["result"] != CONFIG["test_completed"] or name in exported:
                raise ValueError(CONFIG["test_failed"].format(name=name))
            path = Path(case[CONFIG["artifact_property"]]).resolve()
            if path.parent != objects:
                raise ValueError(CONFIG["bad_export"].format(path=path))
            exported[name] = path
    if set(exported) != set(names.values()):
        raise ValueError(CONFIG["bad_tests"].format(names=list(exported)))
    return {family: exported[name] for family, name in names.items()}


def build(binary, output):
    if output.exists():
        raise FileExistsError(output)
    source = ROOT / CONFIG["guest_directory"]
    fixtures = ROOT.parent / CONFIG["fixture_directory"]
    compiler = [*CONFIG["compiler"], CONFIG["target"], CONFIG["march"]]
    manifest = {"test_binary_sha256": digest(binary), "guests": {},
                "sources": {CONFIG[key]: digest(source / CONFIG[key])
                            for key in ("guest_entry", "guest_driver", "guest_constants")}}
    manifest["fixtures"] = {name: digest(fixtures / name) for name in CONFIG["fixture_files"]}
    with tempfile.TemporaryDirectory(dir=output.parent) as temporary:
        scratch = Path(temporary).resolve()
        staged = scratch / CONFIG["staging_directory"]
        staged.mkdir()
        exports = export_objects(binary, scratch, fixtures)
        entry = scratch / CONFIG["entry_object"]
        subprocess.run([*compiler, CONFIG["compile_only"], source / CONFIG["guest_entry"], CONFIG["output_option"], entry], check=True)
        drivers = {}
        for family in exports:
            driver = scratch / (family + CONFIG["test_object_extension"])
            flags = CONFIG["cases"][family]["defines"]
            subprocess.run([*compiler, *CONFIG["driver_flags"], *flags, CONFIG["compile_only"],
                            source / CONFIG["guest_driver"], CONFIG["output_option"], driver], check=True)
            drivers[family] = driver
        for family, route, optimization in cases():
            name = "-".join((family, route, optimization))
            obj = exports[family] / route / (optimization + CONFIG["test_object_extension"])
            check_macho(obj, CONFIG["object_kind"])
            guest = staged / name
            subprocess.run([*CONFIG["linker"], *CONFIG["link_flags"], CONFIG["segment_address"],
                            CONFIG["text_segment"], CONFIG["image_base"], CONFIG["output_option"], guest,
                            entry, drivers[family], obj], check=True)
            check_macho(guest, CONFIG["executable_kind"])
            # FVP loads the linked file at its physical base without a Darwin loader.
            layout = subprocess.check_output([*CONFIG["segments"], guest], text=True)
            segments = re.findall(CONFIG["size_pattern"], layout, re.MULTILINE)
            base = int(CONFIG["image_base"], 0)
            if (len(segments) != sum(line.startswith(CONFIG["segment_prefix"]) for line in layout.splitlines()) or
                    not any(s == CONFIG["text_segment"] for s, _, _ in segments) or
                    any(int(address, 0) != base + int(offset) for _, address, offset in segments)):
                raise ValueError(CONFIG["bad_map"].format(path=guest))
            symbols = subprocess.check_output([*CONFIG["symbols"], guest], text=True)
            start, = (int(row.split()[0], 16) for row in symbols.splitlines()
                      if row.split()[-1] == CONFIG["reset_symbol"])
            manifest["guests"][name] = {"entry": hex(start), "sha256": digest(guest),
                                       "object_sha256": digest(obj)}
        write_json(staged / CONFIG["manifest"], manifest)
        staged.rename(output)


def run(model, guests, output):
    output.mkdir()
    version = subprocess.check_output([model, *CONFIG["version_flags"]], text=True, stderr=subprocess.STDOUT)
    match = re.search(CONFIG["version_pattern"], version)
    if not match or match[1] != CONFIG["model_version"]:
        raise ValueError(CONFIG["bad_version"].format(version=version.strip()))
    manifest = json.loads((guests / CONFIG["manifest"]).read_text())
    names = ["-".join(case) for case in cases()]
    if set(manifest["guests"]) != set(names):
        raise ValueError(CONFIG["bad_inventory"])
    result = {"model_version": CONFIG["model_version"], "model_sha256": digest(model),
              "manifest_sha256": digest(guests / CONFIG["manifest"]), "observations": {}}
    for label in [CONFIG["negative_case"] + CONFIG["negative_suffix"], *names]:
        name = label.removesuffix(CONFIG["negative_suffix"])
        negative = name != label
        guest = guests / name
        item = manifest["guests"][name]
        if digest(guest) != item["sha256"]:
            raise ValueError(CONFIG["bad_hash"].format(name=name))
        params = {**CONFIG["parameters"], CONFIG["reset_parameter"]: item["entry"]}
        if negative:
            params.update(CONFIG["negative_parameters"])
        arguments = [model, CONFIG["model_data"], f'{guest}@{CONFIG["image_base"]}',
                     CONFIG["model_timelimit"], str(CONFIG["model_timeout"])]
        for key, value in params.items():
            arguments.extend([CONFIG["model_parameter"], f"{key}={value}"])
        log = output / (label + CONFIG["model_log_extension"])
        with log.open("w") as stream:
            observed = subprocess.run(arguments, stdout=stream, stderr=subprocess.STDOUT,
                                      timeout=CONFIG["process_timeout"])
        records = [line for line in log.read_text().splitlines()
                   if line.startswith(CONFIG["record_prefix"])]
        expected = ([CONFIG["feature_mismatch"]] if negative
                    else [*CONFIG["control_records"], CONFIG["passed"]])
        if records != expected or observed.returncode != (CONFIG["unsupported_exit"] if negative else 0):
            raise RuntimeError(CONFIG["bad_observation"].format(
                label=label, status=observed.returncode, records=records))
        result["observations"][label] = {"records": records, "exit_code": observed.returncode}
    write_json(output / CONFIG["report"], result)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    options = CONFIG["commands"]
    link = commands.add_parser(options["build"])
    link.add_argument(options["tests"], type=Path, required=True)
    execute = commands.add_parser(options["run"])
    execute.add_argument(options["model"], type=Path, required=True)
    execute.add_argument(options["guests"], type=Path, required=True)
    for command in (link, execute):
        command.add_argument(options["output"], type=Path, required=True)
    args = parser.parse_args()
    if args.command == options["build"]:
        build(args.tests.resolve(), args.output.resolve())
    else:
        run(args.model.resolve(), args.guests.resolve(), args.output.resolve())
