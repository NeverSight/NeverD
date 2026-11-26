import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

BENCH = Path(__file__).resolve().parents[1] / "sigprof"
with mock.patch.object(sys, "path", [str(BENCH), *sys.path]):
    import macos_warm_launch as launch
    import macos_warm_report as report


class WarmLaunchAnalysis(unittest.TestCase):
    def test_switch_does_not_change_warm_file_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            name = "libneverd.dylib"
            ids = {}
            for side in ("A", "B"):
                (p / side).mkdir()
                f = p / side / name
                f.write_bytes(side.encode())
                ids[side] = launch.identity(f)
            switch = launch.WarmPath(p)
            for side in ("A", "A", "B", "A", "B", "B"):
                switch.select(side)
                self.assertEqual(launch.identity(switch.live / name), ids[side])
            switch.select(None)
            self.assertTrue(all(launch.identity(p / k / name) == v for k, v in ids.items()))

    def test_ambient_conditions_measured_independently(self):
        def classify(name, before, during, after):
            return report.load_class(dict(condition=name, before=before, during=during, after=after))
        self.assertEqual(classify("ambient", .1, .38, .15), "quiet")
        self.assertEqual(classify("busy", .96, .99, .99), "busy")
        self.assertEqual(classify("quiet", .98, 1, .99), "unclassified")
        self.assertEqual(classify("ambient", .1, .95, .98), "unclassified")

    def test_quiet_wait_keeps_the_last_windows_and_records_timeouts(self):
        def measure(values):
            user = idle = 0
            ticks = [(0, 0, 0, 0)]
            for value in values:
                user += value
                idle += 100 - value
                ticks.append((user, 0, idle, 0))
            clock = mock.Mock()
            clock.ticks.side_effect = ticks
            with mock.patch.object(launch.time, "sleep"), mock.patch.object(launch.time, "monotonic", return_value=0):
                return launch.settled_ambient(clock, 3 * len(values))

        before, windows, settled = measure([90, 10, 20, 30])
        self.assertEqual([round(100 * value) for value in windows], [10, 20, 30])
        self.assertAlmostEqual(before, .2)
        self.assertTrue(settled)
        before, _, settled = measure([30, 30, 40])
        self.assertLess(before, report.QUIET_BACKGROUND_MAX)
        self.assertFalse(settled)
        self.assertEqual(report.load_class(dict(condition="quiet", before=before, during=.4,
                                                after=.1, quiet_settled=settled)), "unclassified")

    def test_same_wall_tail_for_all_components(self):
        keys = ("pre_main_ms", "main_ms", "teardown_ms", "exit_to_observer_ms")
        rows = [dict(zip(keys, [1, 1, 0, 0])) for _ in range(100)]
        for i in range(3):
            rows[i]["main_ms"] += 50
        for i in range(3, 5):
            rows[i]["exit_to_observer_ms"] += 100
        for row in rows:
            row["wall_ms"] = sum(row.values())
            row["spawn_to_birth_ms"] = 0
        for row in rows[:5]:
            row["pre_main_ms"] += 4
            row["wall_ms"] += 4
            row["spawn_to_birth_ms"] = 1
        excess = report.tail_components(rows)
        self.assertEqual(excess["main_ms"], 30)
        self.assertEqual(excess["exit_to_observer_ms"], 40)
        self.assertEqual(excess["spawn_to_birth_ms"], 1)
        self.assertEqual(excess["birth_to_main_ms"], 3)
        self.assertEqual(sum(excess[k] for k in keys), excess["wall_ms"])

    def test_difference_of_percentiles_preserves_pairing(self):
        pairs = [({"wall_ms": v}, {"wall_ms": 19 - v}) for v in range(20)]
        stats = report.summarize_group({"one": pairs, "two": pairs}, False, 80)["metrics"]["wall_ms"]
        self.assertEqual(stats["p95_difference"], 0)
        self.assertEqual(stats["by_round"], {"one": 0, "two": 0})
        self.assertLess(stats["p95_difference_ci95"][0], 0)
        self.assertGreater(stats["p95_difference_ci95"][1], 0)


@unittest.skipUnless(sys.platform == "darwin", "macOS process lifetime counters required")
class WarmLaunchBoundaries(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.root = Path(cls.tmp.name)
        source = BENCH / "macos-warm-launch.c"
        helper, entry = cls.root / "clock.dylib", cls.root / "entry.o"
        cc = ["xcrun", "clang", "-O2", "-Wall", "-Wextra", "-Werror"]
        subprocess.run([*cc, "-dynamiclib", source, "-o", helper], check=True)
        subprocess.run([*cc, "-DNEVERD_WARM_ENTRY", "-c", source, "-o", entry], check=True)
        fixture = cls.root / "delay.c"
        fixture.write_text("""
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void delay(const char *name) {
  const char *phase = getenv("TEST_LAUNCH_PHASE");
  if (phase && strcmp(phase, name) == 0) usleep(35000);
}
__attribute__((constructor)) static void initialize(void) { delay("pre"); }
__attribute__((destructor)) static void finish(void) { delay("teardown"); }
int main(void) { delay("main"); return 0; }
""")
        cls.exe = cls.root / "fixture"
        subprocess.run([*cc, fixture, entry, "-Wl,-e,_neverd_warm_entry", "-o", cls.exe], check=True)
        cls.clock = launch.Clock(helper)

    def test_delays_are_charged_to_their_own_phase(self):
        for phase, field in [("pre", "pre_main_ms"), ("main", "main_ms"), ("teardown", "teardown_ms")]:
            with self.subTest(phase=phase):
                env = {**os.environ, "TEST_LAUNCH_PHASE": phase}
                row = launch.observe(self.clock, [str(self.exe)], env, True, self.root / "stderr")
                self.assertGreaterEqual(row[field], 30)
                parts = sum(row[k] for k in ("pre_main_ms", "main_ms", "teardown_ms", "exit_to_observer_ms"))
                self.assertTrue(math.isclose(parts, row["wall_ms"]))

    def test_reaper_delay_is_outside_kernel_lifetime(self):
        original_select = launch.select.select
        def delay_after_exit(*args):
            result = original_select(*args)
            time.sleep(.035)
            return result
        with mock.patch.object(launch.select, "select", side_effect=delay_after_exit):
            row = launch.observe(self.clock, ["/usr/bin/true"], os.environ, False, self.root / "stderr")
        self.assertGreaterEqual(row["exit_to_observer_ms"], 30)
        self.assertTrue(math.isclose(row["spawn_to_birth_ms"] + row["lifetime_ms"] +
                                     row["exit_to_observer_ms"], row["wall_ms"]))


if __name__ == "__main__":
    unittest.main()
