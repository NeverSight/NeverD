"""Strictness tests for generated x87 evidence, independent of running fixtures."""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_x87_portable_generated as recorder


def fixture():
    commands, observations = [], []
    for name, shape in recorder.expected_artifacts().items():
        commands.append({"artifact": name})
        for case in sorted(recorder.expected_cases(shape)):
            status = ((2, 6, 5, 9, 10, 12, 13)[int(case[8:])]
                      if case.startswith("refusal-") else
                      1 if case.startswith("null-") else 0)
            observations.append({"artifact": name, "case": case, "status": status,
                                 "before": "00" * 528, "after": "00" * 528,
                                 "native_reference": None, "return": "0"})
    return commands, observations


def reset_fixture(native=False):
    commands, rows = [], []
    for name, kind in recorder.expected_artifacts().items():
        commands.append({"artifact": name,
            "source_bytes": "488D04374801D0" + ("D9D0", "0F77", "0F0E")[kind] + "C3",
            "target": ("x86_64" if native else "aarch64") + "-unknown-linux-gnu"})
        for case in sorted(recorder.reset_cases()):
            before = recorder.reset_input(case)
            after = bytearray(before)
            refused = case.startswith("reset-refusal-")
            status = (2, 6, 5)[int(case[-1])] if refused else 0
            if not refused:
                if kind == 0:
                    after[24:32] = (0x1007).to_bytes(8, "little")
                else:
                    top = int(case.split("-")[1])
                    for logical in range(8):
                        physical = (logical + top) % 8
                        after[48 + physical * 16:64 + physical * 16] = before[48 + logical * 16:64 + logical * 16]
                    after[19], after[20] = 0x47, 0
                if kind == 2:
                    after[9] = 0
            reference = None
            if native and not refused and kind != 2 and case.endswith("-255"):
                reference = {"control": int.from_bytes(after[16:18], "little"),
                    "status_defined": int.from_bytes(after[18:20], "little") & (0xb8ff if kind == 0 else 0xffff),
                    "tags": after[20], "retained_result": after[160:170].hex(),
                    "retained_second": after[144:154].hex()}
            rows.append({"artifact": name, "case": case, "before": before.hex(), "after": after.hex(),
                "status": status, "return": "ffffffffffffffff" if refused else "4002",
                "instruction_completed": not refused, "memory": [], "memory_access": False,
                "has_scalar_result": False, "instruction_address": "1007", "data_address": "0",
                "native_reference": reference})
    return commands, rows


class PortableGeneratedEvidenceTests(unittest.TestCase):
    def test_complete_protocol_has_fixed_coverage(self):
        commands, observations = fixture()
        result = recorder.audit_observations(commands, observations, False)
        self.assertEqual(result["artifacts"], 36)
        self.assertEqual(result["observations"], 6096)
        self.assertEqual(result["distinct_cases"], 508)
        self.assertEqual(result["statuses"]["0"], 5772)
        self.assertEqual(result["statuses"]["1"], 72)
        self.assertEqual(result["native_x64_original_comparisons"], 0)

    def test_missing_case_or_duplicate_cannot_count_as_coverage(self):
        commands, observations = fixture()
        with self.assertRaisesRegex(ValueError, "missing"):
            recorder.audit_observations(commands, observations[:-1])
        with self.assertRaisesRegex(ValueError, "duplicate"):
            recorder.audit_observations(commands, observations + observations[:1])
        with self.assertRaisesRegex(ValueError, "artifact"):
            recorder.audit_observations(commands + commands[:1], observations)

    def test_a_refusal_cannot_become_silent_success(self):
        commands, observations = fixture()
        row = next(item for item in observations if item["case"] == "refusal-0")
        row["status"] = 0
        with self.assertRaisesRegex(ValueError, "status"):
            recorder.audit_observations(commands, observations)

    def test_cross_route_disagreement_and_partial_state_fail(self):
        commands, observations = fixture()
        observations[-1]["return"] = "1"
        with self.assertRaisesRegex(ValueError, "disagree"):
            recorder.audit_observations(commands, observations)
        observations[-1]["return"] = "0"
        observations[0]["after"] = "00" * 527
        with self.assertRaisesRegex(ValueError, "complete"):
            recorder.audit_observations(commands, observations)

    def test_native_reference_is_host_specific_and_required_on_x64(self):
        commands, observations = fixture()
        with self.assertRaisesRegex(ValueError, "missing native"):
            recorder.audit_observations(commands, observations, True)
        for row in observations:
            if row["case"].startswith("exact-"):
                row["native_reference"] = {"defined": "fixture"}
        result = recorder.audit_observations(commands, observations, True)
        self.assertEqual(result["native_x64_original_comparisons"], 5760)
        with self.assertRaisesRegex(ValueError, "non-x64"):
            recorder.audit_observations(commands, observations, False)

    def test_skip_or_missing_named_test_cannot_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "results.xml"
            path.write_text('<testsuites><testsuite><testcase classname="X87PortableGenerated" '
                            'name="AllRoutesExecuteExactStateAndRefusalBoundaries" '
                            'status="run" result="completed"/></testsuite></testsuites>')
            recorder.audit_tests(path)
            path.write_text(path.read_text().replace('/>', '><skipped message="unavailable"/></testcase>'))
            with self.assertRaisesRegex(ValueError, "without skips"):
                recorder.audit_tests(path)
            path.write_text('<testsuites/>')
            with self.assertRaisesRegex(ValueError, "exactly"):
                recorder.audit_tests(path)


