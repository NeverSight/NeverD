"""CI scheduling must retain every profile and verify the reused native runtime."""
from __future__ import annotations

from collections import Counter
import io
import hashlib
import json
import os
import re
import signal
import socket
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

from scripts.ci import mobile_android_shards as shards
from scripts.ci import mobile_linux_bundle as bundle


class MobileProfileShardsTests(unittest.TestCase):
    def setUp(self):
        self.env = dict(os.environ, NEVERD_JAVA21_HOME="/jdk21", NEVERD_JAVA25_HOME="/jdk25",
                        NEVERD_KOTLIN_JAVA_HOME="/jdk21", RUNNER_TEMP="/runner-temp",
                        NEVERD_D8="/android/d8", ANDROID_HOME="/android", GITHUB_WORKSPACE="/workspace")

    def test_catalog_has_exactly_one_shard_per_complete_profile(self):
        profiles = shards.profiles(self.env)
        partitions = [shards.shard_profiles(profiles, i, 32) for i in range(32)]
        self.assertEqual(Counter(p["id"] for p in profiles),
                         Counter(p["id"] for group in partitions for p in group))
        self.assertLessEqual(max(map(len, partitions)) - min(map(len, partitions)), 1)
        self.assertGreaterEqual(len(profiles), 863)
        self.assertEqual(len(profiles), len({p["id"] for p in profiles}))
        by_id = {p["id"]: p for p in profiles}
        for name in ("android-anonymous-covariant-bridges", "android-anonymous-covariant-bridges-sdk",
                     "android-captured-constructor-reads", "android-captured-constructor-reads-sdk",
                     "android-instance-intersection-invocations", "android-instance-intersection-invocations-sdk",
                     "android-implicit-static-defaults", "android-implicit-static-defaults-sdk",
                     "android-anonymous-member-allocations", "android-anonymous-member-allocations-sdk",
                     "android-anonymous-member-allocations-conditional", "android-anonymous-member-allocations-conditional-sdk",
                     "android-seeded-nominal-results", "android-seeded-nominal-results-sdk",
                     "android-conditional-scalar-conversions", "android-conditional-scalar-conversions-sdk",
                     "android-allocation-protected-super-calls", "android-allocation-protected-super-calls-sdk",
                     "android-allocation-scalar-unary", "android-allocation-scalar-unary-sdk",
                     "android-allocation-floating", "android-allocation-floating-sdk",
                     "android-allocation-array-payload", "android-allocation-array-payload-sdk",
                     "android-primitive-constructor-overloads", "android-primitive-constructor-overloads-sdk",
                     "android-primitive-constructor-byte-view", "android-primitive-constructor-byte-view-sdk",
                     "android-scoped-raw-constructors", "android-scoped-raw-constructors-sdk",
                     "android-unseeded-scalar-carrier", "android-unseeded-scalar-carrier-sdk",
                     "android-unseeded-scalar-carrier-package", "android-unseeded-scalar-carrier-package-sdk",
                     "android-generic-array-transfers", "android-generic-array-transfers-sdk",
                     "android-package-array-transfers", "android-package-array-transfers-sdk",
                     "android-allocation-array-reads", "android-allocation-array-reads-sdk",
                     "android-captured-static-super", "android-constructor-prologue"):
            self.assertEqual(by_id[name]["jdk"], 25)
        self.assertEqual(by_id["android-protected-final-suffix"]["jdk"], 21)
        for name in ("android-concrete-virtual-names", "android-concrete-virtual-names-sdk"):
            self.assertEqual(by_id[name]["jdk"], 21)
        for name in ("android-private-static-names", "android-private-static-names-sdk",
                     "android-private-static-names-ordinary", "android-private-static-names-ordinary-sdk",
                     "android-final-instance-names-implicit", "android-final-instance-names-implicit-sdk",
                     "android-final-instance-names-generic", "android-final-instance-names-generic-sdk",
                     "android-final-instance-names-implicit-generic", "android-final-instance-names-implicit-generic-sdk"):
            self.assertEqual(by_id[name]["jdk"], 21)
        anonymous_allocations = [p for p in profiles
                                 if p["argv"][0] == "scripts/test_mobile_android_anonymous_member_allocations.py"]
        self.assertEqual(Counter(("--conditional" in p["argv"], "--dependency-provider" in p["argv"])
                                 for p in anonymous_allocations),
                         Counter({(False, False): 1, (False, True): 1,
                                  (True, False): 1, (True, True): 1}))
        for profile in profiles:
            if "--java-home" in profile["argv"]:
                actual = profile["argv"][profile["argv"].index("--java-home") + 1]
                self.assertEqual(actual, f"/jdk{profile['jdk']}")
            if "--kotlin-java-home" in profile["argv"]:
                actual = profile["argv"][profile["argv"].index("--kotlin-java-home") + 1]
                self.assertEqual(actual, "/jdk21")

    def test_workflow_schedules_every_shard_and_uploads_every_profile(self):
        workflow = (shards.ROOT / ".github/workflows/mobile.yml").read_text()
        matrix = re.search(r"^        shard: (\[.*\])$", workflow, re.MULTILINE)
        count = re.search(r"--shard-count (\d+)", workflow)
        self.assertIsNotNone(matrix)
        self.assertIsNotNone(count)
        self.assertEqual(json.loads(matrix.group(1)), list(range(int(count.group(1)))))
        self.assertIn("${{ runner.temp }}/android-*/", workflow)
        for profile in shards.profiles(self.env):
            self.assertTrue(profile["id"].startswith("android-"))

    def test_pre_sharding_argv_and_jdk_inventory_remains_complete(self):
        # Independently expanded from each former workflow step in its own shell.
        # Append new profiles after this historical prefix; do not replace old coverage.
        original = [{"jdk": p["jdk"], "argv": p["argv"]}
                    for p in shards.profiles(self.env)[:811]]
        digest = hashlib.sha256(json.dumps(original, sort_keys=True).encode()).hexdigest()
        self.assertEqual(digest, "d65932491683ff084cfc0323a8cf409a9950bc88608a3f4589a90ccea5f4d2e1")
        self.assertEqual(Counter(p["jdk"] for p in original), {21: 619, 25: 192})

    def test_catalog_rejects_duplicate_output_and_missing_toolchain(self):
        with tempfile.TemporaryDirectory() as temporary:
            catalog = Path(temporary) / "catalog.sh"
            call = ('python scripts/test_mobile_android_bridges.py '
                    '--work-dir "$RUNNER_TEMP/android-duplicate"\n')
            catalog.write_text('profile_group test 21 60\n' + call + call)
            with self.assertRaisesRegex(ValueError, "duplicate"):
                shards.profiles(self.env, catalog)
            env = dict(self.env)
            del env["NEVERD_JAVA25_HOME"]
            with self.assertRaisesRegex(ValueError, "environment"):
                shards.profiles(env, catalog)

    def test_catalog_rejects_nonisolated_output_and_unknown_jdk(self):
        with tempfile.TemporaryDirectory() as temporary:
            catalog = Path(temporary) / "catalog.sh"
            for body, message in (
                ('profile_group test 25 60\npython scripts/test_mobile_android_bridges.py --work-dir /elsewhere', 'non-isolated'),
                ('profile_group test 24 60\npython scripts/test_mobile_android_bridges.py --work-dir "$RUNNER_TEMP/a"', 'JDK'),
            ):
                catalog.write_text(body + '\n')
                with self.assertRaisesRegex(ValueError, message):
                    shards.profiles(self.env, catalog)

    def test_shard_bounds_fail_instead_of_silently_dropping_coverage(self):
        for index, count in ((-1, 2), (2, 2), (0, 0), (0, 65), (0, 3)):
            with self.assertRaises(ValueError):
                shards.shard_profiles([{}, {}], index, count)

    def test_profile_uses_recorded_jdk_and_retains_nonzero_exit(self):
        profile = {"jdk": 21, "argv": ["fixture.py"], "timeout_seconds": 7}
        with tempfile.TemporaryDirectory() as temporary, patch.object(shards.subprocess, "Popen") as popen:
            popen.return_value.wait.return_value = 3
            result = shards.run_profile(profile, self.env, Path(temporary) / "result.log")
            self.assertEqual(result["returncode"], 3)
            call_env = popen.call_args.kwargs["env"]
            self.assertEqual(call_env["JAVA_HOME"], "/jdk21")
            self.assertTrue(call_env["PATH"].startswith("/jdk21/bin" + os.pathsep))
            self.assertEqual(call_env["NEVERD_KOTLIN_JAVA_HOME"], "/jdk21")
            popen.return_value.wait.assert_called_once_with(timeout=7)

    def test_timeout_reaps_process_group_and_reports_failure(self):
        profile = {"jdk": 25, "argv": ["fixture.py"], "timeout_seconds": 1}
        with tempfile.TemporaryDirectory() as temporary, patch.object(shards.subprocess, "Popen") as popen, \
                patch.object(shards.os, "killpg") as kill, patch.object(shards.time, "sleep"):
            popen.return_value.pid = 123
            popen.return_value.wait.side_effect = [subprocess.TimeoutExpired("fixture", 1),
                                                  subprocess.TimeoutExpired("fixture", 5), 0]
            result = shards.run_profile(profile, self.env, Path(temporary) / "result.log")
            self.assertEqual(result["returncode"], 124)
            self.assertTrue(result["timed_out"])
            self.assertEqual([call.args[1] for call in kill.call_args_list],
                             [signal.SIGTERM, 0, signal.SIGKILL])
            self.assertEqual(popen.return_value.wait.call_count, 3)

    def test_timeout_exit_race_still_reaps_and_reports_failure(self):
        profile = {"jdk": 25, "argv": ["fixture.py"], "timeout_seconds": 1}
        with tempfile.TemporaryDirectory() as temporary, patch.object(shards.subprocess, "Popen") as popen, \
                patch.object(shards.os, "killpg", side_effect=ProcessLookupError):
            popen.return_value.pid = 123
            popen.return_value.wait.side_effect = [subprocess.TimeoutExpired("fixture", 1), 0, 0]
            result = shards.run_profile(profile, self.env, Path(temporary) / "result.log")
            self.assertEqual(result["returncode"], 124)
            self.assertTrue(result["timed_out"])
            self.assertEqual(popen.return_value.wait.call_count, 3)

    @unittest.skipUnless(hasattr(os, "killpg") and hasattr(socket, "AF_UNIX"), "POSIX process groups")
    def test_timeout_stops_term_ignoring_child_after_parent_exits(self):
        child_code = (
            "import signal,socket,sys,time; "
            "signal.signal(signal.SIGTERM,signal.SIG_IGN); "
            "s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM); "
            "s.connect(sys.argv[1]); s.sendall(b'ready'); time.sleep(60)"
        )
        with tempfile.TemporaryDirectory(prefix="nd-ci-") as temporary:
            work = Path(temporary)
            endpoint = work / "s"
            parent_pid_file = work / "parent.pid"
            parent = work / "parent.py"
            parent.write_text(
                "import os,subprocess,sys,time\n"
                "from pathlib import Path\n"
                f"Path({str(parent_pid_file)!r}).write_text(str(os.getpid()))\n"
                f"subprocess.Popen([sys.executable, '-c', {child_code!r}, {str(endpoint)!r}])\n"
                "time.sleep(60)\n"
            )
            profile = {"jdk": 21, "argv": [str(parent)], "timeout_seconds": 2}
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
                listener.bind(str(endpoint))
                listener.listen(1)
                listener.settimeout(2)
                try:
                    result = shards.run_profile(profile, self.env, work / "parent.log")
                    self.assertEqual(result["returncode"], 124)
                    self.assertTrue(result["timed_out"])
                    with self.assertRaises(ProcessLookupError):
                        os.kill(int(parent_pid_file.read_text()), 0)
                    connection, _ = listener.accept()
                    with connection:
                        connection.settimeout(2)
                        self.assertEqual(connection.recv(5), b"ready")
                        # The child holds this socket open for its entire lifetime.
                        # EOF proves the surviving child is gone before the next profile.
                        self.assertEqual(connection.recv(1), b"")
                    next_profile = {"jdk": 25, "argv": ["-c", "print('next profile')"],
                                    "timeout_seconds": 2}
                    self.assertEqual(shards.run_profile(next_profile, self.env, work / "next.log")["returncode"], 0)
                finally:
                    if parent_pid_file.exists():
                        try:
                            os.killpg(int(parent_pid_file.read_text()), signal.SIGKILL)
                        except ProcessLookupError:
                            pass

    def test_failed_profile_does_not_suppress_later_evidence(self):
        profiles = [{"id": "first", "jdk": 21}, {"id": "second", "jdk": 25}]
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "evidence"
            def run(profile, env, log):
                log.write_text("owned fixture result")
                return {**profile, "returncode": int(profile["id"] == "first"), "log": str(log)}
            with patch.object(shards, "profiles", return_value=profiles), \
                    patch.object(shards, "run_profile", side_effect=run) as execute, \
                    patch.object(sys, "argv", ["shards", "--shard", "0", "--shard-count", "1", "--output", str(output)]), \
                    patch("sys.stdout", new_callable=io.StringIO):
                self.assertEqual(shards.main(), 1)
            self.assertEqual(execute.call_count, 2)
            self.assertEqual(json.loads((output / "summary.json").read_text()),
                             {"planned": 2, "completed": 2, "failed": 1})
            self.assertEqual(len((output / "results.jsonl").read_text().splitlines()), 2)


