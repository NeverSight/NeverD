"""Measurements cannot erase refusals, mutate state or invent fallback work."""

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import benchmark_x87_portable as benchmark


def fixture(mixed=False):
    row = {"protocol": 1, "mode": "generated", "shape": 0, "mixed": mixed,
           "iterations": 64, "completed": 62 if mixed else 64, "refused": 2 if mixed else 0,
           "fallback_available": False, "fallback_calls": 0, "elapsed_ns": 1000,
           "peak_rss_kib": 1, "generated_object_bytes": 0, "state": "AA", "memory": "BB",
           "return": "sentinel" if mixed else "2800", "refusal_state": "CC", "refusal_memory": "DD"}
    exact = {"after": "AA", "memory": [{}, {}, {"bytes": "BB"}], "return": "2800"}
    refused = {"after": "CC", "memory": [{}, {}, {"bytes": "DD"}], "return": "sentinel"}
    return row, exact, refused


class PortableMeasurementTests(unittest.TestCase):
    def test_exact_and_mixed_counts_bind_their_frozen_states(self):
        for mixed in (False, True):
            row, exact, refused = fixture(mixed)
            benchmark.validate_measurement(row, "generated", 0, mixed, 64, exact, refused)
        row, exact, refused = fixture(True)
        row["refused"] = 0
        with self.assertRaisesRegex(ValueError, "counts"):
            benchmark.validate_measurement(row, "generated", 0, True, 64, exact, refused)

    def test_changed_state_memory_return_or_refusal_fails(self):
        for field in ("state", "memory", "return", "refusal_state", "refusal_memory"):
            row, exact, refused = fixture(True)
            row[field] = "changed"
            with self.assertRaises(ValueError):
                benchmark.validate_measurement(row, "generated", 0, True, 64, exact, refused)

    def test_invalid_timing_and_fabricated_fallback_fail(self):
        for field, value in (("elapsed_ns", 0), ("elapsed_ns", float('nan')),
                             ("peak_rss_kib", -1), ("completed", True),
                             ("fallback_calls", 1), ("fallback_available", True)):
            row, exact, refused = fixture()
            row[field] = value
            with self.assertRaises(ValueError):
                benchmark.validate_measurement(row, "generated", 0, False, 64, exact, refused)

    def test_native_session_must_match_full_serialized_state(self):
        row, _, _ = fixture()
        row.update(mode="session", logical_wire="CAFE", generated_object_bytes=256)
        benchmark.validate_measurement(row, "session", 0, False, 64, session={"after": "CAFE"})
        with self.assertRaisesRegex(ValueError, "session"):
            benchmark.validate_measurement(row, "session", 0, False, 64, session={"after": "CAFF"})

    def test_raw_transport_keeps_all_eighty_bits_and_canaries(self):
        row, _, _ = fixture()
        row.update(mode="transport", memory="A50100000000000080FF3FA5")
        benchmark.validate_measurement(row, "transport", 0, False, 64)
        row["memory"] = "A50000000000000080FF3FA5"
        with self.assertRaisesRegex(ValueError, "80-bit"):
            benchmark.validate_measurement(row, "transport", 0, False, 64)


if __name__ == "__main__":
    unittest.main()
