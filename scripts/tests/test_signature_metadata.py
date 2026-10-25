"""Immutable metadata refinements preserve bytes and retain counterexamples."""

from pathlib import Path
import tempfile
import unittest

from scripts.signatures import build_msvc_signatures as builder
from scripts.signatures import refine_pattern_metadata as metadata


class MetadataRefinementTests(unittest.TestCase):
    def refine(self, old, sources, names=None):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            original = root / "runtime.pat"
            original.write_text(old)
            evidence = []
            for index, text in enumerate(sources):
                path = root / f"raw{index}.pat"
                path.write_text(text)
                evidence.append(path)
            result = metadata.refine([original], evidence, names)
            self.assertEqual(original.read_text(), old)
            return result.get(original, old)

    def test_same_body_gains_only_missing_immutable_fields(self):
        old = "AABBCCDD....C3 00 0000 0007 :0000 routine ^0003 existing\n"
        raw = old.replace(" :", " !evidence :")
        raw = raw.rstrip() + " ^0004 ??_7Widget@@6B@ ^0005 helper ^0006 __imp_API\n"
        result = builder.parse_line(self.refine(old, [raw]))
        self.assertEqual(result.key, builder.parse_line(old).key)
        self.assertEqual(result.names, builder.parse_line(old).names)
        self.assertEqual(result.refs, ((3, "existing"), (4, "??_7Widget@@6B@"), (6, "__imp_API")))

    def test_missing_field_in_another_version_remains_unknown(self):
        old = "AABBCCDD 00 0000 0004 :0000 routine\n"
        raw = old.replace(" :", " !evidence :")
        typed = raw.rstrip() + " ^0001 ??_7Widget@@6B@\n"
        self.assertEqual(self.refine(old, [typed, raw]), old)
        changed = typed.replace("??_7Widget@@6B@", "??_7Other@@6B@")
        line = builder.parse_line(self.refine(old, [typed, changed]))
        self.assertEqual(line.refs, ((1, "??_7Other@@6B@"), (1, "??_7Widget@@6B@")))

    def test_every_existing_alias_requires_source_ownership(self):
        old = "AABBCCDD 00 0000 0004 :0000 first :0000 second\n"
        raw = "AABBCCDD 00 0000 0004 !evidence :0000 first ^0001 __imp_API\n"
        self.assertEqual(self.refine(old, [raw]), old)
        result = builder.parse_line(self.refine(old, [raw, raw.replace("first", "second")]))
        self.assertEqual(result.refs, ((1, "__imp_API"),))

    def test_partial_or_other_body_evidence_cannot_refine(self):
        old = "AABBCCDD 00 0000 0005 :0000 routine\n"
        raw = old.replace(" :", " !evidence :").rstrip() + " ^0001 __imp_API\n"
        self.assertEqual(self.refine(old, [raw]), old)
        self.assertEqual(self.refine(old.replace("0005", "0004"), [raw]),
                         old.replace("0005", "0004"))

    def test_existing_fields_and_nonprimary_tables_are_preserved(self):
        old = "AABBCCDD 00 0000 0004 :0000 routine ^0001 original\n"
        raw = "AABBCCDD 00 0000 0004 !evidence :0000 routine ^0001 __imp_API ^0002 ??_7Widget@@6BBase@@@\n"
        self.assertEqual(self.refine(old, [raw]), old)
        with self.assertRaisesRegex(builder.BuildError, "raw !evidence"):
            self.refine(old, [raw.replace(" !evidence", "")])

    def test_reviewed_subset_does_not_modify_other_records(self):
        first = "AABBCCDD 00 0000 0004 :0000 first\n"
        second = first.replace("first", "second")
        sources = [(first + second).replace(" :", " !evidence :").replace(
            "first\n", "first ^0001 __imp_API\n").replace(
                "second\n", "second ^0001 __imp_API\n")]
        self.assertEqual(self.refine(first + second, sources, {"first"}),
                         first.rstrip() + " ^0001 __imp_API\n" + second)


if __name__ == "__main__":
    unittest.main()
