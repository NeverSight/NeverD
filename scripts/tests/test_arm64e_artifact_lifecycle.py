"""Exercise the actual SDK producer's ownership and interrupted-run recovery."""

import fcntl
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2] / "unittests"
SPEC = importlib.util.spec_from_file_location(
    "arm64e_source_calls_test", ROOT / "arm64e_source_calls_test.py")
SDK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SDK)
CASES = json.loads((ROOT / "Arm64eArtifactLifecycleFixtures.def").read_text())


class Arm64eArtifactLifecycleTests(unittest.TestCase):
    measurements = {}

    def test_pending_compiler_keeps_ownership_after_controller_strong_kill(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            release = root / CASES["consumer_release"]
            with subprocess.Popen([sys.executable, "-c", CASES["producer"],
                                   str(root), CASES["consumer"]],
                                  env=dict(os.environ, PYTHONPATH=str(ROOT.parent / "scripts"))) as child:
                try:
                    end = time.monotonic() + CASES["consumer_wait_seconds"]
                    while not (root / CASES["consumer_ready"]).exists() and time.monotonic() < end:
                        time.sleep(0.01)
                    self.assertTrue((root / CASES["consumer_ready"]).exists())
                    child.kill()
                    child.wait(timeout=CASES["child_timeout"])
                    with (root / SDK.OwnedProducts(root, False).policy["lock"]).open("a+") as lock:
                        with self.assertRaises(BlockingIOError):
                            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    self.assertTrue((root / CASES["product"]).exists())
                    release.touch()
                    with SDK.OwnedProducts(root, False):
                        self.assertFalse((root / CASES["product"]).exists())
                finally:
                    release.touch()
                    if child.poll() is None:
                        child.kill()
                        child.wait()

    def test_registered_partial_products_clean_on_every_exit(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            counts = []
            for error in (None, RuntimeError, KeyboardInterrupt, SystemExit):
                for _ in range(CASES["iterations"]):
                    try:
                        with SDK.OwnedProducts(root, False) as products:
                            product = root / CASES["product"]
                            products.append(product)
                            product.write_text(CASES["payload"])
                            (root / CASES["input"]).write_text(CASES["payload"])
                            (root / CASES["diagnostic"]).write_text(CASES["payload"])
                            if error:
                                raise error()
                    except (RuntimeError, KeyboardInterrupt, SystemExit):
                        pass
                    self.assertFalse(product.exists())
                    counts.append((len(list(root.iterdir())),
                                   sum(p.stat().st_size for p in root.iterdir())))
            self.assertEqual(len(set(counts)), 1)
            self.measurements["sdk_exits"] = counts

    def test_actual_sdk_entry_reuses_the_original_fixture_after_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            compilations = []

            def command(args, **kwargs):
                if "-o" in args:
                    compilations.append(args)
                    Path(args[args.index("-o") + 1]).write_text(CASES["payload"])
                return subprocess.CompletedProcess(args, 0, CASES["compiler_version"])

            args = [str(ROOT), "--library", CASES["library"], "--compiler",
                    CASES["compiler"], "--artifacts", str(root)]
            counts = []
            for _ in range(CASES["iterations"]):
                with mock.patch.object(sys, "argv", args), \
                     mock.patch.object(SDK.subprocess, "run", side_effect=command), \
                     mock.patch.object(SDK.ctypes, "CDLL", side_effect=RuntimeError), \
                     self.assertRaises(RuntimeError):
                    SDK.main()
                self.assertEqual((root / CASES["input"]).read_text(), CASES["payload"])
                counts.append((len(list(root.iterdir())),
                               sum(p.stat().st_size for p in root.iterdir())))
            self.assertEqual(len(compilations), 1)
            self.assertEqual(len(set(counts)), 1)
            self.measurements["sdk_entry"] = counts

    def test_strong_kill_recovery_and_explicit_retention(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with subprocess.Popen([sys.executable, "-c", CASES["child"], str(root)],
                                  stdout=subprocess.PIPE, text=True,
                                  env=dict(os.environ, PYTHONPATH=str(ROOT))) as child:
                try:
                    self.assertEqual(child.stdout.readline().strip(), CASES["ready"])
                    with (root / SDK.OwnedProducts(root, False).policy["lock"]).open("a+") as lock:
                        with self.assertRaises(BlockingIOError):
                            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    child.send_signal(signal.SIGKILL)
                    child.wait(timeout=CASES["child_timeout"])
                finally:
                    if child.poll() is None:
                        child.kill()
                        child.wait()
            product = root / CASES["product"]
            self.assertTrue(product.exists())
            with SDK.OwnedProducts(root, True) as products:
                self.assertFalse(product.exists())
                products.append(product)
                product.write_text(CASES["payload"])
            with SDK.OwnedProducts(root, False):
                self.assertEqual(product.read_text(), CASES["payload"])

    def test_sp_verifier_entry_preserves_inputs_and_cleans_failed_compilation(self):
        import verify_arm64e_sp_memory as verifier

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            artifacts, output = root / "input", root / CASES["output"]
            artifacts.mkdir()
            report = root / "tests.json"
            report.write_text("{}\n")
            for name in verifier.CONFIG["input_files"]:
                (artifacts / name).write_text(CASES["payload"])
            (artifacts / verifier.CONFIG["harness_file"]).write_text(
                verifier.CONFIG["main_token"] + " {}\n")
            args = [str(ROOT), "--stack-artifacts", str(artifacts), "--stack-report",
                    str(report), "--neverd-revision", "0" * 40, "--sdk", str(root),
                    "--output", str(output), "--profile", "sp-writeback"]

            def command(args, **kwargs):
                if "-o" in args:
                    Path(args[args.index("-o") + 1]).write_text(CASES["payload"])
                    return subprocess.CompletedProcess(args, 1, CASES["payload"])
                return subprocess.CompletedProcess(args, 0, CASES["compiler_version"])

            counts = []
            for _ in range(CASES["iterations"]):
                with mock.patch.object(sys, "argv", args), \
                     mock.patch.object(verifier, "validate_report", return_value=str(artifacts)), \
                     mock.patch.object(verifier.platform, "system", return_value="Darwin"), \
                     mock.patch.object(verifier.platform, "machine", return_value="arm64"), \
                     mock.patch.object(verifier.shutil, "which", return_value=CASES["compiler"]), \
                     mock.patch.object(SDK.subprocess, "run", side_effect=command), \
                     self.assertRaises(subprocess.CalledProcessError):
                    verifier.main()
                self.assertFalse((output / "stack/harness.o").exists())
                self.assertTrue((output / "stack/inputs/llvm-macho.o").is_file())
                files = [p for p in output.rglob("*") if p.is_file()]
                counts.append((len(files), sum(p.stat().st_size for p in files)))
            self.assertEqual(len(set(counts)), 1)
            self.measurements["sp_entry"] = counts


if __name__ == "__main__":
    unittest.main()
