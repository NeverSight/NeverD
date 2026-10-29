"""Evidence must contain actual native tests, every control and every object."""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_x87_translation as recorder


def fixture():
    rows = []
    for top in range(8):
        for pc in (0, 2, 3):
            for rc in range(4):
                for level in (0, 2):
                    case = f"exact-{top}-{pc}-{rc}"
                    rows.append({"case": case, "level": level, "exit_reason": 1,
                        "guest_instructions": 7, "blocks_translated": 6,
                        "generated_bytes": 1, "before": "00", "after": "01",
                        "session_after": "01", "blocks": [
                            {"object": f"{case}-O{level}-block-{i}.o", "guest_bytes": code,
                             "status": 260 if i == 5 else 256, "audit_stages": 511,
                             "registry": "registry", "artifact_identity": "object",
                             "attempted": i != 5, "completed": i != 5,
                             "portable_status": 1 if i == 5 else 0, "after": "01"}
                            for i, code in enumerate(["48C7C000000000DB2F", "DB2E", "DEC1",
                                                       "DB3A", "DFE0", "C3"])]})
    return rows


def reset_fixture():
    rows = []
    for kind in range(3):
        for case in range(75):
            for level in (0, 2):
                refused = case >= 72
                rows.append({"kind": kind, "case": case, "level": level,
                    "exit_reason": 6 if refused else 1,
                    "guest_instructions": 2 if refused else 3,
                    "blocks_translated": 1 if refused else 2,
                    "generated_bytes": 1, "before": "00", "after": "01",
                    "session_after": "01", "blocks": [
                        {"object": f"reset-{kind}-{case}-O{level}-block-{i}.o",
                         "guest_bytes": "C3" if i else "48C7C022110000" + ("D9D0", "0F77", "0F0E")[kind],
                         "entry_pc": "4009" if i else "4000",
                         "status": 261 if refused else 260 if i else 256,
                         "audit_stages": 511, "registry": "registry", "artifact_identity": "object",
                         "attempted": i == 0, "completed": i == 0 and not refused,
                         "portable_status": 1 if i else (2, 6, 5)[case - 72] if refused else 0,
                         "after": "01"} for i in range(1 if refused else 2)]})
    return rows


class X87TranslationEvidenceTests(unittest.TestCase):
    def test_complete_control_and_object_counts(self):
        coverage, objects = recorder.audit_observations(fixture())
        self.assertEqual(coverage["cases"], 96)
        self.assertEqual(coverage["observations"], 192)
        self.assertEqual(len(objects), 1152)

    def test_missing_duplicate_and_changed_identity_fail(self):
        rows = fixture()
        for bad in (rows[:-1], rows + rows[:1]):
            with self.assertRaises(ValueError):
                recorder.audit_observations(bad)
        rows[0]["blocks"][0]["object"] = "../other.o"
        with self.assertRaisesRegex(ValueError, "object"):
            recorder.audit_observations(rows)

    def test_terminal_protocol_and_audit_cannot_be_omitted(self):
        for field, value in (("status", 0), ("audit_stages", 255), ("completed", False),
                             ("portable_status", 13), ("guest_bytes", "90")):
            rows = fixture()
            rows[0]["blocks"][0][field] = value
            with self.assertRaisesRegex(ValueError, "protocol"):
                recorder.audit_observations(rows)

    def test_state_and_optimization_disagreement_fail(self):
        rows = fixture()
        rows[0]["session_after"] = "02"
        with self.assertRaisesRegex(ValueError, "disagree"):
            recorder.audit_observations(rows)
        rows = fixture()
        rows[1]["blocks"][0]["after"] = "02"
        with self.assertRaisesRegex(ValueError, "disagree"):
            recorder.audit_observations(rows)

    def test_skipped_missing_failed_and_duplicate_native_tests_fail(self):
        cases = ['<testcase classname="' + name.rsplit('.', 1)[0] + '" name="' +
                 name.rsplit('.', 1)[1] + '" status="run" result="completed"/>'
                 for name in sorted(recorder.EXPECTED_TESTS)]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "results.xml"
            path.write_text('<testsuites>' + ''.join(cases) + '</testsuites>')
            self.assertEqual(recorder.audit_tests(path), 17)
            for bad in (cases[:-1], cases + cases[:1],
                        [cases[0].replace('/>', '><skipped/></testcase>')] + cases[1:],
                        [cases[0].replace('/>', '><failure/></testcase>')] + cases[1:]):
                path.write_text('<testsuites>' + ''.join(bad) + '</testsuites>')
                with self.assertRaises(ValueError):
                    recorder.audit_tests(path)


class X87ResetTranslationEvidenceTests(unittest.TestCase):
    def test_all_state_and_refusal_cases_are_required(self):
        rows = reset_fixture()
        coverage, objects = recorder.audit_reset_observations(rows)
        self.assertEqual(coverage["cases"], 225)
        self.assertEqual(coverage["observations"], 450)
        self.assertEqual(coverage["refusals"], 18)
        self.assertEqual(len(objects), 882)
        for bad in (rows[:-1], rows + rows[:1]):
            with self.assertRaises(ValueError):
                recorder.audit_reset_observations(bad)

    def test_wrong_reset_bytes_or_refusal_protocol_fail(self):
        for case, field, value in ((0, "guest_bytes", "90"), (0, "entry_pc", "4007"),
                                  (72, "status", 256), (73, "portable_status", 0),
                                  (74, "completed", True), (0, "audit_stages", 255)):
            rows = reset_fixture()
            row = next(row for row in rows if row["case"] == case)
            row["blocks"][0][field] = value
            with self.assertRaisesRegex(ValueError, "protocol"):
                recorder.audit_reset_observations(rows)

    def test_objects_and_sessions_cannot_disagree(self):
        for field in ("session_after", "after"):
            rows = reset_fixture()
            rows[0][field] = "02"
            with self.assertRaisesRegex(ValueError, "disagree"):
                recorder.audit_reset_observations(rows)
        rows = reset_fixture()
        rows[1]["blocks"][0]["after"] = "02"
        with self.assertRaisesRegex(ValueError, "disagree"):
            recorder.audit_reset_observations(rows)
        rows = reset_fixture()
        rows[0]["blocks"][0]["object"] = "../unbound.o"
        with self.assertRaisesRegex(ValueError, "object"):
            recorder.audit_reset_observations(rows)


if __name__ == "__main__":
    unittest.main()
