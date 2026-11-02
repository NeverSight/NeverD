"""Immutable metadata refinements preserve bytes and retain counterexamples."""

from pathlib import Path
import tempfile
import unittest

from scripts.signatures import build_msvc_signatures as builder
from scripts.signatures import refine_pattern_metadata as metadata


class MetadataRefinementTests(unittest.TestCase):
    def test_external_operands_keep_all_versions_kinds_and_existing_fields(self):
        old = "488D05........C3 00 0000 0008 !caller :0000 getter\n"
        raw = old.replace("!caller", "!evidence")
        first = raw.rstrip() + " &0003 narrow\n"
        second = first.replace("narrow", "wide")
        self.assertEqual(self.refine(old, [first]), old)
        for sources in ([first, second], [second, first]):
            result = builder.parse_line(self.refine(old, sources, external_data=True))
            self.assertEqual(result.refs, ((3, "narrow"), (3, "wide")))
            self.assertEqual(result.key, builder.parse_line(old).key)
            self.assertEqual(result.data_sites, {3})
            self.assertFalse(result.local_sites)
            for unknown in (raw, first.replace("&0003", "^0003"),
                            raw.rstrip() + " %0003 00000013:anchor\n"):
                self.assertEqual(self.refine(old, [*sources, unknown], external_data=True,
                                             local_data=True, caller_code=True), old)
        ordinary = old.replace(" !caller", "")
        self.assertEqual(self.refine(ordinary, [first], external_data=True), ordinary)
        for kind in ("^", "&", "%"):
            original = "00000013:anchor" if kind == "%" else "original"
            prior = old.rstrip() + f" {kind}0003 {original}\n"
            self.assertEqual(self.refine(prior, [first], external_data=True), prior)
        aliases = old.replace("getter", "getter :0000 missing")
        self.assertEqual(self.refine(aliases, [first], external_data=True), aliases)

    def test_throw_info_fields_need_metadata_kind_and_every_raw_version(self):
        old = "90909090488D05........C3 00 0000 000C :0000 thrower\n"
        raw = old.replace(" :", " !evidence :")
        first = raw.rstrip() + " ^0007 _TI2?AVWidget@@\n"
        second = first.replace("Widget", "Other")
        result = builder.parse_line(self.refine(old, [first, second]))
        self.assertEqual(result.refs, ((7, "_TI2?AVOther@@"), (7, "_TI2?AVWidget@@")))
        for unknown in (raw, first.replace("^0007", "&0007")):
            self.assertEqual(self.refine(old, [first, unknown]), old)
        for name in ("_TI0?AVWidget@@", "_TI02?AVWidget@@", "_TIVC2?AVWidget@@", "_TI1H"):
            self.assertFalse(metadata.immutable_reference(name))

    def test_caller_code_refinement_retains_all_sources_and_reference_kinds(self):
        old = "90909090488D05........C3 00 0000 000C !caller :0000 parent\n"
        raw = old.replace("!caller", "!evidence")
        first = raw.rstrip() + " ^0007 child\n"
        second = first.replace("child", "other")
        self.assertEqual(self.refine(old, [first]), old)
        for sources in ([first, second], [second, first]):
            changed = builder.parse_line(self.refine(old, sources, caller_code=True))
            self.assertEqual(changed.key, builder.parse_line(old).key)
            self.assertTrue(changed.require_caller)
            self.assertEqual(changed.refs, ((7, "child"), (7, "other")))
            for missing in (raw, first.replace("^0007", "&0007")):
                self.assertEqual(self.refine(old, [*sources, missing], caller_code=True), old)
        for kind in ("", " !refs"):
            ordinary = old.replace(" !caller", kind)
            self.assertEqual(self.refine(ordinary, [first], caller_code=True), ordinary)
        existing = old.rstrip() + " ^0007 original\n"
        self.assertEqual(self.refine(existing, [first], caller_code=True), existing)

    def refine(self, old, sources, names=None, **options):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            original = root / "runtime.pat"
            original.write_text(old)
            evidence = []
            for index, text in enumerate(sources):
                path = root / f"raw{index}.pat"
                path.write_text(text)
                evidence.append(path)
            result = metadata.refine([original], evidence, names, **options)
            self.assertEqual(original.read_text(), old)
            return result.get(original, old)

    def test_local_relations_need_unanimous_raw_ownership_and_keep_old_bodies(self):
        old = "90909090488D05........C3 00 0000 000C !caller :0000 anchor\n"
        raw = old.replace("!caller", "!evidence")
        local = raw.rstrip() + " %0007 00000007:anchor\n"
        self.assertEqual(self.refine(old, [local]), old)
        changed = self.refine(old, [local], local_data=True)
        result = builder.parse_line(changed)
        self.assertEqual(result.local_sites, {7})
        self.assertEqual(result.key, builder.parse_line(old).key)
        self.assertEqual(self.refine(old, [local, raw], local_data=True), old)
        ordinary = old.replace(" !caller", "")
        self.assertEqual(self.refine(ordinary, [local], local_data=True), ordinary)

    def test_local_alternatives_are_complete_and_never_hide_a_missing_site(self):
        old = "90909090488D05........C3 00 0000 000C !refs :0000 consumer\n"
        raw = old.replace("!refs", "!evidence")
        first = raw.rstrip() + " %0007 00000013:anchor\n"
        second = raw.rstrip() + " %0007 00000014:anchor\n"
        for sources in ([first, second], [second, first]):
            result = builder.parse_line(self.refine(old, sources, local_data=True))
            self.assertEqual(result.refs, ((7, "00000013:anchor"), (7, "00000014:anchor")))
            self.assertEqual(result.local_sites, {7})
            self.assertEqual(self.refine(old, [*sources, raw], local_data=True), old)
        existing = old.rstrip() + " &0007 external\n"
        self.assertEqual(self.refine(existing, [first, second], local_data=True), existing)

    def test_same_body_gains_only_missing_immutable_fields(self):
        old = "AABBCCDD....C3 00 0000 0007 :0000 routine ^0003 existing\n"
        raw = old.replace(" :", " !evidence :")
        raw = raw.rstrip() + " ^0004 ??_7Widget@@6B@ ^0005 helper ^0006 __imp_API\n"
        result = builder.parse_line(self.refine(old, [raw]))
        self.assertEqual(result.key, builder.parse_line(old).key)
        self.assertEqual(result.names, builder.parse_line(old).names)
        self.assertEqual(result.refs, ((3, "existing"), (4, "??_7Widget@@6B@"), (6, "__imp_API")))

    def test_data_spelling_cannot_become_immutable_metadata(self):
        old = "AABBCCDD 00 0000 0004 :0000 routine\n"
        raw = "AABBCCDD 00 0000 0004 !evidence :0000 routine &0001 __imp_API\n"
        self.assertEqual(self.refine(old, [raw]), old)
        self.assertEqual(self.refine(old, [raw.replace("__imp_API", "??_7Widget@@6B@")]), old)
        function_kind = raw.replace("&0001", "^0001")
        self.assertEqual(self.refine(old, [raw, function_kind]), old)

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
