#!/usr/bin/env python3
"""Exercise experiment ownership with KiB fixtures, never a Capstone build."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from capstone_perf_lifecycle import Experiment, Store, write_json


class LifecycleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.input = self.root / "frozen-input.bin"
        self.input.write_bytes(b"original-input")
        self.cache = self.root / "canonical-cache"
        self.cache.mkdir()
        (self.cache / "reusable.o").write_bytes(b"canonical-object")
        self.identities = [
            (p.stat().st_ino, p.read_bytes())
            for p in [self.input, self.cache / "reusable.o"]
        ]

    def tearDown(self):
        self.assertEqual(
            self.identities,
            [
                (p.stat().st_ino, p.read_bytes())
                for p in [self.input, self.cache / "reusable.o"]
            ],
        )
        self.temp.cleanup()

    def test_repeated_success_and_failure_are_bounded(self):
        observed = []
        for iteration in range(8):
            with self.subTest(iteration=iteration):
                try:
                    with Experiment(
                        self.root, "fixture", [self.input, self.cache]
                    ) as run:
                        (run.workspace / "source-link").symlink_to(
                            self.cache, target_is_directory=True
                        )
                        code = (
                            "from pathlib import Path; import os; "
                            "p=Path(os.environ['CAPSTONE_PERF_WORKSPACE']); "
                            "(p/'candidate.a').write_bytes(b'x'*8192); "
                            "(p/'expanded-report.json').write_bytes(b' '*32768); "
                            "print('diagnostic-'+('x'*50000)); "
                            f"raise SystemExit({int(iteration % 2 == 0)})"
                        )
                        run.run("produce", [sys.executable, "-c", code])
                except subprocess.CalledProcessError:
                    self.assertEqual(iteration % 2, 0)
                store = self.root / ".capstone-perf"
                self.assertFalse((store / "current").exists())
                files = [path for path in store.rglob("*") if path.is_file()]
                observed.append(
                    {
                        "round": iteration,
                        "files": len(files),
                        "bytes": sum(path.stat().st_size for path in files),
                        "workspace_directories": sum(
                            path.is_dir() for path in store.iterdir()
                        ),
                    }
                )
                diagnostic = json.loads((store / "last-failure.json").read_text())[
                    "diagnostics"
                ]
                self.assertLessEqual(
                    sum(len(row["tail"].encode()) for row in diagnostic), 16384
                )
                self.assertIn("x", diagnostic[0]["tail"])
        self.assertEqual({row["files"] for row in observed}, {4})
        self.assertEqual({row["workspace_directories"] for row in observed}, {0})
        self.assertLess(
            max(row["bytes"] for row in observed)
            - min(row["bytes"] for row in observed),
            1024,
        )
        print("LIFECYCLE_ROUNDS=" + json.dumps(observed))

    def test_current_and_pending_readers_retain_outputs(self):
        with Experiment(
            self.root, "ready", [self.input, self.cache], ["compare", "publish"]
        ) as run:
            (run.workspace / "candidate.a").write_bytes(b"needed")
            expected = run.workspace
        with Store(self.root) as store:
            store.recover()
            self.assertEqual((expected / "candidate.a").read_bytes(), b"needed")
            store.release("compare", {"checked": True})
            self.assertTrue(expected.exists())
        with self.assertRaisesRegex(RuntimeError, "readers"):
            with Experiment(self.root, "next"):
                self.fail("A new run replaced a pending reader")
        with Store(self.root) as store:
            store.release("publish", {"decision": "retired after comparison"})
        self.assertFalse(expected.exists())

    def test_recovery_refuses_unregistered_tree(self):
        directory = self.root / ".capstone-perf/current"
        directory.mkdir(parents=True)
        (directory / "user-source.c").write_text("original source")
        with self.assertRaisesRegex(ValueError, "Unregistered"):
            with Experiment(self.root, "new"):
                pass
        self.assertEqual((directory / "user-source.c").read_text(), "original source")

    def test_creation_and_retirement_crash_windows(self):
        with Store(self.root) as store:
            owner = {"experiment": "creating", "token": "fixture"}
            state = {"owner": owner, "status": "creating", "readers": [], "steps": []}
            write_json(store.state_path, state)
            store.workspace.mkdir()
            (store.workspace / ".owner.json.tmp").write_text("{")
            store.recover()
            self.assertFalse(store.workspace.exists())
            state = store.start("retiring", [self.input])
            write_json(
                store.directory / "last-retired.json", {"summary": "keep this decision"}
            )
            state["status"] = "retiring"
            write_json(store.state_path, state)
            (store.workspace / ".owner.json").unlink()
            (store.workspace / "logs").rmdir()
            store.recover()
            self.assertFalse(store.workspace.exists())
            self.assertEqual(
                json.loads((store.directory / "last-retired.json").read_text()),
                {"summary": "keep this decision"},
            )

    def launch_waiting_owner(self, delay):
        ready = self.root / "child-ready"
        code = """import sys
