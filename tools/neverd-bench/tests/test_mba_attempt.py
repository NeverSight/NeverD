"""Exercise producer cleanup with tiny stand-ins, never real SDK copies."""

import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

RUNNER = Path(__file__).resolve().parents[1] / "mba_attempt.py"
FIXTURE = """
import json, os, sys, time
from pathlib import Path
root = Path(os.environ['NEVERD_MBA_ATTEMPT'])
scratch = Path(os.environ['TMPDIR'])
assert Path(sys.argv[1]).read_bytes() == b'original oracle'
(scratch / 'candidate').mkdir()
(scratch / 'candidate' / 'sdk').write_bytes(b'x' * 65536)
(root / 'evidence.json').write_text(json.dumps({'answer': 42}))
(root / 'child.pid').write_text(str(os.getpid()))
if sys.argv[2] == 'waiting':
    time.sleep(60)
sys.exit(9 if sys.argv[2] == 'failure' else 0)
"""


@unittest.skipUnless(os.name == "posix", "local MBA attempts use POSIX process leases")
class MBAAttemptTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="mba-lifecycle-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.managed = self.root / "managed"
        self.oracle = self.root / "oracle"
        self.oracle.write_bytes(b"original oracle")
        self.fixture = self.root / "producer.py"
        self.fixture.write_text(FIXTURE)

    def command(self, outcome):
        return [sys.executable, str(RUNNER), "--directory", str(self.managed),
                "--", sys.executable, str(self.fixture), str(self.oracle), outcome]

    def run_case(self, outcome):
        return subprocess.run(self.command(outcome), capture_output=True, text=True,
                              timeout=10)

    def await_child(self):
        path = self.managed / "current/child.pid"
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if path.exists() and path.read_text():
                return int(path.read_text())
            time.sleep(0.01)
        self.fail("fixture did not become ready")

    def test_success_and_failure_have_bounded_evidence_and_no_scratch(self):
        sizes, counts = [], []
        for index in range(8):
            outcome = "success" if index % 2 == 0 else "failure"
            result = self.run_case(outcome)
            self.assertEqual(result.returncode, 0 if outcome == "success" else 9,
                             result.stderr)
            self.assertFalse((self.managed / "current/tmp").exists())
            self.assertFalse((self.managed / "previous/tmp").exists())
            self.assertEqual(self.oracle.read_bytes(), b"original oracle")
            if index:
                files = [p for p in self.managed.rglob("*") if p.is_file()]
                counts.append(len(files))
                sizes.append(sum(p.stat().st_size for p in files))
        self.assertEqual(len(set(counts)), 1)
        self.assertLess(max(sizes) - min(sizes), 128)
        self.assertEqual(json.loads((self.managed / "current/evidence.json").read_text()),
                         {"answer": 42})

    def test_normal_interrupt_cleans_owned_process_and_scratch(self):
        process = subprocess.Popen(self.command("waiting"), stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
        self.await_child()
        process.terminate()
        _, stderr = process.communicate(timeout=10)
        self.assertEqual(process.returncode, 130, stderr)
        self.assertFalse((self.managed / "current/tmp").exists())
        self.assertEqual(json.loads((self.managed / "current/attempt.json").read_text())
                         ["status"], "interrupted")

    def test_killed_parent_keeps_live_reader_lease_then_recovers_next_start(self):
        process = subprocess.Popen(self.command("waiting"), stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
        child = self.await_child()
        try:
            process.kill()
            process.wait(timeout=5)
            blocked = self.run_case("success")
            self.assertEqual(blocked.returncode, 2)
            self.assertIn("still holds the lease", blocked.stderr)
            self.assertTrue((self.managed / "current/tmp/candidate/sdk").exists())
        finally:
            os.killpg(child, signal.SIGKILL)
            process.communicate(timeout=5)
        # SIGKILL cannot run finally. Wait for the inherited lease to close;
        # the next real startup, rather than the test, removes stale scratch.
        import fcntl
        with (self.managed / "lease").open() as lease:
            fcntl.flock(lease, fcntl.LOCK_EX)
        result = self.run_case("success")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.managed / "previous/tmp").exists())
        previous = json.loads((self.managed / "previous/attempt.json").read_text())
        self.assertEqual(previous["status"], "interrupted")

    def test_unowned_inputs_are_never_adopted_or_removed(self):
        self.managed.mkdir()
        original = self.managed / "input"
        original.write_bytes(b"keep")
        result = self.run_case("success")
        self.assertEqual(result.returncode, 2)
        self.assertEqual(original.read_bytes(), b"keep")

    def test_interrupted_slot_setup_recovers_under_the_parent_owner(self):
        self.assertEqual(self.run_case("success").returncode, 0)
        current = self.managed / "current"
        (current / "attempt.json").unlink()
        (current / "tmp").mkdir()
        (current / "tmp/compiler.o").write_bytes(b"temporary")
        result = self.run_case("success")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.managed / "previous/tmp").exists())
        previous = json.loads((self.managed / "previous/attempt.json").read_text())
        self.assertTrue(previous["setupIncomplete"])
        self.assertEqual(previous["status"], "interrupted")


if __name__ == "__main__":
    unittest.main()
