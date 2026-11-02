"""Source coordinate preservation, separate counterclaims and native admission."""

import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from scripts.signatures import build_msvc_signatures as builder
from scripts.signatures import supplement_code_layouts as layouts


BODY = ("488B05........C34883EC28FF15........4885C0488905........"
        "0F95C04883C428C3488B05........C3 00 0000 002C !layout "
        ":0000 left :0008 root :0024 right &0003 data "
        "^000E __imp_Allocate &0018 data &0027 data")


def record(pattern=BODY):
    line = builder.parse_line(pattern)
    members = {}
    for name in line.names:
        offset, text = name.split()
        members.setdefault(int(offset[1:], 16), []).append(text)
    return {"schema": 1, "member": "source.obj", "section": 1,
            "complete": True, "pattern": pattern,
            "members": [{"offset": offset, "names": names} for offset, names in members.items()]}


class CodeLayoutSelection(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.source = self.directory / "evidence.jsonl"

    def write(self, records):
        self.source.write_text("".join(json.dumps(row) + "\n" for row in records))

    def test_distinct_source_claims_are_retained_without_alias_folding(self):
        other = BODY.replace(":0000 left", ":0000 rival")
        self.write([record(), record(other), record()])
        result = layouts.select([self.source], [])
        self.assertEqual(len(result.lines), 2)
        self.assertEqual(len(result.witnesses), 3)
        for text in result.lines:
            parsed = builder.parse_line(text)
            self.assertTrue(parsed.is_code_layout)
            with self.assertRaises(builder.BuildError):
                parsed.with_refs(())
        with self.assertRaises(builder.BuildError):
            builder.fold(result.lines)

    def test_existing_complete_record_is_not_duplicated(self):
        self.write([record()])
        previous = self.directory / "library.npat"
        previous.write_text(BODY + "\n")
        result = layouts.select([self.source], [self.directory])
        self.assertEqual(result.lines, [])
        self.assertEqual(result.existing[0]["sha256"], layouts.sha(previous))

    def test_incomplete_declarations_are_reported_without_candidates(self):
        bad = record()
        bad["complete"] = False
        bad["reason"] = "split source contribution"
        del bad["pattern"]
        self.write([bad, record()])
        result = layouts.select([self.source], [])
        self.assertEqual(result.incomplete, 1)
        self.assertEqual(result.lines, [BODY])
        bad["pattern"] = BODY
        self.write([bad])
        with self.assertRaises(builder.BuildError):
            layouts.select([self.source], [])

    def test_source_coordinates_cannot_disagree_with_native_record(self):
        bad = record()
        bad["members"][1]["offset"] += 1
        self.write([bad])
        with self.assertRaises(builder.BuildError):
            layouts.select([self.source], [])

    def test_native_failure_preserves_previous_artifact(self):
        self.write([record()])
        selection = layouts.select([self.source], [])
        output, producer = self.directory / "library.npat", self.directory / "producer"
        output.write_text("previous artifact\n")
        producer.write_text("native producer fixture")
        with patch.object(layouts.subprocess, "run", side_effect=subprocess.CalledProcessError(1, ["verify"])):
            with self.assertRaises(subprocess.CalledProcessError):
                layouts.publish(selection, output, producer)
        self.assertEqual(output.read_text(), "previous artifact\n")
        with patch.object(layouts.subprocess, "run") as verify:
            report = layouts.publish(selection, output, producer)
        self.assertEqual(verify.call_args.args[0][1], "--verify")
        self.assertEqual(report["records"], 1)
        self.assertEqual(report["global_byte_records"], 0)
        self.assertEqual(report["output"]["sha256"], layouts.sha(output))


if __name__ == "__main__":
    unittest.main()
