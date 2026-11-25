"""Exercise the artifact and process boundaries of native x1 projection replay."""

from contextlib import ExitStack
import json
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import verify_arm64e_x1_projections as projection

CASES = json.loads(Path(__file__).with_suffix(".def").read_text())


class X1ProjectionTests(unittest.TestCase):
    def test_the_link_map_must_keep_every_function_from_the_measured_object(self):
        config = projection.CONFIG
        family = CASES["explicit"]
        owner = Path(CASES["object"])
        pairs = (projection.native.ROOT / config["pairs"]).read_text()
        names = [config["symbol_prefix"] + row[family] for row in re.finditer(config["pair_pattern"], pairs, re.M)]
        header = CASES["inputs"].format(owner=owner, foreign=CASES["foreign"])
        live = [CASES["symbol"].format(index=CASES["live"], symbol=name) for name in names]
        self.assertEqual(projection.linked_symbols(header + "".join(live), family, owner), sorted(names))
        partial = header + "".join(live[:-1])
        wrong = CASES["symbol"].format(index=CASES["wrong"], symbol=names[-1])
        for suffix in ("", wrong, CASES["dead"] + live[-1]):
            with self.subTest(suffix=suffix), self.assertRaises(ValueError):
                projection.linked_symbols(partial + suffix, family, owner)

    def test_export_checks_every_family_route_and_fixture(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            names = projection.inventory()
            for name in names:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(name.as_posix())
            manifest = {**projection.input_identity(), "exporter_sha256": projection.model.digest(root / names[0]),
                        "files": {n.as_posix(): projection.model.digest(root / n) for n in names}}
            path = root / projection.CONFIG["manifest"]
            projection.model.write_json(path, manifest)
            self.assertEqual(projection.checked_inputs(root), manifest)
            with self.assertRaisesRegex(ValueError, projection.CONFIG["errors"]["ordered_inputs"]):
                projection.checked_inputs(root, require_ordered=True)
            ordered_dir = root / projection.ORDERED["directory"]
            ordered_dir.mkdir()
            for name in projection.model.ordered_input_names(projection.DARWIN_ORDERED):
                (ordered_dir / name).write_text(name)
            identity = projection.ordered_identity(ordered_dir)
            ordered_key = projection.ORDERED["inputs"]
            complete = {**manifest, ordered_key: identity}
            projection.model.write_json(path, complete)
            self.assertEqual(projection.checked_inputs(root, require_ordered=True), complete)
            for key in identity:
                with self.subTest(missing_ordered=key), self.assertRaisesRegex(
                        ValueError, projection.CONFIG["errors"]["ordered_inputs"]):
                    projection.model.write_json(path, {**complete, ordered_key: {**identity, key: {}}})
                    projection.checked_inputs(root, require_ordered=True)
            projection.model.write_json(path, complete)
            (ordered_dir / projection.DARWIN_ORDERED["raw_object_file"]).write_bytes(b"")
            with self.assertRaisesRegex(ValueError, projection.CONFIG["errors"]["ordered_inputs"]):
                projection.checked_inputs(root)
            for key in ("fixtures", "files", "source_revision", "exporter_sha256"):
                with self.subTest(missing=key):
                    projection.model.write_json(path, {k: v for k, v in manifest.items() if k != key})
                    with self.assertRaises(ValueError):
                        projection.checked_inputs(root)
            for invalid in (None, [], {**manifest, "exporter_sha256": None},
                            {**manifest, "exporter_sha256": manifest["exporter_sha256"][1:]}):
                with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                    projection.model.write_json(path, invalid)
                    projection.checked_inputs(root)
            projection.model.write_json(path, manifest)
            (root / names[-1]).write_text(names[0].as_posix())
            with self.assertRaises(ValueError):
                projection.checked_inputs(root)

    def test_entry_and_success_must_agree_with_independent_capability(self):
        config = projection.CONFIG
        directory = Path()
        entered = [config["entered"]]
        compared = [*entered, config["compared"]]
        for capable, launchable, event, expected in (
                (False, False, None, config["not_started"]), (True, True, compared, config["executed"]),
                (False, True, entered, config["entry_only"]), (True, True, None, None),
                (True, True, entered, None), (False, True, compared, None), (False, False, entered, None)):
            with self.subTest(capable=capable, launchable=launchable, event=event):
                result = None if event is None else subprocess.CompletedProcess([], 0, "\n".join(event), "")
                with mock.patch.object(projection.native, "execute_apple_slice", return_value=result):
                    if expected is None:
                        with self.assertRaises(ValueError):
                            projection.compare_process([], directory, directory, capable, launchable)
                    else:
                        self.assertEqual(projection.compare_process([], directory, directory, capable, launchable),
                                         expected)

    def test_signal_after_entry_and_nonzero_oracle_results_are_failures(self):
        config = projection.CONFIG
        entered = config["entered"]
        compared = "\n".join((entered, config["compared"]))
        for launchable, code, records, expected in (
                (False, -signal.SIGKILL, "", config["not_started"]),
                (True, -signal.SIGKILL, "", None),
                (False, -signal.SIGKILL, entered, None),
                (True, -signal.SIGILL, entered, None),
                (True, 1, compared, None)):
            with self.subTest(launchable=launchable, code=code, records=records):
                process = subprocess.CompletedProcess([], code, records, "")
                with mock.patch.object(projection.native, "execute_apple_slice", return_value=process):
                    if expected is None:
                        with self.assertRaises(ValueError):
                            projection.compare_process([], Path(), Path(), launchable, launchable)
                    else:
                        self.assertEqual(projection.compare_process([], Path(), Path(), False, launchable), expected)

    def test_all_twenty_images_are_required_for_link_and_execution_claims(self):
        native, config = projection.native, projection.CONFIG
        modes = native.POLICY["cursor_probe"]
        cases = list(projection.model.cases(projection.MODEL["return_profile"]))
        for require, executes in ((False, False), (True, False), (True, True)):
            with self.subTest(require=require, executes=executes), tempfile.TemporaryDirectory() as tmp, ExitStack() as m:
                inputs = Path(tmp)
                (inputs / config["manifest"]).write_text(json.dumps({}))
                output = inputs / config["check"]
                cursor = {"status": native.POLICY["passed"], modes["baseline_key"]: True,
                          **dict.fromkeys((modes["explicit_key"], modes["pacm_pc_key"]), executes),
                          "profiles": {"O0": {"sha256": projection.model.digest(inputs / config["manifest"])}}}

                def observe(directory):
                    native.save(directory / native.POLICY["report_name"], cursor)
                    return cursor

                def image(family, route, opt, directory, out, capable, launchable):
                    self.assertTrue(launchable)
                    self.assertEqual(capable, executes)
                    return {"execution": config["executed"] if capable else config["entry_only"]}

                m.enter_context(mock.patch.object(projection, "checked_inputs", return_value={
                    "source_revision": "", "exporter_sha256": ""}))
                m.enter_context(mock.patch.object(native, "probe_x1_signal_cursor", side_effect=observe))
                measured = m.enter_context(mock.patch.object(projection, "measure_image", side_effect=image))
                code = projection.check(inputs, output, require)
                report = json.loads((output / projection.MODEL["report"]).read_text())
                self.assertEqual(measured.call_count, len(cases))
                self.assertEqual(set(report["images"]), {"-".join(case) for case in cases})
                self.assertTrue(report["all_linked"])
                self.assertEqual(report["all_executed"], executes)
                self.assertEqual(code, native.POLICY["exit_codes"][
                    native.POLICY["unavailable" if require and not executes else "passed"]])

    def test_r6_build_hands_the_pinned_exporter_to_export_and_removes_the_build(self):
        recipe = projection.CONFIG["r6"]
        for fail in (False, True):
            with self.subTest(fail=fail), tempfile.TemporaryDirectory() as tmp, ExitStack() as m:
                output = Path(tmp) / projection.CONFIG["export"]
                runs = m.enter_context(mock.patch.object(projection.subprocess, "run"))
                exporter = m.enter_context(mock.patch.object(projection, "export"))

                def finish(tests, destination):
                    self.assertTrue(tests.parents[1].is_dir())
                    self.assertEqual(tests.relative_to(tests.parents[1]), Path(recipe["bin_dir"]) / recipe["target"])
                    if fail:
                        raise ValueError(tests)
                    destination.mkdir()

                exporter.side_effect = finish
                if fail:
                    with self.assertRaises(ValueError):
                        projection.build_r6(output)
                else:
                    projection.build_r6(output)
                    with self.assertRaises(FileExistsError):
                        projection.build_r6(output)
                exporter.assert_called_once()
                build = exporter.call_args.args[0].parents[1]
                self.assertFalse(build.exists())
                self.assertEqual(output.exists(), not fail)
                dependencies, configure, compile_command = [call.args[0] for call in runs.call_args_list]
                self.assertEqual(dependencies, recipe["dependencies"])
                for value in (recipe["tag"], recipe["sha256"]):
                    self.assertTrue(any(arg.endswith(value) for arg in configure))
                self.assertEqual(compile_command, [sys.executable, *[arg.format(build=build, **recipe)
                                                                     for arg in recipe["compile"]]])

    def test_native_acceptance_joins_five_controls_and_all_generated_processes(self):
        native, config = projection.native, projection.CONFIG
        revision = native.provenance()["source_revision"]
        gate = native.POLICY["x1_qualification"]
        modes = native.POLICY["cursor_probe"]
        cases = list(projection.model.cases(projection.MODEL["return_profile"]))
        # Separate hardware availability, entry, each signing recipe and broken measurements.
        for controls, launch, explicit, prefix, broken_control, broken_image in (
                (True, True, True, True, False, False),
                (False, True, True, True, False, False),
                (False, True, True, False, False, False),
                (False, True, False, True, False, False),
                (False, False, False, False, False, False),
                (False, False, False, False, True, False),
                (True, True, True, True, False, True)):
            with self.subTest(controls=controls, explicit=explicit, prefix=prefix, broken_control=broken_control,
                              broken_image=broken_image), tempfile.TemporaryDirectory() as tmp, ExitStack() as m:
                inputs = Path(tmp).resolve()
                output = inputs / config["check"]
                manifest = inputs / config["manifest"]
                manifest.write_text(json.dumps({}))
                sha = projection.model.digest(manifest)
                cursor = {modes["baseline_key"]: launch, modes["explicit_key"]: explicit, modes["pacm_pc_key"]: prefix,
                          "status": native.POLICY["passed" if launch else "unavailable"],
                          "profiles": {native.POLICY["optimizations"][0]: {"sha256": sha}}}

                ordered = native.POLICY["ordered_probe"]

                def probe(directory):
                    mode = directory.name
                    if mode == projection.X1["mode"]:
                        result = cursor
                    elif mode == ordered["mode"]:
                        result = {ordered["qualified_key"]: controls, "profiles": dict.fromkeys(native.POLICY["optimizations"]),
                                  "status": native.POLICY["passed" if controls else "unavailable"]}
                    else:
                        failed = broken_control and mode == native.POLICY["probe_mode"]
                        result = {**dict.fromkeys(gate["requirements"][mode], True),
                                  "status": native.POLICY["failed" if failed else "passed"]}
                    result["source_revision"] = revision
                    native.save(directory / native.POLICY["report_name"], result)
                    return result

                def image(family, route, opt, source, dest, capable, launchable):
                    self.assertEqual(launchable, launch)
                    self.assertEqual(capable, explicit if family == CASES["explicit"] else prefix)
                    if broken_image:
                        raise ValueError(dest)
                    return {"execution": config["executed" if capable else "entry_only" if launch else "not_started"]}

                m.enter_context(mock.patch.object(projection, "checked_inputs", return_value={
                    "source_revision": revision, "exporter_sha256": sha}))
                aggregate = m.enter_context(mock.patch.object(native, "qualify_x1", wraps=native.qualify_x1))
                probes = [m.enter_context(mock.patch.object(native, fn.__name__, side_effect=probe))
                          for fn in (native.probe, native.probe_signal_cursor, native.probe_x1_signal_cursor,
                                     native.probe_cpta, native.probe_ordered_q)]
                measured = m.enter_context(mock.patch.object(projection, "measure_image", side_effect=image))
                memory = m.enter_context(mock.patch.object(projection, "measure_ordered", return_value={
                    projection.DARWIN_ORDERED["gpr_key"]: True, projection.DARWIN_ORDERED["simd_key"]: controls}))
                m.enter_context(mock.patch.object(sys, "argv", [__file__, config["check"], config["require_native"],
                                                               config["inputs"], str(inputs), config["output"], str(output)]))
                if broken_image:
                    with self.assertRaises(ValueError):
                        projection.main()
                else:
                    code = projection.main()
                report = json.loads((output / projection.MODEL["report"]).read_text())
                passed = controls and explicit and prefix and not broken_image
                status = "failed" if broken_control or broken_image else "passed" if passed else "unavailable"
                self.assertEqual(report["status"], native.POLICY[status])
                if not broken_image:
                    self.assertEqual(code, native.POLICY["exit_codes"][report["status"]])
                self.assertEqual(report[config["qualified_key"]], passed)
                self.assertEqual(report[gate["complete_key"]], controls)
                self.assertEqual(memory.call_count, int(not broken_control))
                self.assertEqual(report.get(projection.ORDERED["receipt"]), None if broken_control else memory.return_value)
                directory = output / gate["complete_mode"]
                aggregate.assert_called_once_with(directory, require_complete=True)
                for observed in probes:
                    observed.assert_called_once()
                receipt = report[config["control_receipt"]]
                control_report = output / receipt["report"]
                self.assertEqual(projection.model.digest(control_report), receipt["sha256"])
                children = json.loads(control_report.read_text())["probes"]
                self.assertEqual(set(children), {*gate["requirements"], ordered["mode"]})
                for child in children.values():
                    self.assertEqual(projection.model.digest(directory / child["report"]), child["sha256"])
                if not broken_control:
                    child = output / gate["complete_mode"] / projection.X1["mode"] / native.POLICY["report_name"]
                    self.assertEqual(report["cursor_sha256"], projection.model.digest(child))
                if not broken_control and not broken_image:
                    self.assertEqual(measured.call_count, len(cases))
                    executed = sum(item["execution"] == config["executed"] for item in report["images"].values())
                    self.assertEqual(executed, sum(explicit if family == CASES["explicit"] else prefix
                                                   for family, _, _ in cases))
                    self.assertTrue(report["all_linked"])
                else:
                    self.assertFalse(report["all_linked"])
                    self.assertEqual(measured.call_count, int(broken_image))

    def test_generated_q_controls_qualification_independently_of_x1_image_entry(self):
        native, config = projection.native, projection.CONFIG
        gate = native.POLICY["x1_qualification"]
        original_q = native.POLICY["ordered_probe"]["qualified_key"]
        gpr_key, simd_key = (projection.DARWIN_ORDERED[key] for key in ("gpr_key", "simd_key"))
        for has_slice, original, gpr, simd in ((False, False, True, False), (False, True, True, True),
                                               (True, True, False, False), (True, False, True, True),
                                               (True, True, True, False)):
            with self.subTest(slice=has_slice, original=original, gpr=gpr, simd=simd), \
                    tempfile.TemporaryDirectory() as tmp, ExitStack() as m:
                inputs = Path(tmp)
                (inputs / config["manifest"]).write_text(json.dumps({}))
                output = inputs / config["check"]
                digest = projection.model.digest(inputs / config["manifest"])
                profile = native.POLICY["optimizations"][0]
                cursor = {"status": native.POLICY["passed"], "profiles": {profile: {"sha256": digest} if has_slice else {}},
                          **dict.fromkeys((projection.X1[key] for key in ("explicit_key", "pacm_pc_key")), True),
                          native.POLICY["cursor_probe"]["baseline_key"]: True}

                def controls(_, report, require_native):
                    self.assertTrue(require_native)
                    report[gate["complete_key"]] = original
                    return cursor, {"profiles": {profile: {}}, original_q: original}

                m.enter_context(mock.patch.object(projection, "checked_inputs", return_value={
                    "source_revision": native.provenance()["source_revision"], "exporter_sha256": digest}))
                m.enter_context(mock.patch.object(projection, "projection_controls", side_effect=controls))
                receipt = {gpr_key: gpr, simd_key: simd}
                memory = m.enter_context(mock.patch.object(projection, "measure_ordered", return_value=receipt))
                images = m.enter_context(mock.patch.object(projection, "measure_image", return_value={"execution": config["executed"]}))
                inconsistent = gpr and simd != original
                if inconsistent:
                    with self.assertRaisesRegex(ValueError, config["errors"]["ordered_control"]):
                        projection.check(inputs, output, require_native=True)
                else:
                    self.assertEqual(projection.check(inputs, output, require_native=True),
                                     native.POLICY["exit_codes"][native.POLICY["unavailable"]])
                report = json.loads((output / projection.MODEL["report"]).read_text())
                self.assertEqual(report["status"], native.POLICY["failed" if inconsistent else "unavailable"])
                self.assertFalse(report[config["qualified_key"]])
                self.assertEqual(report[projection.ORDERED["receipt"]], receipt)
                self.assertEqual(images.called, has_slice and not inconsistent)
                memory.assert_called_once()

    def test_stale_controls_or_a_changed_probe_cannot_authorize_a_generated_process(self):
        native, config = projection.native, projection.CONFIG
        gate = native.POLICY["x1_qualification"]
        ordered = native.POLICY["ordered_probe"]
        modes = (projection.X1["mode"], ordered["mode"])
        cases = [(gate["complete_mode"], False), *((mode, corrupt) for mode in modes for corrupt in (False, True))]
        for damaged, corrupt in cases:
            with self.subTest(damaged=damaged, corrupt=corrupt), \
                    tempfile.TemporaryDirectory() as tmp, ExitStack() as m:
                inputs = Path(tmp)
                manifest = inputs / config["manifest"]
                manifest.write_text(json.dumps({}))
                revision = native.provenance()["source_revision"]
                different = revision[::-1]
                output = inputs / config["check"]

                def qualify(directory, *, require_complete):
                    filename = native.POLICY["report_name"]
                    probes = {}
                    for mode in modes:
                        path = directory / mode / filename
                        path.parent.mkdir()
                        native.save(path, {"source_revision": different if mode == damaged and not corrupt else revision,
                                           ordered["qualified_key"]: True})
                        probes[mode] = {"report": path.relative_to(directory).as_posix(), "sha256": projection.model.digest(path)}
                        if mode == damaged and corrupt:
                            path.write_bytes(path.read_bytes() + b"\n")
                    report = {"source_revision": different if damaged == gate["complete_mode"] else revision,
                              "status": native.POLICY["passed"], gate["complete_key"]: True, ordered["qualified_key"]: True,
                              "probes": probes}
                    native.save(directory / filename, report)
                    return report

                m.enter_context(mock.patch.object(projection, "checked_inputs", return_value={
                    "source_revision": revision, "exporter_sha256": projection.model.digest(manifest)}))
                m.enter_context(mock.patch.object(native, "qualify_x1", side_effect=qualify))
                measure = m.enter_context(mock.patch.object(projection, "measure_image"))
                with self.assertRaisesRegex(ValueError, config["errors"]["control_identity"]):
                    projection.check(inputs, output, require_native=True)
                report = json.loads((output / projection.MODEL["report"]).read_text())
                self.assertFalse(report[config["qualified_key"]])
                self.assertEqual(report["status"], native.POLICY["failed"])
                measure.assert_not_called()
