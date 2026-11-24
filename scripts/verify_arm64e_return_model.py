#!/usr/bin/env python3
"""Build and execute Darwin arm64e return, frame-walk and ordered-memory oracles on Arm FVP."""

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


def export_cases(binary, scratch, names, environment, artifact_property, extra_env=None):
    report = scratch / CONFIG["report"]
    objects = scratch / CONFIG["test_object_directory"]
    subprocess.run([binary, CONFIG["test_filter_flag"] + ":".join(names),
                    CONFIG["test_report_flag"] + str(report)], check=True,
                   env={**os.environ, **(extra_env or {}), environment: str(objects)})
    exported = {}
    for suite in json.loads(report.read_text())["testsuites"]:
        for case in suite["testsuite"]:
            name = suite["name"] + "." + case["name"]
            if case["result"] != CONFIG["test_completed"] or name in exported:
                raise ValueError(CONFIG["test_failed"].format(name=name))
            path = Path(case[artifact_property]).resolve()
            if path.parent != objects:
                raise ValueError(CONFIG["bad_export"].format(path=path))
            exported[name] = path
    if set(exported) != set(names):
        raise ValueError(CONFIG["bad_tests"].format(names=list(exported)))
    return exported


def compile_object(source, obj, flags=()):
    subprocess.run([*CONFIG["compiler"], CONFIG["target"], CONFIG["march"], *flags,
                    CONFIG["compile_only"], source, CONFIG["output_option"], obj], check=True)


def link_guest(guest, entry, driver, obj, support=()):
    subprocess.run([*CONFIG["linker"], *CONFIG["link_flags"], CONFIG["segment_address"],
                    CONFIG["text_segment"], CONFIG["image_base"], CONFIG["output_option"], guest,
                    entry, driver, *support, obj], check=True)
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
    return {"entry": hex(start), "sha256": digest(guest),
            "object_sha256": digest(obj)}


def build(binary, output):
    if output.exists():
        raise FileExistsError(output)
    source = ROOT / CONFIG["guest_directory"]
    fixtures = ROOT.parent / CONFIG["fixture_directory"]
    manifest = {"test_binary_sha256": digest(binary), "guests": {},
                "sources": {CONFIG[key]: digest(source / CONFIG[key])
                            for key in ("guest_entry", "guest_driver", "guest_constants")}}
    manifest["fixtures"] = {name: digest(fixtures / name) for name in CONFIG["fixture_files"]}
    with tempfile.TemporaryDirectory(dir=output.parent) as temporary:
        scratch = Path(temporary).resolve()
        staged = scratch / CONFIG["staging_directory"]
        staged.mkdir()
        names = {family: spec["test"] for family, spec in CONFIG["cases"].items()}
        exports = export_cases(binary, scratch, names.values(), CONFIG["artifact_environment"],
                               CONFIG["artifact_property"], {CONFIG["fixture_environment"]: str(fixtures)})
        entry = scratch / CONFIG["entry_object"]
        compile_object(source / CONFIG["guest_entry"], entry)
        drivers = {}
        for family in names:
            driver = scratch / (family + CONFIG["test_object_extension"])
            flags = CONFIG["cases"][family]["defines"]
            compile_object(source / CONFIG["guest_driver"], driver, [*CONFIG["driver_flags"], *flags])
            drivers[family] = driver
        for family, route, optimization in cases():
            name = "-".join((family, route, optimization))
            obj = exports[names[family]] / route / (optimization + CONFIG["test_object_extension"])
            check_macho(obj, CONFIG["object_kind"])
            guest = staged / name
            manifest["guests"][name] = link_guest(guest, entry, drivers[family], obj)
        write_json(staged / CONFIG["manifest"], manifest)
        staged.rename(output)


def build_cursor(output):
    if output.exists():
        raise FileExistsError(output)
    cursor = CONFIG["cursor"]
    source = ROOT / CONFIG["guest_directory"]
    native = json.loads((ROOT / cursor["native_policy"]).read_text())["cursor_probe"]
    fixture_dir = (ROOT.parent / native["source"]).parent
    inputs = [source / CONFIG[key] for key in ("guest_entry", "guest_driver", "guest_constants")]
    inputs.append(source / cursor["driver"])
    inputs.extend(ROOT.parent / path for path in native["source_files"])
    manifest = {"guests": {}, "sources": {str(path.relative_to(ROOT.parent)): digest(path) for path in inputs}}
    with tempfile.TemporaryDirectory(dir=output.parent) as temporary:
        scratch = Path(temporary)
        staged = scratch / CONFIG["staging_directory"]
        staged.mkdir()
        entry = scratch / CONFIG["entry_object"]
        compile_object(source / CONFIG["guest_entry"], entry, [cursor["define"]])
        driver = scratch / (Path(CONFIG["guest_driver"]).stem + CONFIG["test_object_extension"])
        compile_object(source / CONFIG["guest_driver"], driver, [*CONFIG["driver_flags"], cursor["define"]])
        for optimization in CONFIG["optimizations"]:
            name = cursor["name"].format(optimization=optimization)
            obj = scratch / (name + CONFIG["test_object_extension"])
            flags = [cursor["define"], *cursor["flags"], cursor["optimization_option"].format(optimization=optimization),
                     cursor["include_option"], ROOT.parent / native["include"], cursor["include_option"], fixture_dir]
            compile_object(source / cursor["driver"], obj, flags)
            check_macho(obj, CONFIG["object_kind"])
            manifest["guests"][name] = link_guest(staged / name, entry, driver, obj)
        write_json(staged / CONFIG["manifest"], manifest)
        staged.rename(output)


def ordered_routes(policy):
    for route, source in policy["sources"].items():
        for optimization in CONFIG["optimizations"]:
            yield route + "-" + optimization, source, "-" + optimization
    yield policy["object_route"], policy["object_file"], None


