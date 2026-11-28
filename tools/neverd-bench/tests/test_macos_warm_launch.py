from collections import Counter
from contextlib import ExitStack
import itertools
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
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

    def test_control_inodes_are_independent_and_old_records_still_report(self):
        with tempfile.TemporaryDirectory() as directory, ExitStack() as patches:
            root = Path(directory)
            scratch = root / "trial"
            scratch.mkdir()
            (root / "bin").mkdir()
            for name in ("before", "after", "bin/neverd", "probe", "marked"):
                (root / name).write_text(name)
            args = SimpleNamespace(build=root, baseline_sdk=root / "before",
                                   candidate_sdk=root / "after", image=root / "probe",
                                   function="fixture", rounds=2, pairs=4, control_load=False,
                                   quiet_wait=0, entry_counters=False)
            ticks = itertools.count(1)
            clock = mock.Mock()
            clock.ticks.side_effect = lambda: (next(ticks), 0, 1, 0)

            def observe(clock, argv, env, marked, errors, *, entry_counters):
                sdk = Path(env["DYLD_LIBRARY_PATH"]) / "libneverd.dylib"
                Path(argv[-1]).write_text("output")
                errors.write_text(f"dyld[1]: <uuid> {sdk}\n")
                return dict(inode=sdk.stat().st_ino, wall_ms=3, pre_main_ms=1, main_ms=1,
                            teardown_ms=1, spawn_to_birth_ms=0, exit_to_observer_ms=0)

            patches.enter_context(mock.patch.object(launch, "prepare", return_value=(clock, root / "marked")))
            patches.enter_context(mock.patch.object(launch, "observe", side_effect=observe))
            patches.enter_context(mock.patch.object(launch.subprocess, "run"))
            patches.enter_context(mock.patch.object(launch.time, "sleep"))
            patches.enter_context(mock.patch.object(launch.os, "getloadavg", return_value=(0, 0, 0), create=True))
            patches.enter_context(mock.patch.object(launch, "ambient", return_value=.1))
            path = root / "raw.jsonl"
            with path.open("w") as output:
                launch.measure(args, scratch, output)
            rows = [json.loads(line) for line in path.read_text().splitlines()]
            samples = [r for r in rows if r["event"] == "sample"]
            inodes = {side: {r["inode"] for r in samples if r["side"] == side}
                      for side in ("A", "B", "A1", "A2")}
            self.assertTrue(all(len(ids) == 1 for ids in inodes.values()))
            self.assertEqual(len(set.union(*inodes.values())), 4)
            for kind in ("stock", "marked"):
                uses = Counter(r["side"] for r in samples if r["kind"] == kind)
                self.assertEqual(uses, {"A": 8, "B": 8, "A1": 4, "A2": 4})

            current = report.summarize(path, draws=8)
            del rows[0]["sdk_sha256"]
            for row in samples:
                if row["side"] == "A1":
                    row["side"] = "A"
            path.write_text("".join(json.dumps(r) + "\n" for r in rows))
            self.assertEqual(current["groups"], report.summarize(path, draws=8)["groups"])

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

    def test_entry_bounds_keep_unobserved_gate_time_uncertain(self):
        row = dict(birth_to_main_ms=100, cpu_at_entry_ms=21, cpu_at_stop_ms=22,
                   runnable_at_entry_ms=70, runnable_at_stop_ms=90,
                   probe_to_gate_ms=2, gate_to_observer_ms=3)
        self.assertEqual(report.entry_bounds(row),
                         {"cpu_ms": (19, 21), "ready_ms": (64, 71), "other_ms": (10, 15)})
        row["gate_to_observer_ms"] = 30
        bounds = report.entry_bounds(row)
        self.assertEqual(bounds["other_ms"], (10, 32))
        row["runnable_at_entry_ms"] = 120
        with self.assertRaises(ValueError):
            report.entry_bounds(row)

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
        premain = BENCH / "macos-premain.c"
        subprocess.run([*cc, "-dynamiclib", source, premain, "-o", helper], check=True)
        subprocess.run([*cc, "-DNEVERD_WARM_ENTRY", "-c", source, "-o", entry], check=True)
        fixture = cls.root / "delay.c"
        fixture.write_text("""
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void delay(const char *name) {
  const char *phase = getenv("TEST_LAUNCH_PHASE");
  if (phase && strcmp(phase, name) == 0) usleep(35000);
}
static void *worker(void *arg) { return arg; }
__attribute__((constructor)) static void initialize(void) {
  delay("pre");
  const char *phase = getenv("TEST_LAUNCH_PHASE");
  if (phase && strcmp(phase, "joined") == 0) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, worker, NULL) || pthread_join(thread, NULL))
      abort();
  }
  if (phase && strcmp(phase, "cpu") == 0) {
    uint64_t start = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
    while (clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - start < 25000000) {}
  }
}
__attribute__((destructor)) static void finish(void) { delay("teardown"); }
int main(void) { delay("main"); return 0; }
""")
        cls.exe = cls.root / "fixture"
        subprocess.run([*cc, fixture, entry, "-Wl,-e,_neverd_warm_entry", "-o", cls.exe], check=True)
        subprocess.run([*cc, "-DNEVERD_PREMAIN_ENTRY", "-c", premain, "-o", entry], check=True)
        cls.accounted = cls.root / "accounted"
        subprocess.run([*cc, fixture, entry, "-Wl,-e,_neverd_premain_entry", "-o", cls.accounted],
                       check=True)
        cls.clock = launch.Clock(helper)

    def test_delays_are_charged_to_their_own_phase(self):
        for phase, field in [("pre", "pre_main_ms"), ("main", "main_ms"), ("teardown", "teardown_ms")]:
            with self.subTest(phase=phase):
                env = {**os.environ, "TEST_LAUNCH_PHASE": phase}
                row = launch.observe(self.clock, [str(self.exe)], env, True, self.root / "stderr")
                self.assertGreaterEqual(row[field], 30)
                parts = sum(row[k] for k in ("pre_main_ms", "main_ms", "teardown_ms", "exit_to_observer_ms"))
                self.assertTrue(math.isclose(parts, row["wall_ms"]))

    def test_entry_distinguishes_constructor_cpu_from_sleep(self):
        for phase, field, minimum in (("cpu", "cpu_ms", 24), ("pre", "other_ms", 30)):
            with self.subTest(phase=phase):
                row = launch.observe(self.clock, [str(self.accounted)],
                                     dict(os.environ, TEST_LAUNCH_PHASE=phase),
                                     True, self.root / "stderr", entry_counters=True)
                lower, upper = report.entry_bounds(row)[field]
                gate = row["probe_to_gate_ms"] + row["gate_to_observer_ms"]
                self.assertGreaterEqual(upper, minimum)
                self.assertGreaterEqual(lower + gate, minimum)

    def test_joined_initializer_thread_prevents_wall_partition(self):
        with self.assertRaisesRegex(ValueError, "initializer threads"):
            launch.observe(self.clock, [str(self.accounted)], dict(os.environ, TEST_LAUNCH_PHASE="joined"),
                           True, self.root / "stderr", entry_counters=True)

    def test_pause_after_entry_is_outside_main(self):
        original_snapshot = launch.snapshot
        def delayed_snapshot(clock, pid):
            counters = original_snapshot(clock, pid)
            time.sleep(.04)
            return counters
        with mock.patch.object(launch, "snapshot", side_effect=delayed_snapshot):
            row = launch.observe(self.clock, [str(self.accounted)],
                                 dict(os.environ, TEST_LAUNCH_PHASE="main"),
                                 True, self.root / "stderr", entry_counters=True)
        self.assertGreaterEqual(row["observed_pause_ms"], 35)
        self.assertGreaterEqual(row["main_ms"], 30)
        self.assertNotIn("wall_ms", row)

    def test_exit_before_stop_reaps_the_child(self):
        original_spawn = os.posix_spawn
        children = []
        def spawn(*args, **kwargs):
            children.append(original_spawn(*args, **kwargs))
            return children[-1]
        with mock.patch.object(launch.os, "posix_spawn", side_effect=spawn):
            with self.assertRaises(OSError):
                launch.observe(self.clock, ["/usr/bin/true"], os.environ, True,
                               self.root / "stderr", entry_counters=True)
        with self.assertRaises(ChildProcessError):
            os.waitpid(children[0], os.WNOHANG)

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
