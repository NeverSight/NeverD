#!/usr/bin/env python3
"""Build and execute Darwin arm64e return, frame-walk and memory oracles on Arm FVP."""

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


def cases(profile):
    if profile not in (CONFIG["previous_return_profile"], CONFIG["return_profile"]):
        raise ValueError(CONFIG["bad_inventory"])
    routes = list(CONFIG["routes"])
    if profile == CONFIG["return_profile"]:
        routes.extend(CONFIG["backend_objects"])
    for family in CONFIG["cases"]:
        for route in routes:
            for optimization in CONFIG["optimizations"]:
                yield family, route, optimization


def check_macho(path, kind, subtype=CONFIG["arm64e_subtype"]):
    header = struct.unpack_from(CONFIG["macho_header"], path.read_bytes())
    magic, cpu, actual_subtype, filetype = header
    if (magic != CONFIG["macho_magic"] or cpu != CONFIG["arm64_cpu"] or
            actual_subtype & CONFIG["subtype_mask"] != subtype or filetype != kind):
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


def compile_object(source, obj, flags=(), target=CONFIG["target"]):
    subprocess.run([*CONFIG["compiler"], target, CONFIG["march"], *flags,
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
                "return_profile": CONFIG["return_profile"],
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
        for family, route, optimization in cases(manifest["return_profile"]):
            name = "-".join((family, route, optimization))
            directory = exports[names[family]]
            compiled = optimization + CONFIG["test_object_extension"]
            support = []
            if route in CONFIG["backend_objects"]:
                obj = directory / CONFIG["backend_objects"][route]
                caller = directory / CONFIG["caller_directory"] / compiled
                check_macho(caller, CONFIG["object_kind"])
                support.append(caller)
            else:
                obj = directory / route / compiled
            check_macho(obj, CONFIG["object_kind"])
            item = link_guest(staged / name, entry, drivers[family], obj, support)
            if support:
                item[CONFIG["caller_hash_field"]] = digest(caller)
            manifest["guests"][name] = item
        write_json(staged / CONFIG["manifest"], manifest)
        staged.rename(output)


def build_signing_address(binary, output):
    if output.exists():
        raise FileExistsError(output)
    profile = CONFIG["signing_address"]
    source = ROOT / CONFIG["guest_directory"]
    with tempfile.TemporaryDirectory(dir=output.parent) as temporary:
        scratch = Path(temporary).resolve()
        staged = scratch / CONFIG["staging_directory"]
        staged.mkdir()
        exported = export_cases(binary, scratch, [profile["test"]], profile["environment"],
                                profile["artifact_property"])
        assembly = exported[profile["test"]]
        report = json.loads((scratch / CONFIG["report"]).read_text())
        case, = [case for suite in report["testsuites"] for case in suite["testsuite"]]
        if int(case[profile["count_property"]]) != profile["bodies"]:
            raise ValueError(CONFIG["bad_inventory"])
        entry = scratch / CONFIG["entry_object"]
        driver = scratch / (Path(CONFIG["guest_driver"]).stem + CONFIG["test_object_extension"])
        obj = assembly.with_suffix(CONFIG["test_object_extension"])
        compile_object(source / CONFIG["guest_entry"], entry)
        compile_object(source / CONFIG["guest_driver"], driver, CONFIG["driver_flags"])
        compile_object(assembly, obj)
        name = profile["profile"]
        guest = link_guest(staged / name, entry, driver, obj)
        write_json(staged / CONFIG["manifest"], {"return_profile": name, "guests": {name: guest},
                   "test_binary_sha256": digest(binary), profile["assembly_hash_property"]: digest(assembly),
                   profile["count_property"]: profile["bodies"]})
        staged.rename(output)


def build_control_guests(output, family):
    if output.exists():
        raise FileExistsError(output)
    profile = CONFIG[family]
    source = ROOT / CONFIG["guest_directory"]
    native = json.loads((ROOT / CONFIG["native_policy"]).read_text())[profile["native_probe"]]
    inputs = [source / CONFIG[key] for key in ("guest_entry", "guest_driver", "guest_constants")]
    inputs.append(source / profile["driver"])
    inputs.extend(ROOT.parent / path for path in native["source_files"])
    manifest = {"guests": {}, "sources": {str(path.relative_to(ROOT.parent)): digest(path) for path in inputs}}
    with tempfile.TemporaryDirectory(dir=output.parent) as temporary:
        scratch = Path(temporary)
        staged = scratch / CONFIG["staging_directory"]
        staged.mkdir()
        entry = scratch / CONFIG["entry_object"]
        compile_object(source / CONFIG["guest_entry"], entry, [profile["define"]])
        driver = scratch / (Path(CONFIG["guest_driver"]).stem + CONFIG["test_object_extension"])
        compile_object(source / CONFIG["guest_driver"], driver, [*CONFIG["driver_flags"], profile["define"]])
        flags = list(profile.get("flags", CONFIG["driver_flags"]))
        if "include_option" in profile:
            flags.extend([profile["include_option"], ROOT.parent / native["include"],
                          profile["include_option"], (ROOT.parent / native["source"]).parent])
        for optimization in CONFIG["optimizations"]:
            name = profile["name"].format(optimization=optimization)
            obj = scratch / (name + CONFIG["test_object_extension"])
            compile_object(source / profile["driver"], obj,
                           [profile["define"], *flags, CONFIG["optimization_option"].format(optimization=optimization)])
            check_macho(obj, CONFIG["object_kind"])
            manifest["guests"][name] = link_guest(staged / name, entry, driver, obj)
        write_json(staged / CONFIG["manifest"], manifest)
        staged.rename(output)


def ordered_routes(target, profile):
    policy = CONFIG["ordered"]
    # The first model guests (Q forms only) predate named profiles.
    direct_backends = (target["fixed_input_profile"], target["profile"])
    if profile not in (*direct_backends, target["previous_profile"]) and not (
            "quadword_profile" in target and profile == target["quadword_profile"]):
        raise ValueError(CONFIG["bad_inventory"])
    for route, source in policy["sources"].items():
        for optimization in CONFIG["optimizations"]:
            yield route + "-" + optimization, source, "-" + optimization
    yield policy["object_route"], target["object_file"], None
    if profile in direct_backends:
        yield policy["raw_object_route"], target["raw_object_file"], None


def ordered_input_names(target):
    names = {filename for _, filename, _ in ordered_routes(target, target["profile"])}
    return sorted(names | {CONFIG["ordered"]["dispatch"]})


def export_ordered_cases(binary, scratch):
    policy = CONFIG["ordered"]
    return export_cases(binary, scratch, [policy["test"]],
                        policy["artifact_environment"], policy["artifact_property"])[policy["test"]]


def build_ordered(binary, output, native=False):
    if output.exists():
        raise FileExistsError(output)
    with tempfile.TemporaryDirectory(dir=output.parent) as temporary:
        exports = export_ordered_cases(binary, Path(temporary).resolve())
        link_ordered(exports, digest(binary), output, native=native)


def link_ordered(exports, exporter_sha256, output, *, native):
    policy = CONFIG["ordered"]
    target = policy["native"] if native else policy
    triple = target["target"] if native else CONFIG["target"]
    subtype = target["subtype"] if native else CONFIG["arm64e_subtype"]
    source = ROOT / CONFIG["guest_directory"]
    fixtures = ROOT.parent / policy["fixture_directory"]
    inputs = [policy["driver"], policy["constants"], policy["interface"]]
    inputs.extend([target["driver"], target["probe"]] if native else
                  [CONFIG[key] for key in ("guest_entry", "guest_driver", "guest_constants")])
    manifest = {"test_binary_sha256": exporter_sha256, "guests": {},
                "ordered_profile": target["profile"],
                "fixtures": {policy["fixture"]: digest(fixtures / policy["fixture"])},
                "sources": {name: digest(source / name) for name in inputs},
                "exports": {name: digest(exports / name) for name in ordered_input_names(target)}}
    with tempfile.TemporaryDirectory(dir=output.parent) as temporary:
        scratch = Path(temporary).resolve()
        staged = scratch / CONFIG["staging_directory"]
        staged.mkdir()
        flags = [*CONFIG["driver_flags"], policy["alignment_flag"]]
        if not native:
            flags.append(policy["define"])
            entry = scratch / CONFIG["entry_object"]
            compile_object(source / CONFIG["guest_entry"], entry, [policy["define"]])
        driver_source = target["driver"] if native else CONFIG["guest_driver"]
        driver = scratch / (Path(driver_source).stem + CONFIG["test_object_extension"])
        oracle = scratch / (Path(policy["driver"]).stem + CONFIG["test_object_extension"])
        dispatch = scratch / (Path(policy["dispatch"]).stem + CONFIG["test_object_extension"])
        compile_object(source / driver_source, driver, flags, triple)
        compile_object(source / policy["driver"], oracle,
                       [*flags, policy["march"], policy["include_option"], fixtures], triple)
        compile_object(exports / policy["dispatch"], dispatch, flags, triple)
        if native:
            probe = scratch / (Path(target["probe"]).stem + CONFIG["test_object_extension"])
            compile_object(source / target["probe"], probe, flags, triple)
        for label, filename, optimization in ordered_routes(target, manifest["ordered_profile"]):
            obj = exports / filename
            if optimization:
                obj = scratch / (label + CONFIG["test_object_extension"])
                compile_object(exports / filename, obj, [*CONFIG["driver_flags"], optimization], triple)
            check_macho(obj, CONFIG["object_kind"], subtype)
            image = staged / label
            if native:
                subprocess.run([*CONFIG["compiler"], triple, driver, probe, oracle, dispatch, obj,
                                CONFIG["output_option"], image], check=True)
                check_macho(image, CONFIG["executable_kind"], subtype)
                item = {"sha256": digest(image), "object_sha256": digest(obj)}
            else:
                item = link_guest(image, entry, driver, obj, [oracle, dispatch])
            manifest["guests"][label] = item
        write_json(staged / CONFIG["manifest"], manifest)
        staged.rename(output)


def run_native_ordered(guests, output):
    policy = CONFIG["ordered"]
    target = policy["native"]
    manifest = json.loads((guests / CONFIG["manifest"]).read_text())
    names = [label for label, _, _ in ordered_routes(target, manifest.get("ordered_profile"))]
    if set(manifest["guests"]) != set(names):
        raise ValueError(CONFIG["bad_inventory"])
    output.mkdir()
    result = {"manifest_sha256": digest(guests / CONFIG["manifest"]), "observations": {}}
    for name in names:
        image = guests / name
        if digest(image) != manifest["guests"][name]["sha256"]:
            raise ValueError(CONFIG["bad_hash"].format(name=name))
        check_macho(image, CONFIG["executable_kind"], target["subtype"])
        observed = subprocess.run([image], capture_output=True, text=True, timeout=CONFIG["process_timeout"])
        (output / (name + CONFIG["model_log_extension"])).write_text(observed.stdout + observed.stderr)
        records = observed.stdout.splitlines()
        if observed.returncode == target["skip_exit"]:
            valid = records == [target["unavailable"]]
        else:
            valid = (observed.returncode == 0 and len(records) == 2 and
                     records[0] in (target["gpr"], target["simd"]) and records[1] in target["rules"])
        if not valid:
            raise RuntimeError(CONFIG["bad_observation"].format(
                label=name, status=observed.returncode, records=records))
        item = {"records": records, "exit_code": observed.returncode}
        if name != names[0] and item != result["observations"][names[0]]:
            raise ValueError(target["inconsistent"])
        result["observations"][name] = item
    first = result["observations"][names[0]]
    result[target["gpr_key"]] = first["exit_code"] == 0
    result[target["simd_key"]] = first["records"][0] == target["simd"]
    write_json(output / CONFIG["report"], result)
    return first["exit_code"]


def run(model, guests, output, cursor=False, ordered=False, *, writeback=False):
    output.mkdir()
    version = subprocess.check_output([model, *CONFIG["version_flags"]], text=True, stderr=subprocess.STDOUT)
    match = re.search(CONFIG["version_pattern"], version)
    if not match or match[1] != CONFIG["model_version"]:
        raise ValueError(CONFIG["bad_version"].format(version=version.strip()))
    manifest = json.loads((guests / CONFIG["manifest"]).read_text())
    policy = CONFIG
    if ordered:
        policy = CONFIG["ordered"]
    elif writeback:
        policy = CONFIG["writeback"]
    passed = policy["passed"]
    records_required = ([policy["control_record"].format(mode) for mode in range(policy["control_count"])]
                        if writeback else policy["control_records"])
    publication = (not (cursor or ordered or writeback) and
                   manifest.get("return_profile") == CONFIG["signing_address"]["profile"])
    if publication:
        names = [CONFIG["signing_address"]["profile"]]
        if manifest.get(CONFIG["signing_address"]["count_property"]) != CONFIG["signing_address"]["bodies"]:
            raise ValueError(CONFIG["bad_inventory"])
    elif ordered:
        profile = manifest.get("ordered_profile", policy["quadword_profile"])
        names = [label for label, _, _ in ordered_routes(policy, profile)]
        if profile == policy["quadword_profile"]:
            passed = policy["legacy_passed"]
        elif profile != policy["profile"]:
            passed = policy["fixed_input_passed"]
    elif cursor or writeback:
        profile = policy if writeback else CONFIG["cursor"]
        names = [profile["name"].format(optimization=opt) for opt in CONFIG["optimizations"]]
    else:
        profile = manifest.get("return_profile", CONFIG["previous_return_profile"])
        names = ["-".join(case) for case in cases(profile)]
    if set(manifest["guests"]) != set(names):
        raise ValueError(CONFIG["bad_inventory"])
    result = {"model_version": CONFIG["model_version"], "model_sha256": digest(model),
              "manifest_sha256": digest(guests / CONFIG["manifest"]), "observations": {}}
    negative_case = names[0] if publication or cursor or ordered or writeback else CONFIG["negative_case"]
    for label in [negative_case + CONFIG["negative_suffix"], *names]:
        name = label.removesuffix(CONFIG["negative_suffix"])
        negative = name != label
        guest = guests / name
        item = manifest["guests"][name]
        if digest(guest) != item["sha256"]:
            raise ValueError(CONFIG["bad_hash"].format(name=name))
        params = {**CONFIG["parameters"], CONFIG["reset_parameter"]: item["entry"]}
        if ordered or writeback:
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
                    else [*records_required, passed])
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
    signing_link = commands.add_parser(options["build_signing_address"])
    ordered_link = commands.add_parser(options["build_ordered"])
    native_link = commands.add_parser(options["build_native_ordered"])
    native_execute = commands.add_parser(options["run_native_ordered"])
    for command in (link, signing_link, ordered_link, native_link):
        command.add_argument(options["tests"], type=Path, required=True)
    cursor_link = commands.add_parser(options["build_cursor"])
    writeback_link = commands.add_parser(options["build_writeback"])
    execute = commands.add_parser(options["run"])
    cursor_execute = commands.add_parser(options["run_cursor"])
    ordered_execute = commands.add_parser(options["run_ordered"])
    writeback_execute = commands.add_parser(options["run_writeback"])
    executions = (execute, cursor_execute, ordered_execute, writeback_execute)
    for command in executions:
        command.add_argument(options["model"], type=Path, required=True)
    for command in (*executions, native_execute):
        command.add_argument(options["guests"], type=Path, required=True)
    for command in (link, signing_link, cursor_link, writeback_link, ordered_link, native_link, *executions, native_execute):
        command.add_argument(options["output"], type=Path, required=True)
    args = parser.parse_args()
    if args.command == options["build"]:
        build(args.tests.resolve(), args.output.resolve())
    elif args.command == options["build_signing_address"]:
        build_signing_address(args.tests.resolve(), args.output.resolve())
    elif args.command in (options["build_ordered"], options["build_native_ordered"]):
        build_ordered(args.tests.resolve(), args.output.resolve(), native=args.command == options["build_native_ordered"])
    elif args.command == options["run_native_ordered"]:
        raise SystemExit(run_native_ordered(args.guests.resolve(), args.output.resolve()))
    elif args.command in (options["build_cursor"], options["build_writeback"]):
        family = "writeback" if args.command == options["build_writeback"] else "cursor"
        build_control_guests(args.output.resolve(), family)
    else:
        run(args.model.resolve(), args.guests.resolve(), args.output.resolve(),
            cursor=args.command == options["run_cursor"], ordered=args.command == options["run_ordered"],
            writeback=args.command == options["run_writeback"])