def build_ordered(binary, output):
    if output.exists():
        raise FileExistsError(output)
    policy = CONFIG["ordered"]
    source = ROOT / CONFIG["guest_directory"]
    inputs = [CONFIG[key] for key in ("guest_entry", "guest_driver", "guest_constants")]
    inputs.extend((policy["driver"], policy["constants"]))
    manifest = {"test_binary_sha256": digest(binary), "guests": {},
                "sources": {name: digest(source / name) for name in inputs}}
    with tempfile.TemporaryDirectory(dir=output.parent) as temporary:
        scratch = Path(temporary).resolve()
        staged = scratch / CONFIG["staging_directory"]
        staged.mkdir()
        exports = export_cases(binary, scratch, [policy["test"]],
                               policy["artifact_environment"], policy["artifact_property"])[policy["test"]]
        manifest["exports"] = {name: digest(exports / name)
                               for name in [*policy["sources"].values(), policy["object_file"]]}
        entry = scratch / CONFIG["entry_object"]
        flags = [*CONFIG["driver_flags"], policy["define"], policy["alignment_flag"]]
        compile_object(source / CONFIG["guest_entry"], entry, [policy["define"]])
        driver = scratch / (Path(CONFIG["guest_driver"]).stem + CONFIG["test_object_extension"])
        oracle = scratch / (Path(policy["driver"]).stem + CONFIG["test_object_extension"])
        compile_object(source / CONFIG["guest_driver"], driver, flags)
        compile_object(source / policy["driver"], oracle, [*flags, policy["march"]])
        for label, filename, optimization in ordered_routes(policy):
            obj = exports / filename
            if optimization:
                obj = scratch / (label + CONFIG["test_object_extension"])
                compile_object(exports / filename, obj, [*CONFIG["driver_flags"], optimization])
            check_macho(obj, CONFIG["object_kind"])
            manifest["guests"][label] = link_guest(staged / label, entry, driver, obj, [oracle])
        write_json(staged / CONFIG["manifest"], manifest)
        staged.rename(output)


def run(model, guests, output, cursor=False, ordered=False):
    output.mkdir()
    version = subprocess.check_output([model, *CONFIG["version_flags"]], text=True, stderr=subprocess.STDOUT)
    match = re.search(CONFIG["version_pattern"], version)
    if not match or match[1] != CONFIG["model_version"]:
        raise ValueError(CONFIG["bad_version"].format(version=version.strip()))
    manifest = json.loads((guests / CONFIG["manifest"]).read_text())
    policy = CONFIG["ordered"] if ordered else CONFIG
    if ordered:
        names = [label for label, _, _ in ordered_routes(policy)]
    elif cursor:
        names = [CONFIG["cursor"]["name"].format(optimization=opt) for opt in CONFIG["optimizations"]]
    else:
        names = ["-".join(case) for case in cases()]
    if set(manifest["guests"]) != set(names):
        raise ValueError(CONFIG["bad_inventory"])
    result = {"model_version": CONFIG["model_version"], "model_sha256": digest(model),
              "manifest_sha256": digest(guests / CONFIG["manifest"]), "observations": {}}
    negative_case = names[0] if cursor or ordered else CONFIG["negative_case"]
    for label in [negative_case + CONFIG["negative_suffix"], *names]:
        name = label.removesuffix(CONFIG["negative_suffix"])
        negative = name != label
        guest = guests / name
        item = manifest["guests"][name]
        if digest(guest) != item["sha256"]:
            raise ValueError(CONFIG["bad_hash"].format(name=name))
        params = {**CONFIG["parameters"], CONFIG["reset_parameter"]: item["entry"]}
        if ordered:
            params.update(policy["parameters"])
        if negative:
            params.update(policy["negative_parameters"])
        arguments = [model, CONFIG["model_data"], f'{guest}@{CONFIG["image_base"]}',
                     CONFIG["model_timelimit"], str(CONFIG["model_timeout"])]
        for key, value in params.items():
            arguments.extend([CONFIG["model_parameter"], f"{key}={value}"])
        log = output / (label + CONFIG["model_log_extension"])
        with log.open("w") as stream:
            observed = subprocess.run(arguments, stdout=stream, stderr=subprocess.STDOUT,
                                      timeout=CONFIG["process_timeout"])
        records = [line for line in log.read_text().splitlines()
                   if line.startswith(policy["record_prefix"])]
        expected = ([policy["feature_mismatch"]] if negative
                    else [*policy["control_records"], policy["passed"]])
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
    ordered_link = commands.add_parser(options["build_ordered"])
    for command in (link, ordered_link):
        command.add_argument(options["tests"], type=Path, required=True)
    cursor_link = commands.add_parser(options["build_cursor"])
    execute = commands.add_parser(options["run"])
    cursor_execute = commands.add_parser(options["run_cursor"])
    ordered_execute = commands.add_parser(options["run_ordered"])
    for command in (execute, cursor_execute, ordered_execute):
        command.add_argument(options["model"], type=Path, required=True)
        command.add_argument(options["guests"], type=Path, required=True)
    for command in (link, cursor_link, ordered_link, execute, cursor_execute, ordered_execute):
        command.add_argument(options["output"], type=Path, required=True)
    args = parser.parse_args()
    if args.command == options["build"]:
        build(args.tests.resolve(), args.output.resolve())
    elif args.command == options["build_ordered"]:
        build_ordered(args.tests.resolve(), args.output.resolve())
    elif args.command == options["build_cursor"]:
        build_cursor(args.output.resolve())
    else:
        run(args.model.resolve(), args.guests.resolve(), args.output.resolve(),
            cursor=args.command == options["run_cursor"], ordered=args.command == options["run_ordered"])
