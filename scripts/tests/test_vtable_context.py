"""Unpruned source declarations, existing ownership and native record kinds."""

import json
from pathlib import Path
import tempfile
import unittest

from scripts.signatures import build_msvc_signatures as builder
from scripts.signatures import supplement_vtable_context as tables
from scripts.signatures import refine_pattern_metadata as metadata


class VTableContextTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.name = "??_7Widget@@6B@"
        self.row = {"schema": 1, "name": self.name, "complete": True, "bytes": 16,
                    "slots": [{"offset": 0, "names": ["destroy", "fallback"], "weak_search": 1},
                              {"offset": 8, "names": ["action"]}]}
        self.source = self.write("source.jsonl", self.row)
        self.existing = self.root / "runtime.npat"
        self.existing.write_text("55" * 16 + " 00 0000 0010 !caller :0000 fallback\n")

    def write(self, name, *rows):
        path = self.root / name
        path.write_text("".join(json.dumps(row) + "\n" for row in rows))
        return path

    def select(self, evidence=None):
        return tables.select([self.source], evidence or [self.source],
                             [self.existing], {self.name})

    def test_complete_table_is_distinct_from_a_function_and_roundtrips(self):
        selected = self.select()
        self.assertEqual(len(selected), 1)
        line = builder.parse_line(selected[0].text)
        self.assertTrue(line.is_vtable)
        self.assertFalse(line.require_caller)
        self.assertFalse(line.require_references)
        self.assertEqual(line.with_refs(line.refs).text, selected[0].text)
        with self.assertRaises(builder.BuildError):
            builder.fold([line.text])
        with self.assertRaises(builder.BuildError):
            line.with_refs(line.refs[:1])

    def test_incomplete_or_conflicting_counterclaim_vetoes_publication(self):
        incomplete = dict(self.row, complete=False, reason="unsupported weak binding")
        rival = json.loads(json.dumps(self.row))
        rival["slots"][1]["names"] = ["foreign"]
        for row in (incomplete, rival):
            with self.subTest(row=row):
                evidence = self.write("evidence.jsonl", self.row, row)
                with self.assertRaises(builder.BuildError):
                    self.select([evidence])

    def test_missing_evidence_and_missing_candidate_are_errors(self):
        empty = self.write("empty.jsonl")
        with self.assertRaises(builder.BuildError):
            self.select([empty])
        with self.assertRaises(builder.BuildError):
            tables.select([empty], [self.source], [self.existing], {self.name})

    def test_search_rules_preserve_alternatives_without_false_cross_version_conflict(self):
        rows = [dict(self.row, slots=[dict(self.row["slots"][0], weak_search=policy),
                                      self.row["slots"][1]]) for policy in (1, 2, 3)]
        self.assertEqual(len(self.select([self.write("versions.jsonl", *rows)])), 1)

    def test_existing_record_is_preserved_and_not_duplicated(self):
        selected = self.select()[0]
        self.existing.write_text(self.existing.read_text() + selected.text + "\n")
        self.assertEqual(self.select(), [])
        self.existing.write_text(self.existing.read_text().replace("^0008 action", "^0008 foreign"))
        with self.assertRaises(builder.BuildError):
            self.select()

    def test_no_pack_is_written_without_a_strong_context_body(self):
        for text in ("55 00 0000 0001 !caller :0000 fallback\n",
                     "55" * 16 + " 00 0000 0010 :0000 fallback\n"):
            self.existing.write_text(text)
            with self.assertRaises(builder.BuildError):
                self.select()

    def test_malformed_source_fields_are_rejected(self):
        mutations = [lambda r: r.update(complete=1),
                     lambda r: r.update(bytes=15),
                     lambda r: r["slots"].pop(),
                     lambda r: r["slots"][0].update(weak_search=4),
                     lambda r: r["slots"][0].update(names=["destroy"]),
                     lambda r: r["slots"][1].update(offset=0),
                     lambda r: r["slots"][1].update(names=["two tokens"])]
        for mutate in mutations:
            row = json.loads(json.dumps(self.row))
            mutate(row)
            with self.assertRaises(builder.BuildError):
                tables.read(self.write("malformed.jsonl", row))

    def test_malformed_native_table_is_rejected(self):
        text = self.select()[0].text
        variants = [text.replace("^0008", "&0008"), text.replace("^0008", "^0001"),
                    text.replace("0010", "0018"), text.replace(":0000", ":0008"),
                    text.replace("00 0000", "00 0001"), "AA" + text[2:],
                    text.replace(" ^0008 action", ""), text.replace("6B@", "6BBase@@@")]
        for variant in variants:
            with self.subTest(variant=variant), self.assertRaises(builder.BuildError):
                builder.parse_line(variant)

    def test_function_metadata_refinement_preserves_table_bytes(self):
        text = self.select()[0].text + "\n"
        self.existing.write_text(text)
        raw = self.root / "functions.pat"
        raw.write_text("55" * 16 + " 00 0000 0010 !evidence :0000 action\n")
        self.assertEqual(metadata.refine([self.existing], [raw]), {})


if __name__ == "__main__":
    unittest.main()
