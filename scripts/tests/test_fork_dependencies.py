"""Exercise moving default branches, transactional snapshots and native rebuilds."""

import contextlib
from concurrent.futures import ThreadPoolExecutor, TimeoutError
import io
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import build as build_driver
import fork_dependencies as forks

SOURCE_ROOT = forks.ROOT


@unittest.skipUnless(shutil.which("git"), "Git is required")
class ForkFixture(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="neverd-fork-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.repository = self.root / "repository"
        self.repository.mkdir()
        self.git("init", "-b", "main3")
        self.git("config", "user.email", "test@example.invalid")
        self.git("config", "user.name", "Dependency test")
        self.write_revision(1)
        self.build = self.root / "build"
        self.names = json.loads(forks.REPOSITORIES.read_text())
        self.repositories = {name: self.repository.as_posix() for name in self.names}
        self.configuration = self.root / "repositories.json"
        self.configuration.write_text(json.dumps(self.repositories))
        self.workspace = self.root / "workspace"
        self.workspace.mkdir()
        self.run_git(self.workspace, "init", "-b", "dev")
        for name in self.names:
            self.run_git(self.workspace, "-c", "protocol.file.allow=always", "submodule", "add",
                         "-b", "main3", self.repository.as_posix(), "third_party/" + forks.library_name(name))
        self.run_git(self.workspace, "config", "user.email", "test@example.invalid")
        self.run_git(self.workspace, "config", "user.name", "Dependency test")
        self.run_git(self.workspace, "commit", "-m", "initial submodules")
        self.addCleanup(patch.stopall)
        patch.object(forks, "ROOT", self.workspace).start()
        patch.object(forks, "REPOSITORIES", self.configuration).start()

    @staticmethod
    def run_git(directory, *args):
        return subprocess.check_output(["git", "-C", str(directory), *args],
                                       stderr=subprocess.STDOUT, text=True).strip()

    @staticmethod
    def remove_git_directory(directory):
        # Git object files are read-only on Windows. Only change permissions
        # inside this test's disposable repository before removing it.
        for path in directory.rglob("*"):
            if path.is_file():
                path.chmod(stat.S_IREAD | stat.S_IWRITE)
        shutil.rmtree(directory)

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.repository), *args],
                                       stderr=subprocess.STDOUT, text=True).strip()

    def write_revision(self, value):
        (self.repository / "CMakeLists.txt").write_text(
            "add_executable(probe main.c" + (" extra.c" if value == 2 else "") + ")\n"
        )
        (self.repository / "main.c").write_text(
            "#include <stdio.h>\n" +
            ("int value(void);\n" if value == 2 else "int value(void) { return 1; }\n") +
            'int main(void) { printf("%d\\n", value()); return 0; }\n'
        )
        if value == 2:
            (self.repository / "extra.c").write_text("int value(void) { return 2; }\n")
        self.git("add", ".")
        self.git("commit", "-m", f"revision {value}")

    def refresh(self):
        with forks.build_lock(self.build), contextlib.redirect_stdout(io.StringIO()):
            return forks.refresh(self.build)


