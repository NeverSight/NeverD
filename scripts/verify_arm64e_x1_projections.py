#!/usr/bin/env python3
"""Export NeverD signing projections and qualify their signed arm64e.x1 images."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

import verify_arm64e_native as native
import verify_arm64e_return_model as model

CONFIG = json.loads(Path(__file__).with_suffix(".def").read_text())
MODEL = model.CONFIG
FIXTURES = native.ROOT / MODEL["fixture_directory"]
X1 = native.POLICY["x1_cursor_probe"]
SIGN = native.POLICY["cpta_probe"]
SUBTYPE = SIGN["profiles"][SIGN["x1"]]


def inventory():
    sources = [Path(route) / CONFIG["source_name"] for route in (*MODEL["routes"], MODEL["caller_directory"])]
    return [Path(family) / path for family in MODEL["cases"]
            for path in (*sources, *MODEL["backend_objects"].values())]


def input_identity():
    return {"source_revision": native.provenance()["source_revision"],
            "oracle_pairs_sha256": model.digest(native.ROOT / CONFIG["pairs"]),
            "fixtures": {name: model.digest(FIXTURES / name) for name in MODEL["fixture_files"]}}


def export(tests, output):
    if output.exists():
        raise FileExistsError(output)
    with tempfile.TemporaryDirectory(dir=output.parent) as temporary:
        scratch = Path(temporary)
        staged = scratch / MODEL["staging_directory"]
        staged.mkdir()
        tests_by_family = {family: row["test"] for family, row in MODEL["cases"].items()}
        generated = model.export_cases(tests, scratch, tests_by_family.values(), MODEL["artifact_environment"],
                                       MODEL["artifact_property"], {MODEL["fixture_environment"]: str(FIXTURES)})
        files = {}
        for path in inventory():
            family = path.parts[0]
            origin = generated[tests_by_family[family]].joinpath(*path.parts[1:])
            if path.name in MODEL["backend_objects"].values():
                model.check_macho(origin, MODEL["object_kind"])
            dest = staged / path
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(origin, dest)
            files[path.as_posix()] = model.digest(dest)
        model.write_json(staged / CONFIG["manifest"], {**input_identity(), "exporter_sha256": model.digest(tests),
                                                       "files": files})
        staged.rename(output)


def checked_inputs(directory):
    manifest = json.loads((directory / CONFIG["manifest"]).read_text())
    if (not isinstance(manifest, dict) or not isinstance(manifest.get("exporter_sha256"), str) or
            not re.fullmatch(CONFIG["digest_pattern"], manifest["exporter_sha256"]) or
            any(manifest.get(k) != v for k, v in input_identity().items()) or
            manifest.get("files") != {p.as_posix(): model.digest(directory / p) for p in inventory()}):
        raise ValueError(CONFIG["errors"]["inputs"])
    return manifest


def build_r6(output):
    if output.exists():
        raise FileExistsError(output)
    recipe = CONFIG["r6"]
    with tempfile.TemporaryDirectory(dir=output.parent) as temporary:
        build = Path(temporary)
        configure = [arg.format(build=build, **recipe) for arg in recipe["configure"]]
        compile_args = [arg.format(build=build, **recipe) for arg in recipe["compile"]]
        subprocess.run(recipe["dependencies"], cwd=native.ROOT, check=True)
        subprocess.run(configure, cwd=native.ROOT, check=True)
        subprocess.run([sys.executable, *compile_args], cwd=native.ROOT, check=True)
        export(build / recipe["bin_dir"] / recipe["target"], output)


def checked_command(products, directory, label, args):
    result = native.command(products, directory, label, args, native.POLICY["tool_timeout"])
    if result.returncode:
        raise RuntimeError(CONFIG["errors"]["stage"].format(label=label, directory=directory))
    return result


def linked_symbols(link_map, family, owner):
    pairs = (native.ROOT / CONFIG["pairs"]).read_text()
    expected = {CONFIG["symbol_prefix"] + m[family] for m in re.finditer(CONFIG["pair_pattern"], pairs, re.MULTILINE)}
    inputs, _, live = link_map.partition(CONFIG["map_live"])
    live = live.partition(CONFIG["map_dead"])[0]
    indices = {i for i, path in re.findall(CONFIG["map_input"], inputs, re.MULTILINE) if Path(path) == owner}
    names = {symbol for i, symbol in re.findall(CONFIG["map_symbol"], live, re.MULTILINE) if i in indices}
    if not expected or not expected.issubset(names):
        raise ValueError(CONFIG["errors"]["symbols"].format(owner=owner))
    return sorted(expected)


def compare_process(products, directory, image, capable, launchable):
    args = [image] if capable else [image, CONFIG["entry_mode"]]
    result = native.execute_apple_slice(products, directory, CONFIG["execute"], args)
    if not launchable and (result is None or native.killed_before_entry(result)):
        return CONFIG["not_started"]
    records = [CONFIG["entered"], CONFIG["compared"]] if capable else [CONFIG["entered"]]
    if not launchable or result is None or result.returncode or result.stdout.splitlines() != records:
        raise ValueError(CONFIG["errors"]["execution"].format(directory=directory))
    return CONFIG["executed"] if capable else CONFIG["entry_only"]


def measure_image(family, route, opt, inputs, directory, capable, launchable):
    directory.mkdir(parents=True)
    backend = MODEL["backend_objects"].get(route)
    source = inputs / family / (MODEL["caller_directory"] if backend else route) / CONFIG["source_name"]
    cc = [CONFIG["compiler"], *X1["target"], MODEL["optimization_option"].format(optimization=opt)]
    with native.OwnedProducts(directory, False) as products:
        obj, entry, image = (directory / CONFIG[key] for key in ("projected_object", "entry_object", "image"))
        for path in (obj, entry, image):
            products.append(path)
        checked_command(products, directory, CONFIG["compile"],
                        [*cc, *CONFIG["c_flags"], FIXTURES, MODEL["compile_only"], source, MODEL["output_option"], obj])
        model.check_macho(obj, MODEL["object_kind"], SUBTYPE)
        checked_command(products, directory, CONFIG["compile_entry"],
                        [*cc, *CONFIG["entry_flags"], MODEL["compile_only"], model.ROOT / CONFIG["driver"],
                         MODEL["output_option"], entry])
        model.check_macho(entry, MODEL["object_kind"], SUBTYPE)
        link_map = directory / CONFIG["map_name"]
        link = [*cc, *CONFIG["map_args"], link_map, entry, obj]
        owner = obj
        receipt = {"source_sha256": model.digest(source), "object_sha256": model.digest(obj)}
        if backend:
            raw = inputs / family / backend
            model.check_macho(raw, MODEL["object_kind"])
            link.append(raw)
            owner = raw
            receipt["backend_sha256"] = model.digest(raw)
        checked_command(products, directory, CONFIG["link"], [*link, MODEL["output_option"], image])
        model.check_macho(image, MODEL["executable_kind"], SUBTYPE)
        receipt["linked_symbols"] = linked_symbols(link_map.read_text(), family, owner)
        receipt["link_map_sha256"] = model.digest(link_map)
        entitlements = native.sign_x1_slice(products, directory, image, SIGN)
        if entitlements is None:
            raise ValueError(CONFIG["errors"]["stage"].format(label=SIGN["sign_label"], directory=directory))
        receipt.update(native.check_binary(image, SUBTYPE))
        receipt[SIGN["entitlements_key"]] = entitlements
        receipt["execution"] = compare_process(products, directory, image, capable, launchable)
        return receipt


def projection_cursor(output, report, require_native):
    filename = native.POLICY["report_name"]
    if not require_native:
        directory = output / X1["mode"]
        directory.mkdir()
        cursor = native.probe_x1_signal_cursor(directory)
        report["cursor_sha256"] = model.digest(directory / filename)
        return cursor

    gate = native.POLICY["x1_qualification"]
    directory = output / gate["complete_mode"]
    directory.mkdir()
    path = directory / filename
    try:
        controls = native.qualify_x1(directory, require_complete=True)
    finally:
        if path.is_file():
            report[CONFIG["control_receipt"]] = {"report": path.relative_to(output).as_posix(),
                                                "sha256": model.digest(path)}
    if controls["source_revision"] != report["source_revision"]:
        raise ValueError(CONFIG["errors"]["control_identity"])
    report[gate["complete_key"]] = controls[gate["complete_key"]]
    if controls["status"] == native.POLICY["failed"]:
        return None
    receipt = controls["probes"][X1["mode"]]
    raw = (directory / receipt["report"]).read_bytes()
    if hashlib.sha256(raw).hexdigest() != receipt["sha256"]:
        raise ValueError(CONFIG["errors"]["control_identity"])
    cursor = json.loads(raw)
    if cursor["source_revision"] != report["source_revision"]:
        raise ValueError(CONFIG["errors"]["control_identity"])
    report["cursor_sha256"] = receipt["sha256"]
    return cursor


def check(inputs, output, require_execution=False, *, require_native=False):
    manifest = checked_inputs(inputs)
    output.mkdir()
    gate = native.POLICY["x1_qualification"]
    report = {"source_revision": manifest["source_revision"], "exporter_sha256": manifest["exporter_sha256"],
              "input_manifest_sha256": model.digest(inputs / CONFIG["manifest"]),
              "status": native.POLICY["failed"], "all_linked": False, "all_executed": False, "images": {}}
    if require_native:
        report[CONFIG["qualified_key"]] = False
    try:
        cursor = projection_cursor(output, report, require_native)
        if cursor is None:
            return native.POLICY["exit_codes"][report["status"]]
        if cursor["status"] == native.POLICY["failed"]:
            raise RuntimeError(CONFIG["errors"]["capability"])
        rows = cursor["profiles"].values()
        if not cursor["profiles"] or any("sha256" not in row for row in rows):
            report["status"] = native.POLICY["unavailable"]
            return native.POLICY["exit_codes"][report["status"]]
        launchable = cursor[native.POLICY["cursor_probe"]["baseline_key"]]
        for family, route, opt in model.cases(MODEL["return_profile"]):
            label = "-".join((family, route, opt))
            capable = cursor[X1[CONFIG["capabilities"][family]]]
            report["images"][label] = measure_image(family, route, opt, inputs, output / label, capable, launchable)
        report["all_linked"] = True
        report["all_executed"] = all(row["execution"] == CONFIG["executed"] for row in report["images"].values())
        if require_native:
            passed = report[gate["complete_key"]] and report["all_executed"]
            report[CONFIG["qualified_key"]] = passed
        else:
            passed = not require_execution or report["all_executed"]
        report["status"] = native.POLICY["passed" if passed else "unavailable"]
        return native.POLICY["exit_codes"][report["status"]]
    finally:
        model.write_json(output / MODEL["report"], report)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    builder = modes.add_parser(CONFIG["build"])
    producer = modes.add_parser(CONFIG["export"])
    consumer = modes.add_parser(CONFIG["check"])
    producer.add_argument(CONFIG["tests"], type=Path, required=True)
    consumer.add_argument(CONFIG["inputs"], type=Path, required=True)
    consumer.add_argument(CONFIG["require_execution"], action="store_true")
    consumer.add_argument(CONFIG["require_native"], action="store_true")
    for mode in (builder, producer, consumer):
        mode.add_argument(CONFIG["output"], type=Path, required=True)
    args = parser.parse_args()
    if args.mode == CONFIG["build"]:
        build_r6(args.output.resolve())
        return 0
    if args.mode == CONFIG["export"]:
        export(args.tests.resolve(), args.output.resolve())
        return 0
    return check(args.inputs.resolve(), args.output.resolve(), args.require_execution, require_native=args.require_native)


if __name__ == "__main__":
    raise SystemExit(main())
