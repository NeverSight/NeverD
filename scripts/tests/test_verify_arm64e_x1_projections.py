"""Exercise the artifact and process boundaries of native x1 projection replay."""

from contextlib import ExitStack
import json
from pathlib import Path
import re
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
        gate = native.POLICY["x1_qualification"]
        modes = native.POLICY["cursor_probe"]
        cases = list(projection.model.cases(projection.MODEL["return_profile"]))
        # Separate hardware availability, entry, each signing recipe and broken measurements.
        for controls, launch, explicit, prefix, broken_control, broken_image in (
                (True, True, True, True, False, False),
                (False, True, True, True, False, False),
                (False, True, True, False, False, False),
                (False, False, False, False, False, False),
                (False, False, False, False, True, False),
                (True, True, True, True, False, True)):
            with self.subTest(controls=controls, explicit=explicit, prefix=prefix, broken_control=broken_control,
                              broken_image=broken_image), tempfile.TemporaryDirectory() as tmp, ExitStack() as m:
                inputs = Path(tmp)
                output = inputs / config["check"]
                manifest = inputs / config["manifest"]
                manifest.write_text(json.dumps({}))
                sha = projection.model.digest(manifest)
                cursor = {modes["baseline_key"]: launch, modes["explicit_key"]: explicit, modes["pacm_pc_key"]: prefix,
                          "status": native.POLICY["passed" if prefix else "unavailable"],
                          "profiles": {native.POLICY["optimizations"][0]: {"sha256": sha}}}

                def qualify(directory, *, require_complete):
                    self.assertTrue(require_complete)
                    filename = native.POLICY["report_name"]
                    path = directory / projection.X1["mode"] / filename
                    path.parent.mkdir()
                    native.save(path, cursor)
                    status = "failed" if broken_control else "passed" if controls else "unavailable"
                    result = {"status": native.POLICY[status], gate["complete_key"]: controls,
                              "probes": {projection.X1["mode"]: {"report": path.relative_to(directory).as_posix(),
                                                                 "sha256": projection.model.digest(path)}}}
                    native.save(directory / filename, result)
                    return result

                def image(family, route, opt, source, dest, capable, launchable):
                    self.assertEqual(launchable, launch)
                    self.assertEqual(capable, explicit if family == CASES["explicit"] else prefix)
                    if broken_image:
                        raise ValueError(dest)
                    return {"execution": config["executed" if capable else "entry_only" if launch else "not_started"]}

                m.enter_context(mock.patch.object(projection, "checked_inputs", return_value={
                    "source_revision": sha, "exporter_sha256": sha}))
                aggregate = m.enter_context(mock.patch.object(native, "qualify_x1", side_effect=qualify))
                second_cursor = m.enter_context(mock.patch.object(native, "probe_x1_signal_cursor"))
                measured = m.enter_context(mock.patch.object(projection, "measure_image", side_effect=image))
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
                aggregate.assert_called_once()
                second_cursor.assert_not_called()
                receipt = report[config["control_receipt"]]
                self.assertEqual(projection.model.digest(output / receipt["report"]), receipt["sha256"])
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
