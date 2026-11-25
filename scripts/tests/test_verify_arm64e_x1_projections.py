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
            manifest = {**projection.input_identity(),
                        "files": {n.as_posix(): projection.model.digest(root / n) for n in names}}
            path = root / projection.CONFIG["manifest"]
            projection.model.write_json(path, manifest)
            self.assertEqual(projection.checked_inputs(root), manifest)
            for key in ("fixtures", "files", "source_revision"):
                with self.subTest(missing=key):
                    projection.model.write_json(path, {k: v for k, v in manifest.items() if k != key})
                    with self.assertRaises(ValueError):
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
