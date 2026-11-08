import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

from scripts.sbf_artifacts import ArtifactWorkspace


@unittest.skipUnless(os.name == "posix", "SBF managed workspace uses POSIX leases")
class SbfArtifactTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def state(self):
        return json.loads((self.root / ".sbf-artifacts/state.json").read_text())

    def test_repeated_success_and_failure_do_not_accumulate_products(self):
        counts = []
        sizes = []
        for i in range(8):
            try:
                with ArtifactWorkspace(self.root) as store:
                    store.begin("probe", {"trial": i})
                    output = store.own(self.root / f"trial-{i}", directory=True)
                    (output / "worker").write_bytes(b"x" * 4096)
                    (output / "stderr.txt").write_text("required failure diagnostic\n")
                    if i % 2:
                        raise ValueError("synthetic worker failure")
            except ValueError:
                pass
            self.assertFalse(output.exists())
            self.assertEqual(self.state()["entries"], {})
            counts.append(len(list(self.root.rglob("*"))))
            sizes.append(sum(p.stat().st_size for p in self.root.rglob("*") if p.is_file()))
        self.assertEqual(len(set(counts)), 1)
        print("LIFECYCLE_METRIC " + json.dumps(dict(case="disposable-success-failure", files_and_dirs=counts, bytes=sizes)))
        summary = json.loads((self.root / ".sbf-artifacts/summaries/probe.json").read_text())
        self.assertIn("synthetic worker failure", summary["disposition"])
        self.assertTrue(any("required failure diagnostic" in v for v in summary["diagnostic_tails"].values()))

    def test_pending_reader_survives_rotation_until_explicit_release(self):
        with ArtifactWorkspace(self.root) as store:
            store.begin("candidate")
            baseline = store.own(self.root / "before", directory=True)
            (baseline / "query.json").write_text('{"target":1971}')
            store.keep("candidate")
            store.pin("baseline-audit", "candidate")
        for i in range(6):
            with ArtifactWorkspace(self.root) as store:
                store.begin("candidate")
                current = store.own(self.root / f"after-{i}", directory=True)
                (current / "query.json").write_text('{"target":1971}')
                store.keep("candidate")
            self.assertTrue(baseline.exists())
            self.assertEqual(len(self.state()["entries"]), 2)
        self.assertEqual((baseline / "query.json").read_text(), '{"target":1971}')
        with ArtifactWorkspace(self.root) as store:
            store.release("baseline-audit")
        self.assertFalse(baseline.exists())
        self.assertTrue(current.exists())

    def test_workers_are_content_deduplicated_and_original_is_untouched(self):
        original = self.root / "reusable-build-worker"
        original.write_bytes(b"same executable" * 100)
        with ArtifactWorkspace(self.root) as store:
            before = store.snapshot_worker(original, "baseline-worker")
            after = store.snapshot_worker(original, "candidate-worker")
            self.assertEqual(before, after)
        self.assertEqual(len(list((self.root / ".sbf-artifacts/workers").iterdir())), 1)
        original.write_bytes(b"new executable" * 100)
        with ArtifactWorkspace(self.root) as store:
            new = store.snapshot_worker(original, "candidate-worker")
        self.assertTrue(before.exists())
        self.assertNotEqual(new, before)
        with ArtifactWorkspace(self.root) as store:
            store.release("baseline-worker")
        self.assertFalse(before.exists())
        self.assertEqual(new.read_bytes(), original.read_bytes())
        self.assertTrue(original.exists())

    def producer(self, *, child=False):
        code = '''
from pathlib import Path
import subprocess,sys,time
from scripts.sbf_artifacts import ArtifactWorkspace,child_options
root=Path(sys.argv[1])
with ArtifactWorkspace(root) as store:
    store.begin("interrupted")
    p=store.own(root/"partial",directory=True)
    (p/"large-output").write_bytes(b"x"*8192)
    (p/"stderr.txt").write_text("before interruption")
    if sys.argv[2]=="child":
        c=subprocess.Popen([sys.executable,"-c","import time;time.sleep(1.5)"],**child_options())
    print("ready",flush=True)
    time.sleep(60)
'''
        process = subprocess.Popen([sys.executable, "-c", code, str(self.root),
                                    "child" if child else "none"],
                                   stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        self.addCleanup(lambda: process.kill() if process.poll() is None else None)
        self.addCleanup(process.stdout.close)
        self.assertEqual(process.stdout.readline().strip(), "ready")
        return process

    def test_termination_cleans_and_keeps_diagnostic(self):
        for interruption in (signal.SIGINT, signal.SIGTERM):
            with self.subTest(signal=interruption):
                process = self.producer()
                process.send_signal(interruption)
                self.assertNotEqual(process.wait(timeout=5), 0)
                self.assertFalse((self.root / "partial").exists())
                summary = json.loads((self.root / ".sbf-artifacts/summaries/interrupted.json").read_text())
                self.assertIn("KeyboardInterrupt", summary["disposition"])
                self.assertEqual(list(summary["diagnostic_tails"].values()), ["before interruption"])

    def test_sigkill_is_recovered_at_next_start_not_by_finally(self):
        process = self.producer()
        process.kill()
        process.wait(timeout=5)
        self.assertTrue((self.root / "partial").exists())
        unrelated = self.root / "original-input.so"
        unrelated.write_bytes(b"do not remove")
        with ArtifactWorkspace(self.root):
            self.assertFalse((self.root / "partial").exists())
        self.assertEqual(unrelated.read_bytes(), b"do not remove")
        summary = json.loads((self.root / ".sbf-artifacts/summaries/interrupted.json").read_text())
        self.assertEqual(summary["disposition"], "interrupted producer recovered")

    def test_orphaned_live_reader_keeps_lease_after_parent_is_killed(self):
        process = self.producer(child=True)
        process.kill()
        process.wait(timeout=5)
        with self.assertRaisesRegex(RuntimeError, "active producer or reader"):
            with ArtifactWorkspace(self.root):
                pass
        self.assertTrue((self.root / "partial").exists())
        deadline = time.monotonic() + 5
        while True:
            try:
                with ArtifactWorkspace(self.root):
                    break
            except RuntimeError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.05)
        self.assertFalse((self.root / "partial").exists())

    def test_source_overlay_restores_uncommitted_input_on_failure(self):
        source = self.root / "research.cpp"
        source.write_bytes(b"uncommitted candidate")
        with ArtifactWorkspace(self.root / "products") as store:
            store.begin("overlay")
            with self.assertRaisesRegex(ValueError, "fixture failure"):
                with store.source_overlay(source, b"temporary instrumentation"):
                    self.assertEqual(source.read_bytes(), b"temporary instrumentation")
                    raise ValueError("fixture failure")
            self.assertEqual(source.read_bytes(), b"uncommitted candidate")

    def test_failed_build_keeps_unchanged_reusable_executable(self):
        executable = self.root/'canonical-worker'
        executable.write_bytes(b'usable current build')
        with ArtifactWorkspace(self.root/'products') as store:
            store.begin('diagnostic')
            store.temporary_build_product(executable)
            # The fixture build failed before producing any changed output.
        self.assertEqual(executable.read_bytes(), b'usable current build')
        with ArtifactWorkspace(self.root/'products') as store:
            store.begin('diagnostic')
            store.temporary_build_product(executable)
            executable.write_bytes(b'partial instrumented build')
        self.assertFalse(executable.exists())

    def test_overlay_intervening_edit_is_preserved_with_recovery_copy(self):
        source = self.root / "research.cpp"
        source.write_bytes(b"uncommitted candidate")
        with self.assertRaisesRegex(ValueError, "source changed"):
            with ArtifactWorkspace(self.root / "products") as store:
                store.begin("overlay")
                with store.source_overlay(source, b"temporary instrumentation"):
                    source.write_bytes(b"new user edit")
        self.assertEqual(source.read_bytes(), b"new user edit")
        self.assertEqual((self.root / "products/.sbf-artifacts/source-original").read_bytes(),
                         b"uncommitted candidate")

    def test_killed_overlay_recovers_source_before_collecting_products(self):
        source = self.root / "research.cpp"
        source.write_bytes(b"uncommitted candidate")
        code = """
from pathlib import Path
import sys,time
from scripts.sbf_artifacts import ArtifactWorkspace
root=Path(sys.argv[1])
with ArtifactWorkspace(root/'products') as store:
    store.begin('overlay')
    p=store.own(root/'products/partial',directory=True)
    (p/'diagnostic').write_bytes(b'x'*8192)
    store.temporary_build_product(root/'diagnostic-worker')
    with store.source_overlay(root/'research.cpp',b'temporary instrumentation'):
        (root/'diagnostic-worker').write_bytes(b'compiled overlay')
        print('ready',flush=True)
        time.sleep(60)
"""
        process = subprocess.Popen([sys.executable, '-c', code, str(self.root)],
                                   stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        try:
            self.assertEqual(process.stdout.readline().strip(), 'ready')
        finally:
            process.kill()
            process.wait(timeout=5)
            process.stdout.close()
        self.assertEqual(source.read_bytes(), b'temporary instrumentation')
        with ArtifactWorkspace(self.root/'products'):
            self.assertEqual(source.read_bytes(), b'uncommitted candidate')
            self.assertFalse((self.root/'products/partial').exists())
            self.assertFalse((self.root/'diagnostic-worker').exists())
        self.assertFalse((self.root/'products/.sbf-artifacts/source-original').exists())

    def test_existing_paths_and_symlink_parents_are_not_owned(self):
        original = self.root / "source"
        original.mkdir()
        alias = self.root / "alias"
        alias.symlink_to(original, target_is_directory=True)
        with ArtifactWorkspace(self.root) as store:
            store.begin("invalid")
            with self.assertRaises(FileExistsError):
                store.own(original, directory=True)
            with self.assertRaises(ValueError):
                store.own(alias / "new-output")
        self.assertTrue(original.exists())


if __name__ == "__main__":
    unittest.main()
