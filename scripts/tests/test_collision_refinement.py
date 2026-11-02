"""Source-collision refinements preserve uncertainty and library selection."""

from pathlib import Path
import tempfile
import unittest

from scripts.signatures import build_msvc_signatures as builder
from scripts.signatures import refine_colliding_patterns as refinement


class CollisionRefinementTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.existing = self.root / "existing"
        self.existing.mkdir()
        self.output = self.root / "output"
        self.evidence = self.root / "source.pat"

    def run_refinement(self, files, evidence, machine="x64"):
        for name, text in files.items():
            (self.existing / name).write_text(text + "\n---\n")
        self.evidence.write_text(evidence + "\n---\n")
        return refinement.refine(self.existing, [self.evidence], self.output, machine)

    def test_indistinguishable_names_become_incoming_queries_not_aliases(self):
        line = "488B05........C3 00 0000 0008 :0000 first &0003 heap"
        source = line.replace(":0000 first", "!evidence :0000 second")
        result = self.run_refinement({"runtime.pat": line}, source)
        self.assertEqual(result["counts"], {"caller": 1})
        rows = [builder.parse_line(text) for text in (self.output / "runtime.npat").read_text().splitlines()]
        rows = [row for row in rows if row]
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0].names, (":0000 first",))
        self.assertTrue(rows[0].require_caller)
        self.assertNotIn(":0000 first", (self.output / "runtime.pat").read_text())

    def test_different_reference_targets_need_confirmation(self):
        line = "E8........C3 00 0000 0006 :0000 first ^0001 original"
        source = line.replace(":0000 first", "!evidence :0000 second").replace("original", "different")
        result = self.run_refinement({"runtime.pat": line}, source)
        self.assertEqual(result["counts"], {"required": 1})
        self.assertIn("!refs :0000 first ^0001 original", (self.output / "runtime.npat").read_text())

    def test_call_only_clause_keeps_other_source_references(self):
        body = "E8........E8........C3 00 0000 000B"
        line = body + " :0000 first ^0001 identity ^0006 shared"
        source = body + " !evidence :0000 second ^0001 different ^0006 shared"
        self.run_refinement({"runtime.pat": line}, source)
        rows = [builder.parse_line(text) for text in (self.output / "runtime.npat").read_text().splitlines()]
        rows = [row for row in rows if row]
        self.assertEqual([row.refs for row in rows if row.require_references],
                         [((1, "identity"), (6, "shared"))])
        self.assertEqual([row.refs for row in rows if row.require_caller],
                         [((1, "identity"), (6, "shared"))])

    def test_call_clause_and_incoming_source_consensus_remain_separate(self):
        body = "E8........E8........C3 00 0000 000B"
        line = body + " :0000 first ^0001 first_a ^0001 first_b ^0006 original"
        source = body + " !evidence :0000 second ^0001 other ^0006 different\n"
        source += body + " !evidence :0000 first ^0001 first_a ^0001 first_b"
        self.run_refinement({"runtime.pat": line}, source)
        rows = [builder.parse_line(text) for text in (self.output / "runtime.npat").read_text().splitlines()]
        rows = [row for row in rows if row]
        self.assertEqual([row.refs for row in rows if row.require_references],
                         [((1, "first_a"), (1, "first_b"), (6, "original"))])
        self.assertEqual([row.refs for row in rows if row.require_caller],
                         [((1, "first_a"), (1, "first_b"))])

    def test_selected_clause_preserves_all_alternatives_at_its_site(self):
        body = "E8........E8........C3 00 0000 000B"
        line = body + " :0000 first ^0001 first_a ^0001 first_b ^0006 shared"
        source = body + " !evidence :0000 second ^0001 other ^0006 shared"
        self.run_refinement({"runtime.pat": line}, source)
        rows = [builder.parse_line(text) for text in (self.output / "runtime.npat").read_text().splitlines()]
        self.assertEqual([row.refs for row in rows if row and row.require_references],
                         [((1, "first_a"), (1, "first_b"), (6, "shared"))])

    def test_sufficient_image_metadata_precedes_a_callee_candidate(self):
        body = "E8........FF15........C3 00 0000 000C"
        line = body + " :0000 first ^0001 original ^0007 __imp_First"
        source = body + " !evidence :0000 second ^0001 different ^0007 __imp_Second"
        self.run_refinement({"runtime.pat": line}, source)
        rows = [builder.parse_line(text) for text in (self.output / "runtime.npat").read_text().splitlines()]
        self.assertEqual([row.refs for row in rows if row and row.require_references],
                         [((7, "__imp_First"),)])

    def test_pruned_clause_must_still_distinguish_an_already_loaded_rival(self):
        body = "E8........E8........C3 00 0000 000B"
        first = body + " :0000 first ^0001 identity ^0006 shared"
        second = body + " :0000 second ^0001 identity ^0006 different"
        source = body + " !evidence :0000 third ^0001 different ^0006 shared\n"
        source += second.replace(" :0000", " !evidence :0000")
        self.run_refinement({"runtime.pat": first + "\n" + second}, source)
        rows = [builder.parse_line(text) for text in (self.output / "runtime.npat").read_text().splitlines()]
        first_rows = [row for row in rows if row and row.names == (":0000 first",)]
        self.assertEqual(len(first_rows), 1)
        self.assertTrue(first_rows[0].require_references)
        self.assertEqual(first_rows[0].refs, ((1, "identity"), (6, "shared")))

    def test_known_same_library_cohorts_survive_across_parts(self):
        body = "E8........C3 00 0000 0006"
        first = body + " :0000 first ^0001 original"
        second = body + " :0000 second ^0001 different"
        source = second.replace(" :0000", " !evidence :0000")
        result = self.run_refinement({"runtime.pat": first, "runtime.part2.pat": second}, source)
        self.assertEqual(result["counts"], {})
        self.assertEqual((self.output / "runtime.pat").read_text(), first + "\n---\n")

    def test_an_independently_selected_library_cannot_hide_a_counterclaim(self):
        body = "E8........C3 00 0000 0006"
        first = body + " :0000 first ^0001 original"
        second = body + " :0000 second ^0001 different"
        source = second.replace(" :0000", " !evidence :0000")
        result = self.run_refinement({"vs2017.pat": first, "vs2022.pat": second}, source)
        self.assertEqual(result["counts"], {"required": 1})
        self.assertEqual(result["actions"][0]["file"], "vs2017.pat")

    def test_known_name_with_different_source_references_is_still_a_counterclaim(self):
        body = "E8........C3 00 0000 0006"
        first = body + " :0000 first ^0001 original"
        second = body + " :0000 second ^0001 different"
        source = (body + " !evidence :0000 second ^0001 original")
        result = self.run_refinement({"runtime.pat": first + "\n" + second}, source)
        self.assertEqual(result["counts"], {"caller": 1, "required": 1})
        self.assertEqual((self.output / "runtime.pat").read_text(), "---\n")

    def test_refinement_cannot_leave_an_unqualified_cohort_survivor_in_another_part(self):
        body = "E8........C3 00 0000 0006"
        first = body + " :0000 first ^0001 original"
        second = body + " :0000 second ^0001 different"
        source = body + " !evidence :0000 second ^0001 original"
        result = self.run_refinement({"runtime.pat": first, "runtime.part2.pat": second}, source)
        self.assertEqual(result["counts"], {"caller": 1, "required": 1})
        self.assertEqual((self.output / "runtime.part2.pat").read_text(), "---\n")
        self.assertIn("!refs :0000 second", (self.output / "runtime.part2.npat").read_text())

    def test_existing_required_claim_cannot_ignore_new_indistinguishable_source(self):
        line = "E8........C3 00 0000 0006 !refs :0000 first ^0001 original"
        source = "E8........C3 00 0000 0006 !evidence :0000 second ^0001 original"
        result = self.run_refinement({"runtime.npat": line}, source)
        self.assertEqual(result["counts"], {"caller": 1})
        self.assertIn("!caller :0000 first ^0001 original", (self.output / "runtime.npat").read_text())
        self.assertNotIn("!refs", (self.output / "runtime.npat").read_text())

    def test_existing_required_clause_can_use_independently_sufficient_metadata(self):
        body = "E8........FF15........C3 00 0000 000C"
        line = body + " !refs :0000 first ^0001 original ^0007 __imp_First"
        witness = line.replace("!refs", "!evidence")
        rival = body + " !evidence :0000 second ^0001 different ^0007 __imp_Second"
        result = self.run_refinement({"runtime.npat": line}, witness + "\n" + rival)
        rows = [builder.parse_line(text) for text in
                (self.output / "runtime.npat").read_text().splitlines()]
        rows = [row for row in rows if row]
        self.assertEqual(result["counts"], {"reduced": 1})
        self.assertEqual([row.refs for row in rows if row.require_references],
                         [((7, "__imp_First"),)])
        self.assertEqual([row.refs for row in rows if row.require_caller],
                         [((1, "original"), (7, "__imp_First"))])

    def test_existing_required_clause_needs_its_own_complete_source_witness(self):
        body = "E8........FF15........C3 00 0000 000C"
        line = body + " !refs :0000 first ^0001 original ^0007 __imp_First"
        witness = body + " !evidence :0000 first ^0001 original"
        rival = body + " !evidence :0000 second ^0001 different ^0007 __imp_Second"
        result = self.run_refinement({"runtime.npat": line}, witness + "\n" + rival)
        self.assertEqual(result["counts"], {})
        self.assertEqual((self.output / "runtime.npat").read_text(), line + "\n---\n")

    def test_required_reduction_preserves_other_loaded_required_claims(self):
        body = "E8........FF15........C3 00 0000 000C"
        line = body + " !refs :0000 first ^0001 original ^0007 __imp_First"
        other = body + " !refs :0000 second ^0001 different ^0007 __imp_First"
        witness = line.replace("!refs", "!evidence")
        rival = body + " !evidence :0000 third ^0001 original ^0007 __imp_Second"
        self.run_refinement({"runtime.npat": line + "\n" + other}, witness + "\n" + rival)
        rows = [builder.parse_line(text) for text in
                (self.output / "runtime.npat").read_text().splitlines()]
        first = [row for row in rows if row and row.names == (":0000 first",)
                 and row.require_references]
        self.assertEqual([row.refs for row in first],
                         [((1, "original"), (7, "__imp_First"))])

    def test_existing_required_clause_keeps_call_only_constraints(self):
        body = "E8........E8........C3 00 0000 000B"
        line = body + " !refs :0000 first ^0001 original ^0006 shared"
        witness = line.replace("!refs", "!evidence")
        rival = body + " !evidence :0000 second ^0001 different ^0006 shared"
        result = self.run_refinement({"runtime.npat": line}, witness + "\n" + rival)
        self.assertEqual(result["counts"], {})
        self.assertEqual((self.output / "runtime.npat").read_text(), line + "\n---\n")

    def test_required_reduction_retains_a_jointly_necessary_site(self):
        body = "E8........FF15........C3 00 0000 000C"
        line = body + " !refs :0000 first ^0001 original ^0007 __imp_First"
        witness = line.replace("!refs", "!evidence")
        rivals = body + " !evidence :0000 second ^0001 original ^0007 __imp_Second\n"
        rivals += body + " !evidence :0000 third ^0001 different ^0007 __imp_First"
        result = self.run_refinement({"runtime.npat": line}, witness + "\n" + rivals)
        self.assertEqual(result["counts"], {})
        self.assertEqual((self.output / "runtime.npat").read_text(), line + "\n---\n")

    def test_existing_required_claim_survives_distinct_source_references(self):
        line = "E8........C3 00 0000 0006 !refs :0000 first ^0001 original"
        source = "E8........C3 00 0000 0006 !evidence :0000 second ^0001 different"
        result = self.run_refinement({"runtime.npat": line}, source)
        self.assertEqual(result["counts"], {})
        self.assertEqual((self.output / "runtime.npat").read_text(), line + "\n---\n")

    def test_matching_crc_cannot_hide_a_shorter_return_body(self):
        lead = "48894C24084883EC3848C7442420FEFFFFFF488B4C2440E8........90488B4C"
        tail = "........904883C438C3"
        line = lead + " 03 B68B 002E :0000 wrapper ^0018 throw ^0023 destroy " + tail + "CC"
        source = lead + " 03 B68B 002D !evidence :0000 container ^0018 tidy ^0023 destroy " + tail
        result = self.run_refinement({"runtime.pat": line}, source)
        self.assertEqual(result["counts"], {"required": 1})
        rows = [builder.parse_line(text) for text in
                (self.output / "runtime.npat").read_text().splitlines()]
        self.assertEqual([row.refs for row in rows if row and row.require_references],
                         [((0x18, "throw"), (0x23, "destroy"))])

    def test_crc_padding_requires_matching_spans_and_visible_return_and_padding(self):
        long = builder.Shape.of("9090 02 1234 0007 C390CC")
        short = builder.Shape.of("9090 02 1234 0005 C3")
        self.assertTrue(long.padding_collision_with(short, machine="x64"))
        for other in ["9090 02 4321 0005 C3", "90 03 1234 0005 C3",
                      "9090 03 1234 0005", "9090 02 1234 0005 .."]:
            self.assertFalse(long.padding_collision_with(builder.Shape.of(other), machine="x64"))
        for other in ["9090 02 1234 0007 C3..CC", "9090 02 1234 0007 C301CC"]:
            self.assertFalse(builder.Shape.of(other).padding_collision_with(short, machine="x64"))
        self.assertFalse(long.padding_collision_with(short, machine="arm64"))
        thumb = builder.Shape.of("0090 02 1234 0008 704700BF")
        self.assertTrue(thumb.padding_collision_with(
            builder.Shape.of("0090 02 1234 0006 7047"), machine="arm"))

    def test_thumb_padding_query_includes_shorter_literal_source_prefixes(self):
        body = "2DE9F84F0DF1200B93E8F0079047BDE8F88F"
        line = body + "0000 00 0000 0014 :0000 filter"
        source = body + " 00 0000 0012 !evidence :0000 termination"
        result = self.run_refinement({"runtime.pat": line}, source, "arm")
        self.assertEqual(result["counts"], {"caller": 1})

    def test_source_can_state_the_same_bytes_with_a_shorter_leading_field(self):
        line = "AABBCCDDEEFF0011 00 0000 0008 :0000 first"
        source = "AABBCCDD 00 0000 0008 !evidence :0000 second EEFF0011"
        result = self.run_refinement({"runtime.pat": line}, source)
        self.assertEqual(result["counts"], {"caller": 1})

    def test_wildcards_cover_crc_bytes_but_do_not_claim_their_values(self):
        lead = "90" * 32
        line = f"{lead} 00 0000 002C :0000 loose " + ".." * 8 + "AABBCCDD"
        source = f"{lead} 08 1234 002C !evidence :0000 precise AABBCCDD"
        result = self.run_refinement({"runtime.pat": line}, source)
        self.assertEqual(result["counts"], {"caller": 1})

    def test_partial_conflicting_records_cannot_be_promoted_to_context(self):
        line = "AABBCCDD 00 0000 0008 :0000 partial"
        source = "AABBCCDD112233C3 00 0000 0008 !evidence :0000 complete"
        result = self.run_refinement({"runtime.pat": line}, source)
        self.assertEqual(result["counts"], {"omitted": 1})
        self.assertFalse((self.output / "runtime.npat").exists())

    def test_real_alias_and_explicit_constraints_remain_unchanged(self):
        line = "E8........C3 00 0000 0006 :0000 first :0000 shared ^0001 original"
        constrained = line.replace(" :0000 first", " !refs :0000 first")
        source = line.replace(":0000 first", "!evidence :0000 second")
        result = self.run_refinement({"runtime.pat": line, "runtime.part2.npat": constrained}, source)
        self.assertEqual(result["counts"], {})

    def test_pruned_sources_and_existing_destinations_fail_before_writes(self):
        line = "AABBCCDD 00 0000 0004 :0000 first"
        with self.assertRaisesRegex(builder.BuildError, "unpruned"):
            self.run_refinement({"runtime.pat": line}, line)
        self.assertFalse(self.output.exists())
        self.output.mkdir()
        sentinel = self.output / "keep"
        sentinel.write_text("unchanged")
        with self.assertRaisesRegex(builder.BuildError, "already exists"):
            refinement.refine(self.existing, [self.evidence], self.output, "x64")
        self.assertEqual(sentinel.read_text(), "unchanged")


if __name__ == "__main__":
    unittest.main()
