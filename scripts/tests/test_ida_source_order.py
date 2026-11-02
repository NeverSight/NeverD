"""The source-order audit must not accept incomplete or mismatched evidence."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = (Path(__file__).resolve().parents[2] / "tools" / "neverd-bench"
          / "audit_ida_source_order.py")
SPEC = importlib.util.spec_from_file_location("ida_source_order", SCRIPT)
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)


def observation(text=b"int target() { return 7; }\n"):
    native = {
        "chunks": [["0x1000", "0x1006"]],
        "instructions": [
            {"address": "0x1000", "bytes": "b807000000"},
            {"address": "0x1005", "bytes": "c3"},
        ],
        "blocks": [{"start": "0x1000", "end": "0x1006", "successors": []}],
    }
    return {
        "status": "ok", "input_sha256": "input-hash", "target": "0x1000",
        "engine": {"kernel": "9.5", "library_sha256": "engine-hash"},
        "preceding": [],
        "source": {"entry": "0x1000", "file": "target.c", "bytes": len(text),
                   "sha256": hashlib.sha256(text).hexdigest()},
        "native_before": native, "native_after": copy.deepcopy(native),
        "ctree_addresses": ["0x1000", "0x1005"],
    }


class SourceOrderTests(unittest.TestCase):
    def test_entry_order_is_preserved_and_invalid_plans_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "entries"
            path.write_text("0x3000\n0x2000\n0x1000\n0x4000\n")
            self.assertEqual(audit.preceding_entries(path, 0x1000),
                             [0x3000, 0x2000])
            for invalid in ("0x1000 0x1000", "0x2000", "-1 0x1000",
                            "0x10000000000000000 0x1000"):
                path.write_text(invalid)
                with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                    audit.preceding_entries(path, 0x1000)

    def test_matching_observations_are_stable_without_equivalence_claim(self):
        first = observation()
        result = audit.compare(first, copy.deepcopy(first))
        self.assertEqual(result["status"], "stable")
        self.assertEqual(result["ctree_addresses_removed"], [])
        self.assertIn("do not prove semantic equivalence", result["interpretation"])

    def test_same_native_body_does_not_hide_lost_pseudocode_provenance(self):
        direct = observation()
        prefix = observation(b"void target() { stop(); }\n")
        prefix["ctree_addresses"] = ["0x1000"]
        result = audit.compare(direct, prefix)
        self.assertEqual(result["status"], "changed")
        self.assertFalse(result["native_inventory_changed_between_orders"])
        self.assertEqual(result["ctree_addresses_removed"], ["0x1005"])
        self.assertTrue(result["source_text_changed"])

    def test_text_alone_or_provenance_alone_requires_review(self):
        direct = observation()
        renamed = observation(b"int renamed() { return 7; }\n")
        result = audit.compare(direct, renamed)
        self.assertEqual(result["status"], "changed")
        self.assertEqual(result["ctree_addresses_removed"], [])
        self.assertEqual(result["ctree_addresses_added"], [])
        mapped = copy.deepcopy(direct)
        mapped["ctree_addresses"] = ["0x1000", "0x1004"]
        result = audit.compare(direct, mapped)
        self.assertEqual(result["status"], "changed")
        self.assertFalse(result["source_text_changed"])
        self.assertEqual(result["ctree_addresses_added"], ["0x1004"])

    def test_changed_native_inventory_requires_review_even_with_identical_text(self):
        direct = observation()
        changed = copy.deepcopy(direct)
        changed["native_after"]["instructions"][0]["bytes"] = "b808000000"
        result = audit.compare(direct, changed)
        self.assertEqual(result["status"], "changed")
        self.assertTrue(result["native_inventory_changed_between_orders"])
        self.assertTrue(result["native_inventory_changed_within_observation"]["prefix"])

    def test_different_inputs_targets_or_engines_are_not_comparable(self):
        direct = observation()
        for key, value in (("input_sha256", "other"), ("target", "0x2000"),
                           ("engine", {"kernel": "9.4"})):
            changed = copy.deepcopy(direct)
            changed[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                audit.compare(direct, changed)

    def test_missing_truncated_and_tampered_source_cannot_pass(self):
        text = b"int target() { return 7; }\n"
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            data = observation(text)
            (directory / "observation.json").write_text(json.dumps(data))
            source = directory / "target.c"
            with self.assertRaises(ValueError):
                audit.checked_observation(directory, "input-hash", 0x1000, [])
            for invalid in (text[:-1], text.replace(b"7", b"8")):
                source.write_bytes(invalid)
                with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                    audit.checked_observation(directory, "input-hash", 0x1000, [])
            source.write_bytes(text)
            self.assertEqual(audit.checked_observation(
                directory, "input-hash", 0x1000, []), data)
            for expected in (("other", 0x1000, []),
                             ("input-hash", 0x2000, []),
                             ("input-hash", 0x1000, [0x800])):
                with self.subTest(expected=expected), self.assertRaises(ValueError):
                    audit.checked_observation(directory, *expected)

    def test_existing_evidence_is_preserved_and_not_reused(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            binary = directory / "binary"
            binary.write_bytes(b"fixture")
            entries = directory / "entries"
            entries.write_text("0x1000\n")
            output = directory / "evidence"
            output.mkdir()
            old = output / "assessment.json"
            old.write_text('{"status": "stable"}\n')
            result = subprocess.run(
                [sys.executable, str(SCRIPT), str(binary),
                 "--target", "0x1000", "--entries-file", str(entries),
                 "--output", str(output),
                 "--ida-python", str(directory / "unavailable-python")],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertEqual(old.read_text(), '{"status": "stable"}\n')
            self.assertFalse((output / "direct").exists())

    def test_failed_child_cannot_publish_a_stable_assessment(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            binary = directory / "binary"
            binary.write_bytes(b"fixture")
            entries = directory / "entries"
            entries.write_text("0x1000\n")
            output = directory / "evidence"
            result = subprocess.run(
                [sys.executable, str(SCRIPT), str(binary),
                 "--target", "0x1000", "--entries-file", str(entries),
                 "--output", str(output),
                 "--ida-python", str(directory / "unavailable-python")],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertTrue((output / "input.json").exists())
            self.assertFalse((output / "assessment.json").exists())

    def test_cli_errors_are_not_reported_as_changed_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            binary = directory / "binary"
            binary.write_bytes(b"fixture")
            entries = directory / "entries"
            entries.write_text("0x1000\n")
            output = directory / "evidence"
            common = [str(binary), "--target", "0x1000", "--entries-file",
                      str(entries), "--output", str(output)]
            for arguments in ([], [*common, "--unknown"],
                              [*common, "--target", "-1"],
                              [*common, "--timeout", "0"],
                              [*common, "--timeout", "nan"],
                              [*common, "--timeout", "inf"]):
                with self.subTest(arguments=arguments):
                    result = subprocess.run(
                        [sys.executable, str(SCRIPT), *arguments],
                        capture_output=True, text=True)
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertIn("Source-order audit failed:", result.stderr)
                    self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
