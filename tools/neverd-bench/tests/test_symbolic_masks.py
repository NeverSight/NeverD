"""Independent checks for the benchmark oracle's typed word semantics."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "symbolic_masks", Path(__file__).resolve().parents[1] / "benchmark_symbolic_masks.py")
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


class WordOracleTests(unittest.TestCase):
    def equivalent(self, before, after, width=32):
        bench.check_semantics([before], [{"output": after}], width)

    def test_narrow_product_controls_keep_changed_factor_parity(self):
        for width in (8, 64, 257):
            rows = bench.narrow_product_cross_terms_corpus(width, 36)
            for index in range(0, len(rows), 2):
                with self.assertRaises(AssertionError):
                    self.equivalent(rows[index], rows[index + 1], width)

    def test_residue_powers_keep_independent_parity_and_near_misses(self):
        for width in (8, 16, 32, 64):
            rows = bench.residue_power_corpus(width, 81)
            self.assertEqual(len(rows), 81)
            self.assertEqual(rows, bench.residue_power_corpus(width, 81))
            scale = 3 << (width - 4)
            self.equivalent(rows[0], f"x+y+z+w+{scale}*z*(x&1)", width)
            self.equivalent(rows[1], f"x+y+z+w+{scale}*z*((~x)&1)", width)
            for index in (0, 1, 2):
                with self.assertRaises(AssertionError):
                    self.equivalent(rows[index], "x+y+z+w", width)
            with self.assertRaises(AssertionError):
                self.equivalent(rows[2],
                                f"x+y+z+w+{scale}*z*((~x)&1)", width)

    def test_large_quotients_preserve_high_views_and_low_changes(self):
        for width in (8, 32, 64):
            rows = bench.residue_quotient_corpus(width, 72)
            self.assertEqual(rows, bench.residue_quotient_corpus(width, 72))
            scale = 3 << (width - 4)
            expected = f"x+y+z+w+{scale}*z*(x&1)"
            self.equivalent(rows[0], expected, width)
            self.equivalent(rows[1], expected, width)
            with self.assertRaises(AssertionError):
                self.equivalent(rows[2], expected, width)

    def test_residue_subgroups_keep_every_multiplier_and_parity(self):
        for width in (8, 32, 64):
            rows = bench.residue_subgroup_corpus(width, 54)
            self.assertEqual(len(rows), 54)
            self.assertEqual(rows, bench.residue_subgroup_corpus(width, 54))
            scale = 3 << (width - 4)
            expected = (f"x+y+z+w+{scale}*z*(z+3)*(x&1)"
                        "*(x^(y<<2))*((x+4)^(y<<2))")
            self.equivalent(rows[9], expected, width)
            with self.assertRaises(AssertionError):
                self.equivalent(rows[9], expected.replace("*(z+3)", ""), width)
            with self.assertRaises(AssertionError):
                self.equivalent(rows[11], expected, width)

    def test_oracle_checks_products_beyond_python_recursion_depth(self):
        row = bench.residue_quotient_corpus(32, 72)[-1]
        self.equivalent(row, row)
        with self.assertRaises(AssertionError):
            self.equivalent(row, "0")

    def test_narrow_arithmetic_and_default_width_promotion(self):
        self.equivalent("(x+1)&255", "zext(extract(x,0,8)+1#8,32)")
        self.equivalent("(x&255)+1", "extract(x,0,8)+1")
        self.equivalent("(x-1)&255", "zext(-1#8+extract(x,0,8),32)")

    def test_signed_extension_and_extract(self):
        self.equivalent("(x&255)-2*(x&128)", "sext(extract(x,0,8),32)")
        self.equivalent("255", "zext(extract(0xFF00#16,8,8),32)")
        self.equivalent("x&255", "zext(trunc(x,8),32)")

    def test_lookup_words_and_overshifts(self):
        self.equivalent("x&1", "extract(0xAAAAAAAAAAAAAAAA#64 >> zext(x&63,64),0,8)&1", 8)
        self.equivalent("0", "zext(extract(x,0,8)>>8#8,32)")
        self.equivalent("0", "zext(extract(x,0,8)<<8#8,32)")

    def test_rejects_wrong_result_and_unsupported_operations(self):
        with self.assertRaises(AssertionError):
            self.equivalent("(x&255)+1", "zext(extract(x,0,8)+1#8,32)")
        with self.assertRaises(ValueError):
            self.equivalent("x", "unknown(x)")

    def test_frontier_controls_have_nonzero_witnesses(self):
        for width in (8, 16, 32, 64):
            rows = bench.residue_frontier_corpus(width, 128)
            for expression in rows[112:]:
                with self.assertRaises(AssertionError):
                    self.equivalent(expression, "x+y+z+w", width)


    def test_distributed_prefix_identities_and_controls(self):
        for width in (8, 16, 32, 64):
            rows = bench.distributed_prefix_corpus(width, 128)
            self.assertEqual(len(rows), 128)
            self.assertEqual(rows, bench.distributed_prefix_corpus(width, 128))
            for expression in rows[:16]:
                self.equivalent(expression, "x+y+z+w", width)
            for expression in rows[112:]:
                with self.assertRaises(AssertionError):
                    self.equivalent(expression, "x+y+z+w", width)


    def test_multiple_residual_identities_and_nonzero_controls(self):
        for width in (8, 16, 32, 64):
            rows = bench.multiple_residual_corpus(width, 128)
            self.assertEqual(rows, bench.multiple_residual_corpus(width, 128))
            for block in range(7):
                for flavor in (0, 2, 4, 6):
                    scale = 3 << (width - 3)
                    tail = "*(1+4*y)" if block == 0 and flavor in (0, 4) else ""
                    self.equivalent(rows[16*block+flavor],
                                    f"x+y+z+w+{scale}*(x&1){tail}", width)
            for expression in rows[112:]:
                with self.assertRaises(AssertionError):
                    self.equivalent(expression, "x+y+z+w", width)


    def test_sparse_linear_tails_preserve_high_bit_controls(self):
        for width in (8, 16, 32, 64):
            rows = bench.sparse_linear_mixed_degree_corpus(width, 135)
            self.assertEqual(len(rows), 135)
            for block, degree in enumerate((4, 8, 12, 16, 18)):
                product = "3*" + "*".join(f"v{i}" for i in range(2, degree + 2))
                # Plain and partitioned products have the same independent
                # arithmetic reading, regardless of their factor order.
                for kind in (0, 2):
                    for order in range(3):
                        index = block * 27 + kind * 9 + order
                        self.equivalent(rows[index], product + "+13", width)
                        self.equivalent(rows[index + 3],
                                        product + "+13+7*(v0+v1)", width)
                        with self.assertRaises(AssertionError):
                            self.equivalent(rows[index + 6],
                                            product + "+13+7*(v0+v1)", width)


if __name__ == "__main__":
    unittest.main()
