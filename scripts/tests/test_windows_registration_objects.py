import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from scripts import check_windows_registration_nested_try as runner
from scripts import replay_windows_registration_nested_try as replay
from scripts import windows_registration_objects as objects
from scripts.tests import test_check_windows_registration_nested_try as fixtures


class ObjectEvidenceTests(unittest.TestCase):
    def capture(self, root):
        with patch.multiple(runner, SOURCE=objects.SOURCE, FORMS=objects.FORMS,
                            CASES=objects.CASES):
            capture = fixtures.NestedTryEvidenceTests().capture(root)
        capture["profile"] = "objects"
        capture["object_proof_sha256"] = runner.file_digest(objects.PROOF)
        capture["object_types_sha256"] = runner.file_digest(objects.TYPES)
        for case in capture["cases"]:
            parent = root / case["case"]
            (parent / "rewrite.xml").write_text(
                '<testsuites tests="2"><testcase/><testcase/></testsuites>')
            source = parent / "decompiled.cpp"
            source.write_text(source.read_text() + "/* ReferenceObject */\n")
            case["decompilation"]["cpp"] = runner.file_digest(source)
        return capture

    def test_object_matrix_binds_profile_types_and_extra_proof(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            self.assertEqual(len(replay.validate_capture(root, capture)), 128)
            for field, value in (("profile", "nested"), ("profile", "unknown"),
                                 ("object_proof_sha256", "stale"),
                                 ("object_types_sha256", "stale")):
                changed = copy.deepcopy(capture)
                changed[field] = value
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    replay.validate_capture(root, changed)
            (root / capture["cases"][0]["case"] / "rewrite.xml").write_text(
                '<testsuites tests="1"><testcase/></testsuites>')
            with self.assertRaises(ValueError):
                replay.validate_capture(root, capture)

    def test_unknown_records_do_not_gain_invented_fields(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            case = capture["cases"][0]
            path = root / case["case"] / "decompiled.cpp"
            text = path.read_text()
            for changed in (text.replace("ReferenceObject", ""), text + "Object.Head;",
                            text + "Object.Tail;", text + "Object.Tag;"):
                path.write_text(changed)
                case["decompilation"]["cpp"] = runner.file_digest(path)
                with self.subTest(source=changed), self.assertRaises(ValueError):
                    replay.validate_capture(root, capture)


if __name__ == "__main__":
    unittest.main()
