"""Policy tests for exception-runtime signature coverage accounting."""

from __future__ import annotations

import unittest

from scripts.signatures.build_eh_signatures import parse_pattern_line


class PatternCoverageTests(unittest.TestCase):
    def test_wildcard_padded_short_function_is_fully_covered(self) -> None:
        pattern = parse_pattern_line(
            "AABB.... 00 0000 0002 :0000 __gxx_personality_v0"
        )

        self.assertIsNotNone(pattern)
        assert pattern is not None
        self.assertTrue(pattern.is_fully_verified)
        self.assertEqual(pattern.fixed_bytes, 2)

    def test_fixed_bytes_past_the_function_do_not_strengthen_the_match(self) -> None:
        pattern = parse_pattern_line(
            "AABBCCDD 00 0000 0002 :0000 __gxx_personality_v0"
        )

        self.assertIsNotNone(pattern)
        assert pattern is not None
        self.assertTrue(pattern.is_fully_verified)
        self.assertEqual(pattern.fixed_bytes, 2)
        self.assertFalse(pattern.can_name_a_personality)

    def test_tail_overshoot_is_truncated_at_the_function_boundary(self) -> None:
        pattern = parse_pattern_line(
            "AABB 00 0000 0003 :0000 __gxx_personality_v0 CCDD"
        )

        self.assertIsNotNone(pattern)
        assert pattern is not None
        self.assertTrue(pattern.is_fully_verified)
        self.assertEqual(pattern.fixed_bytes, 3)
        self.assertFalse(pattern.can_name_a_personality)

    def test_prefix_that_stops_before_the_function_is_not_full_coverage(self) -> None:
        pattern = parse_pattern_line(
            "AABB 00 0000 0004 :0000 __gxx_personality_v0"
        )

        self.assertIsNotNone(pattern)
        assert pattern is not None
        self.assertFalse(pattern.is_fully_verified)

    def test_malformed_pattern_bytes_are_not_accepted_as_coverage(self) -> None:
        for line in (
            "AAGG 00 0000 0002 :0000 __gxx_personality_v0",
            "AABB 00 0000 0003 :0000 __gxx_personality_v0 CCGG",
        ):
            with self.subTest(line=line):
                pattern = parse_pattern_line(line)
                self.assertIsNotNone(pattern)
                assert pattern is not None
                self.assertFalse(pattern.is_well_formed)
                self.assertFalse(pattern.is_fully_verified)

    def test_numeric_fields_must_fit_the_loader_contract(self) -> None:
        for line in (
            "AABB 100 0000 0102 :0000 __gxx_personality_v0",
            "AABB 00 10000 0002 :0000 __gxx_personality_v0",
            "AABB 00 0000 100000000 :0000 __gxx_personality_v0",
        ):
            with self.subTest(line=line):
                pattern = parse_pattern_line(line)
                self.assertIsNotNone(pattern)
                assert pattern is not None
                self.assertFalse(pattern.is_well_formed)
                self.assertFalse(pattern.is_fully_verified)

    def test_crc_must_fit_even_when_the_leading_field_reaches_the_end(self) -> None:
        for line in (
            "AABB 01 6E91 0002 :0000 __gxx_personality_v0",
            "AABBCC 01 6E91 0002 :0000 __gxx_personality_v0",
        ):
            with self.subTest(line=line):
                pattern = parse_pattern_line(line)
                self.assertIsNotNone(pattern)
                assert pattern is not None
                self.assertFalse(pattern.is_well_formed)
                self.assertFalse(pattern.is_fully_verified)


if __name__ == "__main__":
    unittest.main()