from pathlib import Path
from capstone_perf_lifecycle import Experiment
root, ready, delay = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
with Experiment(root, 'signal', [root/'frozen-input.bin']) as run:
    script = "from pathlib import Path; import time; print('child diagnostic', flush=True); Path(%r).write_text('ready'); time.sleep(%s)" % (str(ready), delay)
    run.run('wait', [sys.executable, '-c', script])
"""
        env = os.environ.copy()
        env["PYTHONPATH"] = str(Path(__file__).resolve().parents[1])
        process = subprocess.Popen(
            [sys.executable, "-c", code, str(self.root), str(ready), str(delay)],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        deadline = time.monotonic() + 10
        while not ready.exists() and time.monotonic() < deadline:
            if process.poll() is not None:
                self.fail(process.communicate())
            time.sleep(0.01)
        self.assertTrue(ready.exists())
        return process

    def test_normal_interrupt_cleans_running_child(self):
        process = self.launch_waiting_owner(30)
        process.send_signal(signal.SIGTERM)
        process.communicate(timeout=10)
        self.assertNotEqual(process.returncode, 0)
        self.assertFalse((self.root / ".capstone-perf/current").exists())
        state = json.loads((self.root / ".capstone-perf/last-failure.json").read_text())
        self.assertEqual(state["reason"], "interrupted")
        self.assertIn("child diagnostic", state["diagnostics"][0]["tail"])

    def test_strong_kill_recovers_after_orphaned_child_exits(self):
        process = self.launch_waiting_owner(0.7)
        process.kill()
        process.communicate(timeout=10)
        with self.assertRaisesRegex(RuntimeError, "child still owns"):
            with Store(self.root):
                pass
        deadline = time.monotonic() + 10
        while True:
            try:
                with Store(self.root) as store:
                    store.recover()
                break
            except RuntimeError:
                self.assertLess(time.monotonic(), deadline)
                time.sleep(0.02)
        self.assertFalse((self.root / ".capstone-perf/current").exists())
        state = json.loads((self.root / ".capstone-perf/last-failure.json").read_text())
        self.assertEqual(state["reason"], "recovered-interrupted-run")

    def test_stage_requires_live_owner(self):
        script = "from capstone_perf_lifecycle import managed_workspace; print(managed_workspace())"
        env = os.environ.copy()
        env["PYTHONPATH"] = str(Path(__file__).resolve().parents[1])
        rejected = subprocess.run(
            [sys.executable, "-c", script], env=env, capture_output=True, text=True
        )
        self.assertNotEqual(rejected.returncode, 0)
        with Experiment(self.root, "managed", [self.input]) as run:
            run.run(
                "stage",
                [sys.executable, "-c", script],
                env={"PYTHONPATH": env["PYTHONPATH"]},
            )


if __name__ == "__main__":
    unittest.main()