class MobileLinuxBundleTests(unittest.TestCase):
    def make_bundle(self, directory: Path):
        for relative, contents in (("bin/neverd", "cli"), ("lib/libneverd.so", "shared"),
                                   ("licenses/LLVM-LICENSE.TXT", "license")):
            path = directory / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents)
        manifest = {"schema": 1, "commit": "owned-commit",
                    "files": {p.relative_to(directory).as_posix(): bundle.digest(p)
                              for p in directory.rglob("*") if p.is_file()}}
        (directory / "manifest.json").write_text(json.dumps(manifest))

    def test_verify_requires_hashes_commit_and_closed_runtime_dependencies(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.make_bundle(directory)
            with patch.object(bundle, "dependencies", return_value=[directory / "lib/libneverd.so"]):
                env = bundle.verify(directory, "owned-commit")
                self.assertEqual(env["LD_LIBRARY_PATH"], str(directory.resolve() / "lib"))
                with self.assertRaisesRegex(ValueError, "commit"):
                    bundle.verify(directory, "other-commit")
                (directory / "bin/neverd").write_text("changed")
                with self.assertRaisesRegex(ValueError, "hash mismatch"):
                    bundle.verify(directory, "owned-commit")

    def test_verify_rejects_unbundled_dependencies_and_added_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.make_bundle(directory)
            with patch.object(bundle, "dependencies", return_value=[directory / "outside.so"]):
                with self.assertRaisesRegex(ValueError, "escapes"):
                    bundle.verify(directory, "owned-commit")
            (directory / "extra").write_text("unexpected")
            with self.assertRaisesRegex(ValueError, "unexpected"):
                bundle.verify(directory, "owned-commit")

    def test_ldd_rejects_unresolved_and_unrecognized_dependency_lines(self):
        for output, message in (("libneverd.so => not found\n", "unresolved"),
                                ("unknown output\n", "unrecognized"),
                                ("linux-vdso.so.1 (0x1)\n", "no dynamic")):
            with patch.object(bundle.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, output, "")):
                with self.assertRaisesRegex(ValueError, message):
                    bundle.dependencies(Path("fake-neverd"), {})

    def test_archive_restore_rejects_links_and_parent_traversal(self):
        for link in (True, False):
            with self.subTest(link=link), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                archive_path = root / "runtime.tar.gz"
                with tarfile.open(archive_path, "w:gz") as archive:
                    member = tarfile.TarInfo("escape" if link else "../escape")
                    if link:
                        member.type = tarfile.SYMTYPE
                        member.linkname = "/outside"
                    archive.addfile(member)
                with self.assertRaises((ValueError, tarfile.FilterError)):
                    bundle.restore(archive_path, root / "restored", "owned-commit")
                self.assertFalse((root / "escape").exists())

    def test_pack_and_restore_preserve_executable_and_library_names(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / "producer"
            (build / "bin/licenses").mkdir(parents=True)
            binary = build / "bin/neverd"
            binary.write_text("owned cli")
            binary.chmod(0o755)
            (build / "bin/licenses/LLVM-LICENSE.TXT").write_text("owned notice")
            dependency = build / "libneverd.so"
            dependency.write_text("owned library")
            def deps(path, env):
                if path == binary:
                    return [dependency]
                return [Path(env["LD_LIBRARY_PATH"]) / "libneverd.so"]
            archive = root / "runtime.tar.gz"
            with patch.object(bundle, "dependencies", side_effect=deps):
                bundle.pack(binary, archive, "owned-commit")
                bundle.restore(archive, root / "consumer", "owned-commit")
            restored = root / "consumer/bin/neverd"
            self.assertEqual(restored.read_bytes(), binary.read_bytes())
            self.assertEqual(restored.stat().st_mode & 0o777, 0o755)
            self.assertEqual((root / "consumer/lib/libneverd.so").read_bytes(), dependency.read_bytes())


if __name__ == "__main__":
    unittest.main()
