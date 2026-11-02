"""Source defaults, unpruned counterclaims and in-place body refinement."""

import copy
import json
from pathlib import Path
import tempfile
import unittest

from scripts.signatures import build_msvc_signatures as builder
from scripts.signatures import supplement_runtime_pointers as pointers
from scripts.signatures import refine_pattern_metadata as metadata


class RuntimePointerContextTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.rows = [
            {"schema": 1, "kind": "load_config", "name": "_load_config_used", "complete": True,
             "bytes": 0x140, "fields": [{"offset": 0x78, "name": "cf"}, {"offset": 0x120, "name": "xfg"}]},
            {"schema": 1, "kind": "pointer", "name": "cf", "complete": True, "target": "dispatch"},
            {"schema": 1, "kind": "pointer", "name": "xfg", "complete": True, "target": "extended"},
            {"schema": 1, "kind": "function", "name": "dispatch", "complete": True,
             "pattern": "FFE0 00 0000 0002 !caller :0000 dispatch"},
            {"schema": 1, "kind": "function", "name": "extended", "complete": True,
             "pattern": "FF25........ 00 0000 0006 !caller :0000 extended &0002 cf"}]
        self.source = self.write("source.jsonl", self.rows)
        self.existing = self.root / "runtime.npat"
        self.original = (self.rows[3]["pattern"] + "\n" +
                         self.rows[4]["pattern"].replace(" &0002 cf", "") + "\n---\n")
        self.existing.write_text(self.original)

    def write(self, name, rows):
        path = self.root / name
        path.write_text("".join(json.dumps(r) + "\n" for r in rows))
        return path

    def select(self, evidence=None):
        return pointers.select([self.source], evidence or [self.source], [self.existing], {"cf", "xfg"})

    def test_reuses_and_refines_existing_bodies_without_duplicates(self):
        result = self.select()
        self.assertEqual(len(result.updates), 1)
        self.assertEqual(self.existing.read_text(), self.original)
        self.assertEqual(result.updates[self.existing].count(":0000 extended"), 1)
        self.assertIn("&0002 cf", result.updates[self.existing])
        line = builder.parse_line(result.metadata)
        self.assertTrue(line.is_load_config)
        self.assertFalse(line.require_caller)
        self.assertEqual(line.load_config_fields, result.fields)
        self.assertEqual(line.with_refs(line.refs).text, line.text)
        with self.assertRaises(builder.BuildError):
            builder.fold([line.text])
        with self.assertRaises(builder.BuildError):
            line.with_refs(line.refs[:1])

    def test_repeated_selection_preserves_exact_existing_metadata(self):
        result = self.select()
        self.existing.write_text(result.updates[self.existing] + result.metadata + "\n")
        again = self.select()
        self.assertIsNone(again.metadata)
        self.assertEqual(again.updates, {})

    def test_incomplete_definitions_and_configuration_veto(self):
        for i in range(len(self.rows)):
            row = dict(self.rows[i], complete=False, reason="unsupported binding")
            with self.subTest(i=i), self.assertRaises(builder.BuildError):
                self.select([self.write("counterclaim.jsonl", self.rows + [row])])

    def test_conflicting_fields_defaults_or_reference_constraints_veto(self):
        variants = []
        rows = copy.deepcopy(self.rows)
        rows[0]["fields"][0]["name"] = "different"
        variants.append(rows)
        rows = copy.deepcopy(self.rows)
        rows[1]["target"] = "foreign"
        variants.append(rows)
        rows = copy.deepcopy(self.rows)
        rows[4]["pattern"] = rows[4]["pattern"].replace("&0002 cf", "&0002 xfg")
        variants.append(rows)
        for rows in variants:
            with self.assertRaises(builder.BuildError):
                self.select([self.write("counterclaim.jsonl", rows)])

    def test_no_function_typing_is_inferred_from_pointer_target_name(self):
        for omitted in (3, 4):
            rows = self.rows[:omitted] + self.rows[omitted + 1:]
            missing = self.write("missing.jsonl", rows)
            with self.assertRaises(builder.BuildError):
                pointers.select([missing], [missing], [self.existing], {"cf", "xfg"})

    def test_unexplained_source_wildcards_are_not_complete_evidence(self):
        for suffix in ("", " ^0002 cf", " &0002 not_bound"):
            rows = copy.deepcopy(self.rows)
            rows[4]["pattern"] = rows[4]["pattern"].split(" &")[0] + suffix
            with self.assertRaises(builder.BuildError):
                self.select([self.write("weak.jsonl", rows)])

    def test_missing_existing_body_does_not_publish_a_copy(self):
        self.existing.write_text(self.rows[3]["pattern"] + "\n")
        with self.assertRaises(builder.BuildError):
            self.select()

    def test_existing_constraints_and_metadata_cannot_be_replaced(self):
        self.existing.write_text(self.original.replace("extended\n", "extended &0002 xfg\n"))
        with self.assertRaises(builder.BuildError):
            self.select()
        self.existing.write_text(self.original)
        record = self.select().metadata.replace("^0078 dispatch", "^0078 foreign")
        self.existing.write_text(self.original + record + "\n")
        with self.assertRaises(builder.BuildError):
            self.select()

    def test_malformed_source_records_are_rejected(self):
        mutations = [lambda r: r[0].update(complete=1),
                     lambda r: r[0]["fields"].append(r[0]["fields"][0]),
                     lambda r: r[0].update(bytes=16),
                     lambda r: r[1].update(target="two words"),
                     lambda r: r[4].update(name="wrong_function")]
        for mutation in mutations:
            rows = copy.deepcopy(self.rows)
            mutation(rows)
            with self.assertRaises(builder.BuildError):
                pointers.read([self.write("malformed.jsonl", rows)])

    def test_native_metadata_is_not_an_ordinary_function_refinement(self):
        result = self.select()
        self.existing.write_text(result.metadata + "\n")
        raw = self.root / "functions.pat"
        raw.write_text("55" * 16 + " 00 0000 0010 !evidence :0000 dispatch\n")
        self.assertEqual(metadata.refine([self.existing], [raw]), {})
        for before, after in (("&0078 cf", ""), ("^0078 dispatch", "^0078 dispatch ^0078 other"),
                              (":0000", ":0008"), ("0140 !loadcfg", "0120 !loadcfg")):
            with self.assertRaises(builder.BuildError):
                builder.parse_line(result.metadata.replace(before, after))


if __name__ == "__main__":
    unittest.main()
