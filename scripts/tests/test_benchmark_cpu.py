"""Guard comparable CPU workload inventories and non-timing results."""

import copy
import importlib.util
import json
from pathlib import Path
import unittest


SPEC = importlib.util.spec_from_file_location(
    "benchmark_cpu", Path(__file__).resolve().parents[1] / "benchmark_cpu.py")
BENCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BENCH)


def rows(contract="checked", wide=False):
    workloads = dict(BENCH.WORKLOADS)
    if wide:
        workloads["wide_loop"] = 65570
    return [{"schema_version": 1, "architecture": "aarch64", "contract": contract,
             "backend": "unicorn", "workload": name, "warmup": warmup,
             "sample": 0 if warmup else 1, "elapsed_ns": 100,
             "guest_instructions": count}
            for name, count in workloads.items() for warmup in (True, False)]


def encoded(values):
    return "\n".join(json.dumps(row) for row in values)


class CPUComparisonTest(unittest.TestCase):
    def test_contract_and_optional_workload_must_match_the_request(self):
        for contract in ("checked", "checked-user"):
            for wide in (False, True):
                values = rows(contract, wide)
                self.assertEqual(BENCH.validate(encoded(values), "unicorn", contract, wide), values)
                with self.assertRaises(ValueError):
                    BENCH.validate(encoded(values), "unicorn", contract, not wide)
        with self.assertRaises(ValueError):
            BENCH.validate(encoded(rows("checked-user")), "unicorn")

    def test_partial_duplicate_and_wrong_instruction_counts_are_rejected(self):
        values = rows(wide=True)
        wrong = copy.deepcopy(values)
        wrong[-1]["guest_instructions"] -= 1
        for invalid in (values[:-1], values + values[:1], wrong):
            with self.assertRaises(ValueError):
                BENCH.validate(encoded(invalid), "unicorn", wide_loop=True)

    def test_result_hash_ignores_only_timing_transport_and_row_order(self):
        values = rows(wide=True)
        changed = copy.deepcopy(values)
        for row in changed:
            row["elapsed_ns"] += 1
            row["backend"] = "hvf"
        self.assertEqual(BENCH.result_hash(values), BENCH.result_hash(changed[::-1]))
        changed[0]["guest_instructions"] += 1
        self.assertNotEqual(BENCH.result_hash(values), BENCH.result_hash(changed))

    def test_wide_loop_participates_in_paired_summary(self):
        first, second = rows(wide=True), rows(wide=True)
        for row in first:
            row["elapsed_ns"] *= 2
        result = BENCH.summarize([{"side": "baseline", "rows": first},
                                  {"side": "candidate", "rows": second}], 1, True)
        self.assertEqual(result["wide_loop"]["median_paired_speedup"], 2)


if __name__ == "__main__":
    unittest.main()
