"""Supplement selection keeps raw counterexamples and existing ownership."""

from pathlib import Path
import tempfile
import unittest

from scripts.signatures import build_msvc_signatures as builder
from scripts.signatures import supplement_patterns as supplement


class SupplementTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.existing = self.root / "existing"
        self.existing.mkdir()

    def select(self, candidates, evidence, existing="", **options):
        (self.existing / "runtime.pat").write_text(existing)
        paths = []
        for index, (name, text) in enumerate(evidence):
            path = self.root / f"raw{index}.pat"
            path.write_text(text)
            paths.append((name, path))
        return supplement.select({"runtime": builder.fold(candidates.splitlines())},
                                 paths, self.existing, **options)["runtime"]

    def test_caller_records_keep_distinct_names_without_byte_uniqueness(self):
        a = "33C0C3 00 0000 0003 !caller :0000 a\n"
        b = a.replace(":0000 a", ":0000 b")
        raw = (a + b).replace("!caller", "!evidence")
        result = self.select(a + b, [("runtime", raw)])
        self.assertEqual({line.text for line in result}, {a.strip(), b.strip()})
        self.assertTrue(all(line.require_caller for line in result))
        self.assertEqual([line.text for line in self.select(a + b, [("runtime", raw)], a)],
                         [b.strip()])
        with self.assertRaisesRegex(builder.BuildError, "lacks raw source"):
            self.select(a, [("runtime", b.replace("!caller", "!evidence"))])

    def test_caller_records_never_remove_ordinary_byte_claims(self):
        a = "33C0C3 00 0000 0003 !caller :0000 a\n"
        b = "33C0C3 00 0000 0003 :0000 b\n"
        results = {Path("caller.npat"): builder.fold([a]), Path("ordinary.pat"): builder.fold([b])}
        builder.settle_directory(results)
        self.assertEqual([line.text for line in results[Path("caller.npat")].lines], [a.strip()])
        self.assertEqual([line.text for line in results[Path("ordinary.pat")].lines], [b.strip()])

    def test_caller_deduplication_respects_independently_selected_libraries(self):
        line = "33C0C3 00 0000 0003 !caller :0000 routine\n"
        (self.existing / "older.part2.npat").write_text(line)
        self.assertEqual([item.text for item in self.select(line, [("runtime", line)])],
                         [line.strip()])
        (self.existing / "runtime.part2.npat").write_text(line)
        self.assertEqual(self.select(line, [("runtime", line)]), [])

    def test_caller_metadata_keeps_cross_version_reference_alternatives(self):
        a = "E8........C3 00 0000 0006 !caller :0000 parent ^0001 narrow\n"
        b = a.replace("narrow", "wide").replace("!caller", "!evidence")
        result = self.select(a, [("runtime", a), ("older", b)])
        self.assertEqual(result[0].refs, ((1, "narrow"), (1, "wide")))
        unknown = "E8........C3 00 0000 0006 !evidence :0000 parent\n"
        result = self.select(a, [("runtime", a), ("older", unknown)])
        self.assertEqual(result[0].refs, ())

    def test_ambiguous_ordinary_body_does_not_cover_an_incoming_query(self):
        line = "33C0C3 00 0000 0003 !caller :0000 routine\n"
        old = line.replace(" !caller", "")
        rival = old.replace("routine", "other")
        self.assertEqual(self.select(line, [("runtime", line)], old), [])
        result = self.select(line, [("runtime", line + rival)], old)
        self.assertEqual([item.text for item in result], [line.strip()])
        self.assertEqual(self.select(line, [("runtime", line + rival)], line), [])

    def test_owned_caller_metadata_only_adds_missing_unanimous_fields(self):
        a = "E8........C3 00 0000 0006 !caller :0000 parent ^0001 callee\n"
        old = "E8........C3 00 0000 0006 :0000 parent\n"
        self.assertEqual([line.text for line in self.select(a, [("runtime", a)], old)],
                         [a.strip()])
        self.assertEqual(self.select(a, [("runtime", a)], a), [])
        rival = a.replace("callee", "other").replace("!caller", "!evidence")
        self.assertEqual(self.select(a, [("runtime", a), ("older", rival)], old), [])

    def test_caller_records_reject_incomplete_bodies_and_nonzero_names(self):
        for text in ["33C0C3 00 0000 0004 !caller :0000 a",
                     "33C0C3 00 0000 0003 !caller :0001 a"]:
            with self.assertRaisesRegex(builder.BuildError, "complete body"):
                builder.parse_line(text)

    def test_discarded_source_collision_is_still_evidence(self):
        claim = "AABBCCDD 00 0000 0004 :0000 wanted\n"
        conflict = "AABBCCDD 00 0000 0004 :0000 other\n"
        self.assertEqual(self.select(claim, [("runtime", claim + conflict)]), [])

    def test_covering_source_routine_cannot_be_reintroduced(self):
        claim = "AABBCCDD.... 00 0000 0006 :0000 wanted\n"
        covering = "AABBCCDDEEFF11 00 0000 0007 :0000 other\n"
        self.assertEqual(self.select(claim, [("runtime", claim), ("another", covering)]), [])

    def test_references_can_distinguish_same_library_claims(self):
        a = "AABBCCDD.... 00 0000 0006 :0000 a ^0004 __imp_a\n"
        b = "AABBCCDD.... 00 0000 0006 :0000 b ^0004 __imp_b\n"
        lines = self.select(a + b, [("runtime", a), ("runtime", b)])
        self.assertEqual({line.names for line in lines}, {(':0000 a',), (':0000 b',)})
        self.assertEqual(self.select(a, [("runtime", a), ("another", b)]), [])

    def test_required_context_can_distinguish_independently_selected_packs(self):
        a = "AABBCCDD.... 00 0000 0006 !refs :0000 a ^0004 __imp_a\n"
        b = "AABBCCDD.... 00 0000 0006 :0000 b ^0004 __imp_b\n"
        result = self.select(a, [("runtime", a), ("another", b)])
        self.assertEqual([line.text for line in result], [a.strip()])
        self.assertTrue(result[0].require_references)
        unknown = "AABBCCDD.... 00 0000 0006 :0000 unknown\n"
        self.assertEqual(self.select(a, [("runtime", a), ("another", b + unknown)]), [])

    def test_required_context_checks_every_covering_counterclaim(self):
        a = "AABBCCDD.... 00 0000 0006 !refs :0000 a ^0004 __imp_a\n"
        b = "AABBCCDDEEFF00 00 0000 0007 :0000 b ^0004 __imp_b\n"
        self.assertEqual(len(self.select(a, [("runtime", a), ("another", b)])), 1)
        same = b.replace("__imp_b", "__imp_a")
        self.assertEqual(self.select(a, [("runtime", a), ("another", same)]), [])

    def test_unrelated_ambiguous_siblings_do_not_discard_distinct_required_context(self):
        a = "AABBCCDD.... 00 0000 0006 !refs :0000 a ^0004 __imp_a\n"
        b = "AABBCCDD.... 00 0000 0006 !refs :0000 b ^0004 __imp_shared\n"
        c = b.replace(":0000 b", ":0000 c")
        for candidates in [a + b + c, c + b + a]:
            result = self.select(candidates, [("runtime", a + b + c)])
            self.assertEqual([line.text for line in result], [a.strip()])
        unknown = "AABBCCDD.... 00 0000 0006 :0000 unknown\n"
        self.assertEqual(self.select(a + b + c, [("runtime", a + b + c + unknown)]), [])

    def test_partial_recovery_does_not_promote_optional_or_missing_context(self):
        a = "AABBCCDD.... 00 0000 0006 :0000 a ^0004 __imp_a\n"
        b = "AABBCCDD.... 00 0000 0006 :0000 b ^0004 __imp_shared\n"
        c = b.replace(":0000 b", ":0000 c")
        self.assertEqual(self.select(a + b + c, [("runtime", a + b + c)]), [])
        required = a.replace(" :", " !refs :")
        missing = "AABBCCDD.... 00 0000 0006 :0000 a\n"
        self.assertEqual(self.select(required + b + c + missing,
                                     [("runtime", required + b + c + missing)]), [])

    def test_partial_recovery_preserves_overlapping_alias_uncertainty(self):
        a = "AABBCCDD.... 00 0000 0006 !refs :0000 a :0000 shared ^0004 __imp_a\n"
        alias = a.replace(":0000 a :0000 shared", ":0000 other :0000 shared")
        b = "AABBCCDD.... 00 0000 0006 !refs :0000 b ^0004 __imp_shared\n"
        c = b.replace(":0000 b", ":0000 c")
        source = a + alias + b + c
        self.assertEqual(self.select(source, [("runtime", source)]), [])

    def test_context_requirement_survives_ordinary_raw_evidence(self):
        a = "E9........ 00 0000 0005 !refs :0000 a ^0001 target\n"
        raw = a.replace(" !refs", "")
        result = self.select(a, [("runtime", raw)])
        self.assertEqual([line.text for line in result], [a.strip()])
        covering = "E9........C3 00 0000 0006 :0000 other ^0001 target\n"
        self.assertEqual(self.select(a, [("runtime", raw), ("another", covering)]), [])

    def test_nonmatchable_collision_evidence_remains_a_counterclaim(self):
        a = "AABBCCDD 00 0000 0004 :0000 a\n"
        raw = a.replace(" :", " !evidence :")
        self.assertEqual([line.text for line in self.select(a, [("runtime", raw)])], [a.strip()])
        conflict = raw.replace(":0000 a", ":0000 other")
        self.assertEqual(self.select(a, [("runtime", raw + conflict)]), [])
        with self.assertRaisesRegex(builder.BuildError, "not a candidate"):
            self.select(raw, [("runtime", raw)])

    def test_missing_common_context_is_not_downgraded_to_byte_only(self):
        a = "AABBCCDD.... 00 0000 0006 !refs :0000 a ^0004 callee\n"
        b = "AABBCCDD.... 00 0000 0006 :0000 a\n"
        self.assertEqual(self.select(a + b, [("runtime", a + b)]), [])

    def test_aliases_do_not_duplicate_an_existing_claim(self):
        aliases = "AABBCCDD 00 0000 0004 :0000 copy :0000 move\n"
        old = "AABBCCDD 00 0000 0004 :0000 copy\n"
        self.assertEqual(self.select(aliases, [("runtime", aliases)], old), [])
        self.assertEqual((self.existing / "runtime.pat").read_text(), old)

    def test_relocated_leading_bytes_still_check_exact_collisions(self):
        claim = "....AABB 00 0000 0004 :0000 wanted\n"
        conflict = "....AABB 00 0000 0004 :0000 other\n"
        self.assertEqual(self.select(claim, [("runtime", claim + conflict)]), [])

    def test_candidate_needs_its_own_raw_source(self):
        claim = "AABBCCDD 00 0000 0004 :0000 wanted\n"
        with self.assertRaisesRegex(builder.BuildError, "lacks raw source evidence"):
            self.select(claim, [("another", claim)])

    def test_required_output_uses_native_extension_and_keeps_existing_ownership(self):
        claim = "E9........ 00 0000 0005 !refs :0000 a ^0001 target\n"
        source = self.root / "source.pat"
        source.write_text(claim)
        (self.existing / "runtime.part2.pat").write_text("AABB 00 0000 0002 :0000 old\n")
        self.assertEqual(supplement.main([
            "--candidate", f"runtime={source}", "--evidence", f"runtime={source}",
            "--existing", str(self.existing), "--output", str(self.existing)]), 0)
        self.assertEqual((self.existing / "runtime.part3.npat").read_text(), claim + "---\n")
        self.assertEqual(self.select(claim, [("runtime", claim)]), [])

    def test_single_reference_context_audits_each_clause_against_every_rival(self):
        a = "E8........E9........ 00 0000 000A !refs :0000 a ^0001 first ^0006 last\n"
        b = a.replace(":0000 a", ":0000 b").replace("first", "other")
        result = self.select(a, [("runtime", a + b)], single_reference_context=True)
        self.assertEqual({line.refs for line in result},
                         {((1, "first"), (6, "last")), ((1, "first"),)})
        unknown = "E8........E9........ 00 0000 000A !evidence :0000 unknown\n"
        self.assertEqual(self.select(a, [("runtime", a + b + unknown)],
                                     single_reference_context=True), [])
        covering = b.replace("000A", "000B").replace("E9........ ", "E9........C3 ")
        result = self.select(a, [("runtime", a), ("other", covering)],
                             single_reference_context=True)
        self.assertEqual(len(result), 2)

    def test_single_reference_context_preserves_complete_alternatives(self):
        a = "E8........E9........ 00 0000 000A !refs :0000 a ^0001 first ^0001 alias ^0006 last\n"
        b = "E8........E9........ 00 0000 000A !evidence :0000 b ^0001 alias ^0006 other\n"
        result = self.select(a, [("runtime", a + b)], single_reference_context=True)
        self.assertEqual({line.refs for line in result},
                         {((1, "alias"), (1, "first"), (6, "last")), ((6, "last"),)})
        self.assertFalse(any(line.refs == ((1, "first"),) for line in result))

    def test_single_reference_context_keeps_jointly_necessary_sites(self):
        a = "E8........E9........ 00 0000 000A !refs :0000 a ^0001 first ^0006 last\n"
        b = a.replace(":0000 a", ":0000 b").replace("first", "other")
        c = a.replace(":0000 a", ":0000 c").replace("last", "other")
        result = self.select(a, [("runtime", a + b + c)], single_reference_context=True)
        self.assertEqual([line.text for line in result], [a.strip()])

    def test_single_reference_context_refines_owned_required_but_not_ordinary_claims(self):
        a = "E8........E9........ 00 0000 000A !refs :0000 a ^0001 first ^0006 last\n"
        result = self.select(a, [("runtime", a)], a, single_reference_context=True)
        self.assertEqual({line.refs for line in result}, {((1, "first"),), ((6, "last"),)})
        existing = a + "\n".join(line.text for line in result)
        self.assertEqual(self.select(a, [("runtime", a)], existing,
                                     single_reference_context=True), [])
        self.assertEqual(self.select(a, [("runtime", a)], a.replace(" !refs", ""),
                                     single_reference_context=True), [])
        self.assertEqual(len(self.select(a, [("runtime", a)], a.replace("!refs", "!caller"),
                                         single_reference_context=True)), 3)

    def test_single_reference_context_does_not_promote_optional_or_partial_bodies(self):
        for line in ["E8........E9........ 00 0000 000A :0000 a ^0001 first ^0006 last",
                     "E8........E9........ 00 0000 000B !refs :0000 a ^0001 first ^0006 last"]:
            parsed = builder.parse_line(line)
            self.assertEqual(supplement.reference_variants(parsed), [parsed])

    def test_numbered_output_does_not_replace_existing_parts(self):
        claim = "AABBCCDD 00 0000 0004 :0000 wanted\n"
        source = self.root / "raw.pat"
        source.write_text(claim)
        old = self.existing / "runtime.part2.pat"
        old.write_text("11223344 00 0000 0004 :0000 old\n")
        self.assertEqual(supplement.main([
            "--candidate", f"runtime={source}", "--evidence", f"runtime={source}",
            "--existing", str(self.existing), "--output", str(self.existing)]), 0)
        self.assertIn(":0000 old", old.read_text())
        self.assertEqual((self.existing / "runtime.part3.pat").read_text(), claim + "---\n")


if __name__ == "__main__":
    unittest.main()