class ForkDependenciesTests(ForkFixture):
    def test_tracks_default_branch_instead_of_a_release(self):
        first = forks.resolve_head(str(self.repository))
        self.assertEqual(first["branch"], "main3")
        self.write_revision(2)
        second = forks.resolve_head(str(self.repository))
        self.assertNotEqual(first["commit"], second["commit"])
        self.assertEqual(second["commit"], self.git("rev-parse", "HEAD"))

    def test_refresh_is_atomic_and_keeps_prior_sources(self):
        path = self.refresh()
        before = path.read_bytes()
        old = json.loads(before)
        with forks.build_lock(self.build):
            forks.Exports(self.build).retain("frozen-baseline", old)
        self.write_revision(2)
        with patch.object(forks, "create_archive", side_effect=OSError("unavailable")):
            with self.assertRaises(OSError):
                self.refresh()
        self.assertEqual(path.read_bytes(), before)
        self.refresh()
        self.assertNotEqual(json.loads(path.read_text())["sources"], old["sources"])
        for source in old["sources"].values():
            self.assertEqual(forks.digest(source["archive"]), source["sha256"])

    def test_every_invocation_checks_remote_even_when_cached(self):
        path = self.refresh()
        with patch.object(forks, "resolve_head", side_effect=OSError("offline")):
            with self.assertRaises(OSError):
                self.refresh()
            with self.assertRaises(OSError):
                forks.check(self.build)
        with patch.object(forks, "create_archive", side_effect=AssertionError("cached")):
            self.refresh()
        old_digest = forks.digest(path)
        self.write_revision(2)
        with self.assertRaisesRegex(RuntimeError, "advanced"):
            forks.check(self.build)
        with patch.dict(os.environ, {"NEVERD_DEPENDENCY_SNAPSHOT": old_digest}):
            forks.check(self.build)  # An already-running build stays frozen.
        self.refresh()
        self.assertNotEqual(old_digest, forks.digest(path))
        with patch.dict(os.environ, {"NEVERD_DEPENDENCY_SNAPSHOT": forks.digest(path)}):
            with self.assertRaisesRegex(RuntimeError, "build graph"):
                forks.check(self.build, old_digest)

    def test_preserves_dirty_and_unpublished_submodule_work(self):
        source = self.workspace / "third_party/highway"
        path = source / "main.c"
        original = path.read_text()
        path.write_text("local work\n")
        with self.assertRaisesRegex(RuntimeError, "local submodule changes"):
            self.refresh()
        self.assertEqual(path.read_text(), "local work\n")
        path.write_text(original)
        self.run_git(source, "config", "user.email", "test@example.invalid")
        self.run_git(source, "config", "user.name", "Dependency test")
        self.run_git(source, "commit", "--allow-empty", "-m", "unpublished work")
        local_head = self.run_git(source, "rev-parse", "HEAD")
        with self.assertRaisesRegex(RuntimeError, "divergent submodule commits"):
            self.refresh()
        self.assertEqual(self.run_git(source, "rev-parse", "HEAD"), local_head)

    def test_uninitialized_old_gitlink_and_disconnected_shallow_tips(self):
        original = self.git("rev-parse", "HEAD")
        self.write_revision(2)
        latest = self.git("rev-parse", "HEAD")
        relative = "third_party/highway"
        source = self.workspace / relative
        self.run_git(self.workspace, "submodule", "deinit", "-f", "--", relative)
        self.remove_git_directory(self.workspace / ".git/modules" / relative)
        with patch.dict(os.environ, {"GIT_ALLOW_PROTOCOL": "file"}):
            self.refresh()
        self.assertEqual(self.run_git(source, "rev-parse", "HEAD"), latest)
        # Existing CI caches can contain disconnected shallow A/B roots.
        self.run_git(self.workspace, "submodule", "deinit", "-f", "--", relative)
        self.remove_git_directory(self.workspace / ".git/modules" / relative)
        shutil.rmtree(source)
        self.run_git(self.workspace, "clone", "--depth", "1", self.repository.as_uri(), relative)
        self.run_git(source, "fetch", "--depth", "1", "origin", original)
        self.run_git(source, "checkout", "--detach", original)
        self.assertEqual(self.run_git(source, "rev-parse", "--is-shallow-repository"), "true")
        self.run_git(source, "remote", "set-url", "origin", self.repository.as_posix())
        self.refresh()
        self.assertEqual(self.run_git(source, "rev-parse", "HEAD"), latest)

    def test_tampered_cache_is_rejected(self):
        snapshot = json.loads(self.refresh().read_text())
        Path(next(iter(snapshot["sources"].values()))["archive"]).write_bytes(b"changed")
        with self.assertRaisesRegex(RuntimeError, "archive changed"):
            self.refresh()

    @unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja"), "CMake/Ninja required")
    def test_new_commit_reconfigures_added_sources_and_records_success_only(self):
        self.refresh()
        source = self.root / "consumer"
        source.mkdir()
        module = (SOURCE_ROOT / "cmake/NeverDForks.cmake").as_posix()
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\nproject(Consumer C)\n"
            f'include("{module}")\n'
            'option(USE_FORK "Use test dependency" ON)\n'
            "if(USE_FORK)\n"
            "  neverd_populate_fork(neverd_highway)\n"
            '  add_subdirectory("${neverd_highway_SOURCE_DIR}" dependency)\n'
            '  install(TARGETS probe RUNTIME DESTINATION bin)\n'
            "endif()\n"
        )
        subprocess.run(["cmake", "-S", str(source), "-B", str(self.build), "-G", "Ninja",
                        "-DCMAKE_BUILD_TYPE=Release"],
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        with contextlib.redirect_stdout(io.StringIO()):
            build_driver.build(self.build, ["--target", "probe"])
        exe = self.build / "dependency" / ("probe.exe" if os.name == "nt" else "probe")
        self.assertEqual(subprocess.check_output([str(exe)], text=True).strip(), "1")
        self.write_revision(2)
        # A bare native build must not silently use yesterday's fork.
        stale = subprocess.run(["cmake", "--build", str(self.build), "--target", "probe"],
                               capture_output=True, text=True)
        self.assertNotEqual(stale.returncode, 0)
        self.assertIn("advanced", stale.stdout + stale.stderr)
        self.refresh()
        stale = subprocess.run(["cmake", "--build", str(self.build), "--target", "probe"],
                               capture_output=True, text=True)
        self.assertNotEqual(stale.returncode, 0)
        self.assertIn("build graph", stale.stdout + stale.stderr)
        with contextlib.redirect_stdout(io.StringIO()):
            build_driver.build(self.build, ["--target", "probe"])
        self.assertEqual(subprocess.check_output([str(exe)], text=True).strip(), "2")
        receipt = self.build / "neverd-build-receipt.json"
        self.assertEqual(json.loads(receipt.read_text())["status"], "success")
        forks.check_install(self.build, "Release")
        with self.assertRaisesRegex(RuntimeError, "successful build"):
            forks.check_install(self.build, "Debug")
        install = ["cmake", "--install", str(self.build), "--prefix", str(self.root / "installed")]
        subprocess.run(install, check=True, capture_output=True, text=True)
        with patch.object(forks, "resolve_head", side_effect=OSError("offline")):
            with self.assertRaises(OSError):
                build_driver.build(self.build, ["--target", "probe"])
        self.assertEqual(json.loads(receipt.read_text())["status"], "failed")
        self.assertEqual(subprocess.check_output([str(exe)], text=True).strip(), "2")
        (self.repository / "main.c").write_text("deliberate compile failure\n")
        self.git("add", ".")
        self.git("commit", "-m", "broken tip")
        with self.assertRaises(subprocess.CalledProcessError):
            build_driver.build(self.build, ["--target", "probe"])
        failed = json.loads(receipt.read_text())
        self.assertEqual(failed["status"], "failed")
        self.assertEqual(failed["snapshot"]["sources"]["neverd_highway"]["commit"],
                         self.git("rev-parse", "HEAD"))
        rejected = subprocess.run(install, check=False, capture_output=True, text=True)
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("before installing", rejected.stdout + rejected.stderr)
        subprocess.run(["cmake", "-S", str(source), "-B", str(self.build), "-DUSE_FORK=OFF"],
                       check=True, capture_output=True, text=True)
        build_driver.build(self.build, [])
        inputs = json.loads((self.build / "neverd-dependency-inputs.json").read_text())
        self.assertTrue(all(not item["used"] for item in inputs["sources"].values()))
        subprocess.run(["cmake", "-S", str(source), "-B", str(self.build), "-DUSE_FORK=ON",
                        "-DFETCHCONTENT_SOURCE_DIR_NEVERD_HIGHWAY=" + str(self.repository)],
                       check=True, capture_output=True, text=True)
        inputs = json.loads((self.build / "neverd-dependency-inputs.json").read_text())
        self.assertEqual(inputs["sources"]["neverd_highway"]["selection"], "local_override")
        self.assertTrue(inputs["sources"]["neverd_highway"]["used"])
        with self.assertRaisesRegex(RuntimeError, "local source overrides"):
            build_driver.build(self.build, [])
        self.assertEqual(json.loads(receipt.read_text())["status"], "failed")

    def test_build_lock_serializes_processes_and_releases_after_exit(self):
        code = ("import sys; from pathlib import Path; "
                f"sys.path.insert(0, {str(SOURCE_ROOT / 'scripts')!r}); "
                "import fork_dependencies as f\n"
                "print('waiting', flush=True)\n"
                "with f.build_lock(Path(sys.argv[1])):\n"
                " print('locked', flush=True)\n"
                " sys.stdin.readline()\n")
        first = subprocess.Popen([sys.executable, "-c", code, str(self.build)],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.addCleanup(lambda: first.poll() is None and first.kill())
        self.assertEqual(first.stdout.readline().strip(), "waiting")
        self.assertEqual(first.stdout.readline().strip(), "locked")
        second = subprocess.Popen([sys.executable, "-c", code, str(self.build)],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.addCleanup(lambda: second.poll() is None and second.kill())
        self.assertEqual(second.stdout.readline().strip(), "waiting")
        with ThreadPoolExecutor(max_workers=1) as reader:
            acquired = reader.submit(second.stdout.readline)
            try:
                with self.assertRaises(TimeoutError):
                    acquired.result(timeout=0.2)
            finally:
                first.communicate("release\n", timeout=10)
            self.assertEqual(acquired.result(timeout=10).strip(), "locked")
        second.communicate("release\n", timeout=10)
        with forks.build_lock(self.build):
            pass

    @unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja"), "CMake/Ninja required")
    def test_multi_config_receipt_requires_explicit_profile_and_separate_install(self):
        source = self.root / "consumer"
        source.mkdir()
        module = (SOURCE_ROOT / "cmake/NeverDForks.cmake").as_posix()
        (source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.20)\nproject(Consumer C)\n'
            f'include("{module}")\nadd_executable(probe main.c)\n')
        (source / "main.c").write_text("int main(void) { return 0; }\n")
        subprocess.run(["cmake", "-S", str(source), "-B", str(self.build), "-G", "Ninja Multi-Config",
                        "-DCMAKE_DEFAULT_BUILD_TYPE=Release", "-DCMAKE_CONFIGURATION_TYPES=Release;Debug"],
                       check=True, capture_output=True)
        with self.assertRaisesRegex(ValueError, "explicit --config"):
            build_driver.build(self.build, [])
        build_driver.build(self.build, ["--config=Release"])
        forks.check_install(self.build, "Release")
        with self.assertRaisesRegex(RuntimeError, "successful build"):
            forks.check_install(self.build, "Debug")
        with self.assertRaisesRegex(ValueError, "install is a separate step"):
            build_driver.build(self.build, ["--config", "Release", "--target", "install"])
        receipt = json.loads((self.build / "neverd-build-receipt.json").read_text())
        self.assertEqual(receipt["status"], "failed")

    @unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja"), "CMake/Ninja required")
    def test_notice_staging_before_transport_provider_configuration(self):
        (self.repository / "CMakeLists.txt").write_text("add_library(simdjson STATIC stub.c)\n")
        (self.repository / "stub.c").write_text("int placeholder(void) { return 1; }\n")
        (self.repository / "LICENSE").write_text("Test fixture notice\n")
        self.git("add", ".")
        self.git("commit", "-m", "transport provider fixture")
        self.refresh()
        source = self.root / "consumer"
        source.mkdir()
        module = (SOURCE_ROOT / "cmake/NeverDPerformance.cmake").as_posix()
        (source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.20)\nproject(Consumer C CXX)\n'
            f'include("{module}")\nadd_executable(early main.c)\n'
            'neverd_stage_performance_notices(early)\n'
            'neverd_add_simdjson()\n')
        (source / "main.c").write_text("int main(void) { return 0; }\n")
        subprocess.run(["cmake", "-S", str(source), "-B", str(self.build), "-G", "Ninja",
                        "-DCMAKE_BUILD_TYPE=Release", "-DNEVERD_ENABLE_HIGHWAY=OFF",
                        "-DNEVERD_ENABLE_SIMDUTF=OFF", "-DNEVERD_ENABLE_TBB=OFF"],
                       check=True, capture_output=True)
        build_driver.build(self.build, ["--target", "early"])
        notice = self.build / "licenses/simdjson/LICENSE"
        self.assertEqual(notice.read_text(), "Test fixture notice\n")
        self.assertIn(self.git("rev-parse", "HEAD"),
                      (notice.parent / "SOURCE.md").read_text())


if __name__ == "__main__":
    unittest.main()
