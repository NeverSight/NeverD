"""Small real-process checks of MIPS producers' cleanup and recovery contracts."""

import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from mips_validation_artifacts import MIPSValidationArtifacts, digest


def inventory(directory):
    files = [p for p in directory.rglob("*") if p.is_file()]
    return {"files": len(files), "bytes": sum(p.stat().st_size for p in files),
            "directories": sum(p.is_dir() for p in directory.rglob("*"))}


class MIPSArtifactsTest(unittest.TestCase):
    measurements = {}

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="neverd-mips-lifecycle-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.output = self.root / "output"
        self.input = self.root / "current-sdk"
        self.input.write_bytes(b"current SDK and pending baseline\n")
        self.ctest = self.root / "LastTest.log"
        self.ctest.write_text("active CTest diagnostic\n")
        self.before = (digest(self.input), digest(self.ctest))

    def tearDown(self):
        self.assertEqual(self.before, (digest(self.input), digest(self.ctest)))

    @classmethod
    def tearDownClass(cls):
        if path := os.environ.get("MIPS_LIFECYCLE_TEST_RECEIPT"):
            Path(path).write_text(json.dumps(cls.measurements, indent=2) + "\n")

    def test_repeated_success_and_failure_are_bounded(self):
        sizes = []
        for index in range(7):
            try:
                with MIPSValidationArtifacts(self.output, "fixture", (self.input,)) as run:
                    source = run.work / "kernel.c"
                    source.write_text("unsigned kernel(void) { return 7; }\n")
                    (run.work / "partial.o").write_bytes(b"x" * 4096)
                    with run.process([sys.executable, "-c", "print('diagnostic'); raise SystemExit(7)"],
                                     run.work / "command.log") as (process, _):
                        process.wait(timeout=10)
                    run.keep_source(source)
                    raise RuntimeError("expected fixture failure")
            except RuntimeError as error:
                self.assertEqual(str(error), "expected fixture failure")
            self.assertFalse((self.output / "work").exists())
            commands = json.loads((self.output / "commands.json").read_text())
            self.assertEqual(commands[0]["exit"], 7)
            self.assertIn("diagnostic", commands[0]["logs"][0]["tail"])
            evidence = json.loads((self.output / "evidence/index.json").read_text())[0]
            self.assertEqual(digest(self.output / "evidence" / evidence["file"]), evidence["sha256"])
            sizes.append(inventory(self.output))
        self.assertTrue(all(value == sizes[1] for value in sizes[1:]), sizes)
        self.measurements["seven_failed_runs"] = sizes
        for _ in range(3):
            with MIPSValidationArtifacts(self.output, "fixture", (self.input,)) as run:
                (run.work / "unused.elf").write_bytes(b"generated")
            self.assertFalse((self.output / "work").exists())
        self.measurements["after_success"] = inventory(self.output)

    def test_unowned_and_input_directories_are_never_reclaimed(self):
        self.output.mkdir()
        source = self.output / "user.cpp"
        source.write_text("uncommitted source\n")
        with self.assertRaisesRegex(ValueError, "unowned nonempty"):
            with MIPSValidationArtifacts(self.output, "fixture"):
                pass
        self.assertEqual(source.read_text(), "uncommitted source\n")
        with self.assertRaisesRegex(ValueError, "inputs and baselines"):
            MIPSValidationArtifacts(self.output, "fixture", (source,))

    def start_waiting_producer(self):
        ready = self.root / "ready"
        child = "import os,time; from pathlib import Path; print('interrupted diagnostic', flush=True); Path(" + repr(str(ready)) + ").write_text(str(os.getpid())); time.sleep(60)"
        parent = """import sys
from pathlib import Path
sys.path.insert(0, sys.argv[1])
from mips_validation_artifacts import MIPSValidationArtifacts
with MIPSValidationArtifacts(Path(sys.argv[2]), 'fixture') as run:
    (run.work / 'partial.o').write_bytes(b'partial build')
    (run.work / 'kernel.c').write_text('unsigned kernel(void) { return 7; }')
    with run.process([sys.executable, '-c', sys.argv[3]], run.work / 'child.log') as (process, _):
        process.wait()
"""
        process = subprocess.Popen([sys.executable, "-c", parent, str(ROOT / "scripts"),
                                    str(self.output), child], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        deadline = time.monotonic() + 10
        while not ready.exists() and time.monotonic() < deadline:
            if process.poll() is not None:
                self.fail(process.communicate()[1].decode())
            time.sleep(0.01)
        self.assertTrue(ready.exists())
        return process, int(ready.read_text())

    def test_term_finishes_cleanup_and_records_diagnostic(self):
        process, child = self.start_waiting_producer()
        process.terminate()
        process.communicate(timeout=10)
        self.assertFalse((self.output / "work").exists())
        result = json.loads((self.output / "lifecycle.json").read_text())
        self.assertEqual(result["status"], "failed")
        self.assertEqual(result["commands"], 1)
        evidence = json.loads((self.output / "evidence/index.json").read_text())[0]
        self.assertEqual(digest(self.output / "evidence" / evidence["file"]), evidence["sha256"])
        with self.assertRaises(ProcessLookupError):
            os.kill(child, 0)
        self.measurements["sigterm"] = result

    def test_kill_recovers_next_start_but_preserves_surviving_reader(self):
        process, child = self.start_waiting_producer()
        try:
            process.kill()
            process.communicate(timeout=10)
            self.assertTrue((self.output / "work/partial.o").exists())
            with self.assertRaises(BlockingIOError):
                with MIPSValidationArtifacts(self.output, "fixture"):
                    pass
            self.assertTrue((self.output / "work/partial.o").exists())
        finally:
            os.killpg(child, signal.SIGKILL)
        deadline = time.monotonic() + 10
        while True:
            try:
                with MIPSValidationArtifacts(self.output, "fixture") as run:
                    self.assertGreater(run.recovered_bytes, 0)
                break
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.01)
        self.assertFalse((self.output / "work").exists())
        recovered = json.loads((self.output / "recovered.json").read_text())
        self.assertEqual(len(recovered["commands"]), 1)
        self.assertIn("interrupted diagnostic", recovered["logs"][0]["tail"])
        self.assertTrue((self.output / "evidence/index.json").exists())
        self.measurements["sigkill_recovery"] = json.loads((self.output / "lifecycle.json").read_text())

    def test_exited_leader_cannot_retire_a_live_grandchilds_scratch(self):
        ready = self.root / "grandchild"
        child = ("import os,signal,time; from pathlib import Path; "
                 "signal.signal(signal.SIGTERM, signal.SIG_IGN); "
                 "Path(" + repr(str(ready)) + ").write_text(str(os.getpid())); time.sleep(60)")
        leader = ("import subprocess,sys,time; from pathlib import Path; "
                  "subprocess.Popen([sys.executable,'-c',sys.argv[1]], close_fds=True); "
                  "p=Path(sys.argv[2]); "
                  "\nwhile not p.exists(): time.sleep(.01)\n")
        try:
            with self.assertRaisesRegex(RuntimeError, "live descendants"):
                with MIPSValidationArtifacts(self.output, "fixture") as run:
                    (run.work / "kernel.c").write_text("unsigned kernel(void) { return 7; }")
                    try:
                        with run.process([sys.executable, "-c", leader, child, str(ready)],
                                         run.work / "command.log") as (process, _):
                            process.wait(timeout=10)
                    finally:
                        # The roundtrip compare() finally takes this path.
                        # It must not outrun the common reader/deferred state.
                        run.keep_source(run.work / "kernel.c")
                        run.discard(run.work / "kernel.c", run.work / "command.log")
            self.assertTrue((self.output / "work/kernel.c").exists())
            self.assertEqual(json.loads((self.output / "lifecycle.json").read_text())["status"],
                             "deferred-process-readers")
            with self.assertRaises(BlockingIOError):
                with MIPSValidationArtifacts(self.output, "fixture"):
                    pass
        finally:
            if ready.exists():
                os.kill(int(ready.read_text()), signal.SIGKILL)
        deadline = time.monotonic() + 10
        while True:
            try:
                with MIPSValidationArtifacts(self.output, "fixture") as run:
                    self.assertGreater(run.recovered_bytes, 0)
                break
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(.01)
        self.assertFalse((self.output / "work").exists())
        self.assertTrue((self.output / "evidence/index.json").exists())
        self.measurements["live_grandchild_recovery"] = json.loads((self.output / "lifecycle.json").read_text())

    def test_unknown_startup_owner_is_preserved_for_audit(self):
        with MIPSValidationArtifacts(self.output, "fixture"):
            pass
        scratch = self.output / "work"
        scratch.mkdir()
        (scratch / "partial.o").write_bytes(b"reader ownership not established")
        (self.output / "pending.json").write_text(json.dumps([{"group": None, "argv": ["fixture"]}]))
        with self.assertRaisesRegex(RuntimeError, "reader audit"):
            with MIPSValidationArtifacts(self.output, "fixture"):
                pass
        self.assertEqual((scratch / "partial.o").read_bytes(), b"reader ownership not established")

    def test_actual_paired_driver_reuses_one_directory(self):
        executable = """#!/usr/bin/env python3
import json,sys
from pathlib import Path
if '-o' in sys.argv:
    Path(sys.argv[sys.argv.index('-o')+1]).write_text('unsigned test(void) { return 7; }\\n')
else:
    print(json.dumps({'fingerprint': 97, 'cpu_seconds': .01, 'wall_seconds': .01}))
"""
        for variant in ("a", "b"):
            directory = self.root / variant
            directory.mkdir()
            for name in ("neverd", "neverd-mips-bench"):
                path = directory / name
                path.write_text(executable)
                path.chmod(0o700)
        metadata = self.root / "toolchain.json"
        metadata.write_text(json.dumps({name: "fixture" for name in ("compiler", "llvm", "capstone", "build")}))
        code = self.root / "code"
        code.mkdir()
        command = [sys.executable, str(ROOT / "tools/neverd-bench/mips-paired.py"),
                   "--baseline", str(self.root / "a"), "--candidate", str(self.root / "b"),
                   "--code", str(code), "--firmware", str(self.input), "--output", str(self.output),
                   "--baseline-inputs", str(metadata), "--candidate-inputs", str(metadata),
                   "--baseline-toolchain", str(metadata), "--candidate-toolchain", str(metadata),
                   "--base-commit", "fixture", "--samples", "1", "--repeats", "1"]
        sizes = []
        for _ in range(3):
            result = subprocess.run(command, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            summary = json.loads((self.output / "summary.json").read_text())
            self.assertEqual(
                {row["job"].split("-")[0] for row in summary["summaries"]
                 if not row["job"].startswith("ubus-")},
                {"mips", "mipsel", "mips64", "mips64el"},
            )
            self.assertFalse((self.output / "work").exists())
            sizes.append(inventory(self.output))
        self.assertTrue(all(s["files"] == sizes[0]["files"] for s in sizes), sizes)
        # Timing text varies in length; retained count and bytes remain bounded.
        self.assertLess(max(s["bytes"] for s in sizes) - min(s["bytes"] for s in sizes), 4096)
        self.measurements["actual_paired_driver_three_runs"] = sizes

    def test_actual_roundtrip_driver_cleans_partial_compiler_failure(self):
        library = ROOT / "build-mips/roundtrip/libunicorn.2.dylib"
        if not library.exists():
            self.skipTest("requires the local pinned MIPS Unicorn reference")
        compiler = self.root / "compiler"
        compiler.write_text("""#!/usr/bin/env python3
import sys
from pathlib import Path
if '-print-resource-dir' in sys.argv:
    print('/fixture')
else:
    Path(sys.argv[sys.argv.index('-o')+1]).write_bytes(b'partial object')
    print('expected compiler fixture error')
    raise SystemExit(7)
""")
        compiler.chmod(0o700)
        command = [sys.executable, str(ROOT / "unittests/lift/mips/roundtrip.py"),
                   "--neverd", str(self.input), "--clang", str(compiler), "--ld", str(compiler),
                   "--output", str(self.output), "--function", "mips_add"]
        env = dict(os.environ, LIBUNICORN_PATH=str(library.parent),
                   PYTHONPATH=str(ROOT / "third_party/unicorn/bindings/python"))
        sizes = []
        for _ in range(3):
            result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=20)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("expected compiler fixture error", result.stderr)
            self.assertFalse((self.output / "work").exists())
            self.assertFalse(json.loads((self.output / "results.json").read_text())["complete"])
            self.assertEqual(json.loads((self.output / "commands.json").read_text())[0]["exit"], 7)
            sizes.append(inventory(self.output))
        self.assertTrue(all(value == sizes[0] for value in sizes), sizes)
        self.measurements["actual_roundtrip_driver_three_compiler_failures"] = sizes


if __name__ == "__main__":
    unittest.main()
