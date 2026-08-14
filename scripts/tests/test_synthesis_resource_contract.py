from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
ENUMERATE = ROOT / "lib" / "symbolic" / "synth" / "SymSynthEnumerate.cpp"
STOCHASTIC = ROOT / "lib" / "symbolic" / "synth" / "SymSynthStochastic.cpp"


class SynthesisResourceContractTests(unittest.TestCase):
    def test_enumeration_has_no_implicit_max_cost_ceiling(self) -> None:
        source = ENUMERATE.read_text(encoding="utf-8")
        self.assertIsNone(re.search(r"clamp[^\n;]*Opts\.MaxCost", source))

    def test_sampler_has_no_implicit_slot_or_scratch_ceiling(self) -> None:
        source = STOCHASTIC.read_text(encoding="utf-8")
        self.assertNotIn("kMaxSlots", source)
        self.assertIsNone(re.search(r"Scratch\s*\(\s*32\b", source))
        self.assertIsNotNone(re.search(r"Scratch\s*\(\s*NumSlots\s*,", source))
        self.assertIsNone(re.search(r"clamp[^\n;]*Opts\.StochasticSlots", source))

    def test_sampled_program_cost_uses_saturating_arithmetic(self) -> None:
        source = STOCHASTIC.read_text(encoding="utf-8")
        begin = source.index("size_t programCost(")
        end = source.index("\n}\n", begin) + 2
        function = source[begin:end]
        self.assertIn("saturatingAdd", function)
        self.assertIsNone(re.search(r"\b1\s*\+\s*Cost\[", function))


if __name__ == "__main__":
    unittest.main()