class PortableResetEvidenceTests(unittest.TestCase):
    def test_complete_reset_state_inventory_and_native_reference_counts(self):
        for native in (False, True):
            commands, rows = reset_fixture(native)
            result = recorder.audit_observations(commands, rows, native, "resets")
            self.assertEqual(result["artifacts"], 36)
            self.assertEqual(result["observations"], 2700)
            self.assertEqual(result["distinct_cases"], 225)
            self.assertEqual(result["statuses"], {"0": 2592, "2": 36, "6": 36, "5": 36})
            self.assertEqual(result["native_x64_original_comparisons"], 576 if native else 0)

    def test_missing_and_duplicate_inputs_or_results_fail(self):
        commands, rows = reset_fixture()
        for bad in (rows[:-1], rows + rows[:1]):
            with self.assertRaises(ValueError):
                recorder.audit_observations(commands, bad, False, "resets")
        commands[0]["source_bytes"] = "90C3"
        with self.assertRaisesRegex(ValueError, "instruction"):
            recorder.audit_observations(commands, rows, False, "resets")

    def test_unanimous_wrong_top_or_known_mask_is_not_agreement(self):
        for offset in (19, 9):
            commands, rows = reset_fixture()
            for row in rows:
                if row["case"] == "reset-5-85-150" and "-2-" in row["artifact"]:
                    after = bytearray.fromhex(row["after"])
                    after[offset] = bytes.fromhex(row["before"])[offset]
                    row["after"] = after.hex()
            with self.assertRaisesRegex(ValueError, "state"):
                recorder.audit_observations(commands, rows, False, "resets")

    def test_wrong_seed_or_refusal_publication_fails(self):
        commands, rows = reset_fixture()
        rows[0]["before"] = "00" * 528
        with self.assertRaisesRegex(ValueError, "seeded"):
            recorder.audit_observations(commands, rows, False, "resets")
        commands, rows = reset_fixture()
        row = next(row for row in rows if row["case"] == "reset-refusal-1")
        row["return"] = "4002"
        with self.assertRaisesRegex(ValueError, "publication"):
            recorder.audit_observations(commands, rows, False, "resets")

    def test_native_reference_cannot_be_missing_or_claimed_for_femms(self):
        commands, rows = reset_fixture(True)
        row = next(row for row in rows if row["native_reference"] is not None)
        row["native_reference"] = None
        with self.assertRaisesRegex(ValueError, "native"):
            recorder.audit_observations(commands, rows, True, "resets")
        commands, rows = reset_fixture(True)
        row = next(row for row in rows if row["artifact"] == "default-2-0-O0")
        row["native_reference"] = {}
        with self.assertRaisesRegex(ValueError, "native"):
            recorder.audit_observations(commands, rows, True, "resets")


if __name__ == "__main__":
    unittest.main()
