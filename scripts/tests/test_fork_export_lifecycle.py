"""Exercise export retirement through real local Git, archive and CMake entry points."""

import contextlib
import io
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time
import unittest
from unittest.mock import patch

from scripts.tests.test_fork_dependencies import ForkFixture, SOURCE_ROOT, build_driver, forks


@unittest.skipIf(os.name == "nt", "Automatic retirement needs qualified POSIX child locks")
class ForkExportLifecycleTests(ForkFixture):
    def setUp(self):
        super().setUp()
        self.build = self.build.resolve()
        self.child_logs = {}

    def advance(self, value):
        (self.repository / "revision").write_text(str(value))
        self.git("add", ".")
        self.git("commit", "-m", f"export revision {value}")

    def exports(self):
        return sorted(self.build.glob("third_party/*/*/source.tar.gz"))

    def state(self):
        return forks.Exports(self.build).state

    def consumer(self):
        self.refresh()
        self.source = self.root / "consumer"
        self.source.mkdir()
        module = (SOURCE_ROOT / "cmake/NeverDForks.cmake").as_posix()
        (self.source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\nproject(ExportConsumer C)\n"
            f'include("{module}")\n'
            "neverd_populate_fork(neverd_highway)\n"
            'add_subdirectory("${neverd_highway_SOURCE_DIR}" dependency)\n'
            'if(EXISTS "${CMAKE_SOURCE_DIR}/fail")\n'
            '  message(FATAL_ERROR "deliberate configure failure")\nendif()\n'
            'if(EXISTS "${CMAKE_SOURCE_DIR}/gate.py")\n'
            f'  execute_process(COMMAND "{sys.executable}" "${{CMAKE_SOURCE_DIR}}/gate.py"\n'
            '    COMMAND_ERROR_IS_FATAL ANY)\nendif()\n'
        )
        subprocess.run(["cmake", "-S", str(self.source), "-B", str(self.build), "-G", "Ninja",
                        "-DCMAKE_BUILD_TYPE=Release"], check=True, capture_output=True)
        self.compile()

    def compile(self):
        with contextlib.redirect_stdout(io.StringIO()):
            build_driver.build(self.build, ["--target", "probe"])

    def child(self, body):
        code = ("import sys, os, json, time\nfrom pathlib import Path\n"
                f"sys.path.insert(0, {str(SOURCE_ROOT / 'scripts')!r})\n"
                "import fork_dependencies as f\nimport build as driver\n"
                f"f.ROOT = Path({str(self.workspace)!r})\n"
                f"f.REPOSITORIES = Path({str(self.configuration)!r})\n"
                f"build = Path({str(self.build)!r})\n" + body)
        log = (self.root / f"child-{len(list(self.root.glob('child-*.log')))}.log").open("w+")
        self.addCleanup(log.close)
        process = subprocess.Popen([sys.executable, "-c", code], stdout=log,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        self.child_logs[process.pid] = log.name
        def stop():
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait(timeout=10)
        self.addCleanup(stop)
        return process

    def wait_file(self, path, process):
        deadline = time.monotonic() + 30
        while not path.exists() and time.monotonic() < deadline and process.poll() is None:
            time.sleep(0.02)
        if not path.exists():
            self.fail(f"Child did not reach {path}: exit {process.poll()}\n" +
                      Path(self.child_logs[process.pid]).read_text()[-4000:])

    @unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja"), "CMake/Ninja required")
    def test_repeated_builds_bound_exports_and_preserve_named_readers_and_caches(self):
        self.consumer()
        frozen = forks.Exports(self.build).snapshot()
        with forks.build_lock(self.build):
            forks.Exports(self.build).retain("performance-baseline", frozen)
        legacy = self.build / "third_party/highway" / ("a" * 40)
        legacy.mkdir(parents=True)
        (legacy / "caller-data").write_text("keep legacy source")
        protected = [self.build / "_deps/reusable-cache", self.root / "installed/sdk",
                     self.root / "other-build/third_party/source"]
        for path in protected:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("keep independent product")
        counts = []
        for value in range(2, 7):
            self.advance(value)
            self.compile()
            counts.append(len(self.exports()))
            self.assertEqual(len(self.state()["owned"]), 10)
            self.assertIsNone(self.state()["pending"])
            for source in frozen["sources"].values():
                self.assertEqual(forks.digest(source["archive"]), source["sha256"])
        self.assertEqual(counts, [10] * 5)
        with forks.build_lock(self.build):
            forks.Exports(self.build).release("performance-baseline")
        self.assertEqual(len(self.exports()), 5)
        self.assertEqual(len(self.state()["owned"]), 5)
        self.assertEqual((legacy / "caller-data").read_text(), "keep legacy source")
        for path in protected:
            self.assertEqual(path.read_text(), "keep independent product")

    @unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja"), "CMake/Ninja required")
    def test_failed_configuration_retains_both_graphs_without_growing_on_retry(self):
        self.consumer()
        old = self.state()["graph"]
        self.advance(2)
        failure = self.source / "fail"
        failure.touch()
        with self.assertRaises(subprocess.CalledProcessError):
            self.compile()
        pending = self.state()["pending"]
        self.assertEqual(len(self.exports()), 10)
        for value in range(3, 6):
            self.advance(value)
            with self.assertRaises(subprocess.CalledProcessError):
                self.compile()
            self.assertEqual(self.state()["pending"], pending)
            self.assertEqual(self.state()["graph"], old)
            self.assertEqual(len(self.exports()), 10)
            with self.assertRaisesRegex(RuntimeError, "unfinished CMake"):
                self.refresh()
        failure.unlink()
        self.compile()
        self.assertEqual(len(self.exports()), 5)
        self.assertIsNone(self.state()["pending"])
        self.assertTrue(all(not (self.build / path).exists() for path in old))
        self.assertEqual(forks.Exports(self.build).snapshot()["sources"]["neverd_highway"]["commit"],
                         self.git("rev-parse", "HEAD"))

    def test_failed_archive_keeps_snapshot_and_retires_partial_export(self):
        snapshot = self.refresh().read_bytes()
        self.advance(2)
        original = forks.create_archive
        def fail_after_archive(source, commit, destination):
            original(source, commit, destination)
            raise OSError("deliberate archive failure")
        with patch.object(forks, "create_archive", side_effect=fail_after_archive):
            with self.assertRaisesRegex(OSError, "deliberate archive"):
                self.refresh()
        self.assertEqual((self.build / ".neverd-deps/snapshot.json").read_bytes(), snapshot)
        self.assertEqual(len(self.exports()), 5)
        self.assertEqual(len(self.state()["owned"]), 5)
        self.assertFalse(list(self.build.glob("third_party/*/*/*.download")))

    def test_archive_interruptions_recover_on_next_actual_refresh(self):
        self.refresh()
        for value, signum in enumerate((signal.SIGINT, signal.SIGTERM, signal.SIGKILL), 2):
            with self.subTest(signal=signum):
                self.advance(value)
                snapshot = (self.build / ".neverd-deps/snapshot.json").read_bytes()
                gate = self.root / f"archive-{value}.ready"
                process = self.child(
                    "original = f.create_archive\n"
                    "def gated(source, commit, destination):\n"
                    " original(source, commit, destination)\n"
                    f" Path({str(gate)!r}).touch()\n"
                    " time.sleep(60)\n"
                    "f.create_archive = gated\n"
                    "with f.build_lock(build): f.refresh(build)\n")
                self.wait_file(gate, process)
                process.send_signal(signum)
                self.assertNotEqual(process.wait(timeout=10), 0)
                self.assertEqual((self.build / ".neverd-deps/snapshot.json").read_bytes(), snapshot)
                self.refresh()
                self.assertEqual(len(self.exports()), 5)
                self.assertEqual(len(self.state()["owned"]), 5)
                self.assertFalse(list(self.build.glob("third_party/*/*/*.download")))

    @unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja"), "CMake/Ninja required")
    def test_cmake_child_keeps_lock_after_wrapper_interruptions(self):
        self.consumer()
        for value, signum in enumerate((signal.SIGINT, signal.SIGTERM, signal.SIGKILL), 2):
            with self.subTest(signal=signum):
                self.advance(value)
                ready = self.root / f"cmake-{value}.ready"
                release = self.root / f"cmake-{value}.release"
                gate = self.source / "gate.py"
                gate.write_text(
                    "import os, time\nfrom pathlib import Path\n"
                    f"Path({str(ready)!r}).write_text(str(os.getppid()))\n"
                    f"while not Path({str(release)!r}).exists(): time.sleep(0.02)\n")
                process = self.child("driver.build(build, ['--target', 'probe'])\n")
                self.wait_file(ready, process)
                cmake_pid = int(ready.read_text())
                process.send_signal(signum)
                self.assertNotEqual(process.wait(timeout=10), 0)
                os.kill(cmake_pid, 0)
                waiting = self.root / f"lock-{value}.waiting"
                acquired = self.root / f"lock-{value}.acquired"
                contender = self.child(
                    f"Path({str(waiting)!r}).touch()\n"
                    "with f.build_lock(build):\n"
                    f" Path({str(acquired)!r}).touch()\n")
                self.wait_file(waiting, contender)
                time.sleep(0.3)
                self.assertFalse(acquired.exists(), "CMake reader lost its inherited build lock")
                self.assertEqual(len(self.exports()), 10)
                release.touch()
                self.assertEqual(contender.wait(timeout=30), 0)
                gate.unlink()
                self.compile()
                self.assertEqual(len(self.exports()), 5)
                self.assertIsNone(self.state()["pending"])

    def test_interrupted_marker_creation_and_retirement_are_recoverable(self):
        self.refresh()
        for phase in ("creating", "retiring"):
            with self.subTest(phase=phase):
                key = "third_party/highway/" + ("b" if phase == "creating" else "c") * 40
                path = self.build / key
                gate = self.root / f"{phase}.ready"
                body = (
                    "with f.build_lock(build):\n"
                    " exports = f.Exports(build)\n"
                    " original = exports.save\n"
                    " def gated_save():\n"
                    "  original()\n"
                    f"  if exports.state['owned'].get({key!r}, {{}}).get('phase') == {phase!r}:\n"
                    f"   Path({str(gate)!r}).touch()\n"
                    "   time.sleep(60)\n"
                    " exports.save = gated_save\n"
                )
                if phase == "creating":
                    body += f" exports.create(Path({str(path)!r}))\n"
                else:
                    body += (f" exports.create(Path({str(path)!r}))\n"
                             f" (Path({str(path)!r}) / 'source.tar.gz').write_bytes(b'owned')\n"
                             " exports.collect()\n")
                process = self.child(body)
                self.wait_file(gate, process)
                process.kill()
                process.wait(timeout=10)
                self.refresh()
                self.assertFalse(path.exists())
                self.assertNotIn(key, self.state()["owned"])
                self.assertEqual(len(self.exports()), 5)

    def test_redirected_or_replaced_ownership_is_preserved_and_rejected(self):
        self.refresh()
        with forks.build_lock(self.build):
            exports = forks.Exports(self.build)
            path = self.build / "third_party/highway" / ("d" * 40)
            exports.create(path)
            marker = path / ".neverd-export.json"
            marker.write_text('{"token":"caller replacement"}')
            with self.assertRaisesRegex(ValueError, "ownership changed"):
                exports.collect()
            self.assertTrue(marker.exists())
            external = self.root / "external"
            external.mkdir()
            external.joinpath("source").write_text("caller source")
            redirected = self.build / "third_party/highway" / ("e" * 40)
            redirected.symlink_to(external, target_is_directory=True)
            with self.assertRaisesRegex(ValueError, "redirected"):
                exports.create(redirected)
            self.assertEqual(external.joinpath("source").read_text(), "caller source")

    def test_kill_after_marker_publication_and_removal_recovers_without_guessing(self):
        self.refresh()
        for creating in (True, False):
            with self.subTest(creating=creating):
                key = "third_party/highway/" + ("b" if creating else "c") * 40
                path = self.build / key
                marker = path / ".neverd-export.json"
                ready = self.root / f"marker-{creating}.ready"
                body = (f"marker = Path({str(marker)!r})\n"
                        "def pause():\n"
                        f" Path({str(ready)!r}).touch()\n"
                        " time.sleep(60)\n")
                if creating:
                    body += ("import fork_exports\n"
                             "original = fork_exports.write_json\n"
                             "def gated(path, value):\n"
                             " original(path, value)\n"
                             " if path == marker: pause()\n"
                             "fork_exports.write_json = gated\n")
                else:
                    body += ("original = Path.unlink\n"
                             "def gated(path, *args, **kwargs):\n"
                             " original(path, *args, **kwargs)\n"
                             " if path == marker: pause()\n"
                             "Path.unlink = gated\n")
                body += ("with f.build_lock(build):\n"
                         " exports = f.Exports(build)\n"
                         f" exports.create(Path({str(path)!r}))\n"
                         f" (Path({str(path)!r}) / 'source.tar.gz').write_bytes(b'owned')\n"
                         " exports.collect()\n")
                process = self.child(body)
                self.wait_file(ready, process)
                process.kill()
                process.wait(timeout=10)
                self.assertEqual(marker.exists(), creating)
                self.refresh()
                self.assertFalse(path.exists())
                self.assertNotIn(key, self.state()["owned"])


if __name__ == "__main__":
    unittest.main()
