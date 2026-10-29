"""Reject truncated, changed or unbound cross-host numerical evidence."""

import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_x87_cross_host as recorder
from scripts.tests.test_check_x87_portable_generated import reset_fixture


class CrossHostEvidenceTests(unittest.TestCase):
    def test_raw_bits_and_every_defined_status_bit_are_compared(self):
        request = bytes(24)
        reference = bytes(12)
        for index in range(10):
            changed = bytearray(reference)
            changed[index] = 1
            self.assertEqual(len(recorder.arithmetic_mismatches(request, changed, reference)), 1)
        for flag in (1, 2, 4, 8, 16, 32, 512):
            changed = bytes(10) + flag.to_bytes(2, "little")
            self.assertEqual(len(recorder.arithmetic_mismatches(request, changed, reference)), 1)
        self.assertEqual(recorder.arithmetic_mismatches(request, bytes(10) + b'\x00\x45', reference), [])

    def test_partial_request_or_response_is_never_passing_coverage(self):
        for requests, actual, original in ((bytes(23), bytes(12), bytes(12)),
                                          (bytes(24), bytes(11), bytes(12)),
                                          (bytes(24), bytes(12), bytes(13))):
            with self.assertRaisesRegex(ValueError, "count"):
                recorder.arithmetic_mismatches(requests, actual, original)

    def test_changed_missing_and_unbound_artifacts_fail(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            artifact = directory / "data.bin"
            artifact.write_bytes(b'original')
            record = {"files": recorder.file_manifest(directory)}
            (directory / "record.json").write_text(json.dumps(record))
            self.assertEqual(recorder.read_verified(directory), record)
            artifact.write_bytes(b'changed!')
            with self.assertRaisesRegex(ValueError, "changed"):
                recorder.read_verified(directory)
            artifact.unlink()
            with self.assertRaisesRegex(ValueError, "missing"):
                recorder.read_verified(directory)
            artifact.write_bytes(b'original')
            (directory / "injected.bin").write_bytes(b'new')
            with self.assertRaisesRegex(ValueError, "unbound"):
                recorder.read_verified(directory)

    def test_seed_protocol_includes_all_precision_rounding_pairs(self):
        for seed in recorder.SEEDS:
            payload = recorder.inputs(seed)
            self.assertEqual(len(payload) // 24, 19296)
            controls = {int.from_bytes(payload[i+2:i+4], 'little')
                        for i in range(0, len(payload), 24)}
            self.assertEqual(controls, {0x7f | (pc << 8) | (rc << 10)
                                       for pc in (0, 2, 3) for rc in range(4)})

    def test_same_commit_cannot_hide_modified_or_missing_source_bindings(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            patch = directory / "source.patch"
            patch.write_bytes(b"")
            record = {"git_head": "1" * 40, "untracked_source_sha256": {}}
            recorder.require_clean_source_record(directory, record)
            patch.write_bytes(b"different implementation at the same HEAD")
            with self.assertRaisesRegex(ValueError, "clean committed"):
                recorder.require_clean_source_record(directory, record)
            patch.write_bytes(b"")
            for untracked in ({"lib/injected.cpp": "digest"}, None):
                record["untracked_source_sha256"] = untracked
                with self.assertRaisesRegex(ValueError, "clean committed"):
                    recorder.require_clean_source_record(directory, record)

    def test_reset_cross_host_reader_requires_every_seeded_observation(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            directory = root / "reset-evidence"
            artifacts = directory / "artifacts" / "x87-resets-test"
            artifacts.mkdir(parents=True)
            commands, rows = reset_fixture()
            (directory / "source.patch").write_bytes(b"")
            (artifacts / "commands.jsonl").write_text("\n".join(map(json.dumps, commands)))
            observations = artifacts / "observations.jsonl"
            observations.write_text("\n".join(map(json.dumps, rows)))
            record = {"passed": True, "stable_inputs": True, "profile": "resets",
                      "environment": {"machine": "aarch64"}, "untracked_source_sha256": {}}
            def write_record():
                record["files"] = {k: v for k, v in recorder.file_manifest(directory).items()
                                   if k != "record.json"}
                (directory / "record.json").write_text(json.dumps(record))
            write_record()
            _, actual = recorder.generated_observations(root, False, "resets")
            self.assertEqual(len(actual), 2700)
            observations.write_text("\n".join(map(json.dumps, rows[:-1])))
            write_record()
            with self.assertRaisesRegex(ValueError, "missing reset"):
                recorder.generated_observations(root, False, "resets")


if __name__ == "__main__":
    unittest.main()
