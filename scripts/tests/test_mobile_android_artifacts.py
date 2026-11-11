"""Exercise Android artifact ownership using small actual child processes."""
from __future__ import annotations

import gzip
import io
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

from scripts import mobile_android_artifacts as artifacts


@unittest.skipUnless(os.name == "posix", "POSIX Android verification drivers")
class AndroidArtifactLifecycleTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="nd-artifacts-")
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.root, self.scratch = self.base / "state", self.base / "scratch"
        self.store = artifacts.ArtifactStore(self.root, self.scratch)
        self.sdk = self.base / "original-sdk.jar"
        self.sdk.write_bytes(b"external immutable SDK\n")
        self.sdk_hash = artifacts.digest(self.sdk)

    def tearDown(self):
        self.assertEqual(artifacts.digest(self.sdk), self.sdk_hash)

    def fixture(self, code=0):
        return [sys.executable, "-c",
                "from pathlib import Path; import sys; "
                "p=Path(sys.argv[1]); p.mkdir(); "
                "[(p/n).write_bytes(b'fixture'*512) for n in ('A.class','classes.dex','provider.jar')]; "
                f"print('owned fixture diagnostic {code}'); sys.exit({code})", "{work}/output"]

    def state(self, slot="owned-source"):
        return artifacts.read_json(self.root / "jobs" / (slot + ".json"))

    def evidence(self, slot="owned-source"):
        return artifacts.read_json(self.root / "evidence" / (slot + ".json"))

    def wait_file(self, path, process):
        deadline = time.monotonic() + 5
        while not path.exists() and time.monotonic() < deadline:
            self.assertIsNone(process.poll(), "producer exited before handshake")
            time.sleep(0.01)
        self.assertTrue(path.is_file(), "producer handshake timed out")

    def test_repeated_success_and_failure_do_not_accumulate_work_or_evidence(self):
        counts, sizes = [], []
        for index in range(8):
            code = 0 if index % 2 == 0 else 7
            result = self.store.begin("owned-source", [self.sdk]).run(self.fixture(code), timeout=5)
            self.assertEqual(result["returncode"], code)
            self.assertTrue(self.state()["retired"])
            self.assertFalse((self.scratch / "owned-source").exists())
            evidence = self.evidence()
            self.assertIn(f"owned fixture diagnostic {code}", evidence["diagnostic_tail"])
            inventory = self.root / "evidence/owned-source.files.json.gz"
            self.assertEqual(artifacts.digest(inventory), evidence["inventory_sha256"])
            rows = json.loads(gzip.decompress(inventory.read_bytes()))
            self.assertEqual({r["path"] for r in rows if r["path"].endswith((".class", ".dex", ".jar"))},
                             {"output/A.class", "output/classes.dex", "output/provider.jar"})
            paths = [p for p in self.root.rglob("*") if p.is_file()]
            counts.append(len(paths)); sizes.append(sum(p.stat().st_size for p in paths))
        self.assertEqual(len(set(counts)), 1)
        self.assertLess(max(sizes) - min(sizes), 200)

    def test_failed_launch_and_timeout_keep_diagnostic_and_retire_outputs(self):
        with self.assertRaises(FileNotFoundError):
            self.store.begin("owned-source").run([str(self.base / "missing")])
        self.assertTrue(self.state()["retired"])
        self.assertIn("FileNotFoundError", self.evidence()["exception"])
        result = self.store.begin("owned-source").run(
            [sys.executable, "-c", "import time; print('before timeout',flush=True); time.sleep(30)"],
            timeout=0.2)
        self.assertEqual(result["returncode"], 124)
        self.assertTrue(result["timed_out"])
        self.assertTrue(self.state()["retired"])
        self.assertIn("before timeout", self.evidence()["diagnostic_tail"])

    def test_pending_and_live_readers_preserve_outputs_and_old_cache_inputs(self):
        first = self.store.stage("declarations", self.sdk, self.sdk_hash, {"origin": "owned test"})
        job = self.store.begin("owned-source", [first], ["encoding", "metadata"])
        job.run(self.fixture(), timeout=5)
        other = self.base / "new-sdk.jar"; other.write_bytes(b"new qualified bytes")
        second = self.store.stage("declarations", other, artifacts.digest(other), {})
        self.assertTrue(first.exists())
        self.assertTrue(second.exists())
        again = artifacts.ArtifactStore(self.root, self.scratch)
        with self.assertRaisesRegex(RuntimeError, "reader"):
            again.begin("owned-source")
        with again.reader("owned-source", "encoding") as work:
            self.assertTrue((work / "output/A.class").is_file())
            with self.assertRaises(BlockingIOError):
                again.release("owned-source", "encoding")
            again.recover()
            self.assertTrue(work.is_dir())
        again.release("owned-source", "encoding")
        self.assertTrue(job.work.exists())
        self.assertTrue(first.exists())
        again.release("owned-source", "metadata")
        self.assertFalse(job.work.exists())
        self.assertFalse(first.exists())
        self.assertTrue(second.exists())
        again.retire_cache("declarations")
        self.assertFalse(second.exists())

    def test_live_producer_and_foreign_files_are_not_adopted_or_deleted(self):
        job = self.store.begin("source", [self.sdk])
        (job.work / "needed.class").write_bytes(b"active")
        again = artifacts.ArtifactStore(self.root, self.scratch)
        self.assertTrue((job.work / "needed.class").exists())
        with self.assertRaises(BlockingIOError):
            again.begin("source")
        job.finish("failed", 1)
        foreign = self.base / "foreign"; foreign.mkdir()
        (foreign / "source.java").write_text("valuable source")
        with self.assertRaisesRegex(ValueError, "nonempty"):
            artifacts.ArtifactStore(foreign)
        self.assertEqual((foreign / "source.java").read_text(), "valuable source")
        (self.scratch / "unregistered").mkdir()
        (self.scratch / "unregistered/keep.java").write_text("source")
        with self.assertRaisesRegex(ValueError, "unregistered"):
            again.begin("unregistered")
        again.recover()
        self.assertTrue((self.scratch / "unregistered/keep.java").exists())

    def test_cache_reuses_bytes_and_retires_replaced_versions(self):
        generated = self.base / "qualified.jar"
        for index in range(6):
            generated.write_bytes(bytes([index]) * 128)
            sha = artifacts.digest(generated)
            cached = self.store.stage("declarations", generated, sha, {"generation": index})
            before = cached.stat().st_mtime_ns
            self.assertEqual(self.store.cached("declarations", sha), cached)
            self.assertEqual(self.store.stage("declarations", generated, sha, {}), cached)
            self.assertEqual(cached.stat().st_mtime_ns, before)
            self.assertEqual(list((self.root / "cache").iterdir()), [cached])
        (self.root / "cache/interrupted.part").write_bytes(b"partial extraction")
        artifacts.ArtifactStore(self.root, self.scratch)
        self.assertFalse((self.root / "cache/interrupted.part").exists())
        with self.assertRaisesRegex(ValueError, "SHA-256"):
            self.store.stage("bad", generated, "0" * 64, {})

    def test_download_cache_hit_never_redownloads_and_failed_checksum_is_cleaned(self):
        first = self.store.download("published", self.sdk.as_uri(), self.sdk_hash)
        with patch.object(artifacts.urllib.request, "urlopen", side_effect=AssertionError("redownload")):
            self.assertEqual(self.store.download("published", self.sdk.as_uri(), self.sdk_hash), first)
        with self.assertRaisesRegex(ValueError, "SHA-256"):
            self.store.download("invalid", self.sdk.as_uri(), "0" * 64)
        self.assertTrue(self.state("sdk-invalid")["retired"])
        self.assertFalse((self.scratch / "sdk-invalid").exists())
        self.assertEqual(list((self.root / "cache").iterdir()), [first])

    def test_success_only_audit_reader_does_not_pin_failed_attempt(self):
        job = self.store.begin("probe", success_readers=["source-audit"])
        job.run(self.fixture(1))
        self.assertTrue(self.state("probe")["retired"])
        self.assertEqual(self.state("probe")["readers"], [])
        job = self.store.begin("probe", success_readers=["source-audit"])
        job.run(self.fixture())
        self.assertFalse(self.state("probe")["retired"])
        self.assertTrue((job.work / "output/A.class").is_file())
        self.store.release("probe", "source-audit")
        self.assertTrue(self.state("probe")["retired"])

    def test_qualified_output_is_promoted_before_command_work_is_retired(self):
        paths = []
        job = self.store.begin("sdk-declarations", [self.sdk])

        def promote(work):
            path = work / "output/provider.jar"
            paths.append(self.store.stage("declarations", path, artifacts.digest(path),
                                          {"source_input_sha256": self.sdk_hash}))

        result = job.run(self.fixture(), after_success=promote)
        self.assertEqual(result["returncode"], 0)
        self.assertTrue(paths[0].is_file())
        self.assertFalse(job.work.exists())
        self.assertEqual(paths[0].read_bytes(), b"fixture" * 512)

    def test_frozen_output_paths_are_owned_without_changing_the_command(self):
        output = self.base / "frozen-source-parent"
        job = self.store.begin("probe", [self.sdk], success_readers=["audit"], outputs=[output])
        command = [sys.executable, "-c",
                   "from pathlib import Path; import sys; p=Path(sys.argv[1]); p.mkdir(); "
                   "(p/'.native-staging').mkdir(); (p/'.native-staging/classes.dex').write_bytes(b'dex'); "
                   "print('frozen output written')", str(output)]
        job.run(command)
        self.assertEqual(self.state("probe")["command"], command)
        artifacts.ArtifactStore(self.root, self.scratch)
        self.assertTrue((output / ".native-staging/classes.dex").is_file())
        self.store.release("probe", "audit")
        self.assertFalse(output.exists())
        inventory = self.root / "evidence/probe.files.json.gz"
        self.assertIn("attached-0/.native-staging/classes.dex",
                      {row["path"] for row in json.loads(gzip.decompress(inventory.read_bytes()))})
        foreign = self.base / "foreign-output"; foreign.mkdir()
        (foreign / "keep.java").write_text("source")
        with self.assertRaisesRegex(ValueError, "absent or an empty"):
            self.store.begin("probe", outputs=[foreign])
        self.assertTrue((foreign / "keep.java").is_file())

    def test_attached_outputs_preserve_other_slot_records_and_allow_reuse(self):
        build = self.store.begin("build")
        self.assertEqual(build.run(self.fixture())["returncode"], 0)
        prior = (self.root / "jobs/build.json").read_bytes()
        held = self.store.begin("metadata", readers=["pending-consumer"])
        self.assertEqual(held.run(self.fixture())["returncode"], 0)
        pending = (self.root / "jobs/metadata.json").read_bytes()
        for code in (0, 7):
            output = self.base / "attached-output"
            job = self.store.begin("probe", [self.sdk], outputs=[output])
            self.assertEqual((self.root / "jobs/build.json").read_bytes(), prior)
            self.assertEqual((self.root / "jobs/metadata.json").read_bytes(), pending)
            self.assertEqual(self.state("probe")["slot"], "probe")
            command = [sys.executable, "-c",
                       "from pathlib import Path; import sys; p=Path(sys.argv[1]); p.mkdir(); "
                       "(p/'classes.dex').write_bytes(b'owned'); sys.exit(int(sys.argv[2]))",
                       str(output), str(code)]
            self.assertEqual(job.run(command)["returncode"], code)
            self.assertFalse(output.exists())
            self.assertEqual((self.root / "jobs/build.json").read_bytes(), prior)
            self.assertEqual((self.root / "jobs/metadata.json").read_bytes(), pending)
        self.store.release("metadata", "pending-consumer")
        reused = self.store.begin("build")
        self.assertEqual(reused.run(self.fixture())["returncode"], 0)

    def test_local_driver_enforces_slots_and_returns_actual_failure(self):
        from scripts import mobile_android_driver as driver
        config = {"state_root": str(self.root), "work_root": str(self.scratch),
                  "cwd": str(Path.cwd()), "slots": ["scope"], "sdk_aliases": ["declarations"],
                  "output_parents": [str(self.base)]}
        request = {"slot": "scope", "command": self.fixture(), "timeout_seconds": 5,
                   "inputs": [{"path": str(self.sdk), "sha256": self.sdk_hash}],
                   "publish": {"alias": "declarations", "file": "output/provider.jar"}}
        result = driver.run_request(config, request)
        self.assertEqual(result["returncode"], 0)
        self.assertTrue(Path(result["staged_input"]).is_file())
        self.assertFalse((self.scratch / "scope").exists())
        with self.assertRaisesRegex(ValueError, "configured job slot"):
            driver.run_request(config, {**request, "slot": "another-retry-directory"})
        config_file, request_file = self.base / "config.json", self.base / "request.json"
        artifacts.write_json(config_file, config)
        artifacts.write_json(request_file, {**request, "command": self.fixture(7)})
        process = subprocess.run([sys.executable, str(Path(driver.__file__)), "--config", str(config_file),
                                  "run", str(request_file)], capture_output=True, text=True, timeout=5)
        self.assertEqual(process.returncode, 7, process.stderr)
        self.assertEqual(json.loads(process.stdout)["returncode"], 7)
        self.assertTrue(self.state("scope")["retired"])

    def test_probe_entry_bounds_receipts_and_preserves_frozen_audit_paths(self):
        self._check_probe_entry(False)

    def test_probe_entry_resolves_relative_cli_from_configured_working_directory(self):
        self._check_probe_entry(True)

    def _check_probe_entry(self, relative_cli):
        from scripts import mobile_android_driver as driver
        cli = self.base / "owned-cli"
        cli.write_text(
            f"#!{sys.executable}\n"
            "from pathlib import Path\nimport sys,json\n"
            "p=Path(sys.argv[sys.argv.index('-o')+1]); p.parent.mkdir(parents=True,exist_ok=True)\n"
            "if '--succeed' not in sys.argv:\n"
            " (p.parent/'.native-partial').mkdir(); (p.parent/'.native-partial/data.dex').write_bytes(b'partial')\n"
            " print(json.dumps({'error':'owned generic rejection'})); sys.exit(1)\n"
            "p.mkdir(); (p/'report.json').write_text(json.dumps({'status':'success'}))\n"
            "(p/'Core.java').write_text('class Core {}'); print(json.dumps({'status':'success'}))\n")
        cli.chmod(0o755)
        original = self.base / "owned.apk"; original.write_bytes(b"owned input bytes")
        output = self.base / "frozen-output-root"
        receipt_root = self.base / "receipts"; receipt_root.mkdir()
        template_path, sdk_path, cli_receipt = [self.base / n for n in ("template.json", "sdk.json", "cli.json")]
        template = {"probe": 1, "input_sha256": artifacts.digest(original),
                    "command": ["./owned-cli" if relative_cli else str(cli), "mobile", str(original), "-o", str(output / "unused"),
                                "--classpath", str(self.sdk)]}
        artifacts.write_json(template_path, template)
        artifacts.write_json(sdk_path, {"status": "verified", "sdk_and_application_execution": False,
                                       "classpath": [{"path": str(self.sdk), "sha256": self.sdk_hash}]})
        artifacts.write_json(cli_receipt, {"sha256": artifacts.digest(cli)})
        config = {"state_root": str(self.root), "work_root": str(self.scratch),
                  "cwd": str(self.base if relative_cli else Path.cwd()),
                  "slots": ["probe"], "output_parents": [str(self.base)],
                  "probe": {"template": str(template_path), "classpath_manifest": str(sdk_path),
                            "output_root": str(output), "receipt_root": str(receipt_root)}}
        for number in (2, 3):
            self.assertEqual(driver.run_probe(config, number, cli_receipt)["returncode"], 1)
            self.assertFalse(output.exists())
            self.assertTrue(self.state("probe")["retired"])
            self.assertEqual(len(list(receipt_root.iterdir())), 2)
            receipt = artifacts.read_json(receipt_root / f"neverd-multidex-probe-{number}.command.json")
            self.assertEqual(receipt["command"][:3], template["command"][:3])
            self.assertEqual(receipt["command"][4], str(output / f"neverd-multidex-probe-{number}"))
            self.assertEqual(receipt["input_sha256"], template["input_sha256"])
            self.assertEqual(receipt["cli_sha256"], artifacts.digest(cli))
        self.assertEqual(artifacts.read_json(self.root / "probe-previous.json")["receipt"]["probe"], 2)
        template["command"].append("--succeed")
        artifacts.write_json(template_path, template)
        self.assertEqual(driver.run_probe(config, 4, cli_receipt)["returncode"], 0)
        self.assertEqual(self.state("probe")["readers"], ["full-source-audit"])
        self.assertTrue((output / "neverd-multidex-probe-4/Core.java").is_file())
        self.assertEqual(len(list(receipt_root.iterdir())), 2)
        self.store.release("probe", "full-source-audit")
        self.assertFalse(output.exists())
        self.assertEqual(artifacts.digest(original), template["input_sha256"])

    def test_interrupted_ci_does_not_start_the_next_profile(self):
        from scripts.ci import mobile_android_shards as shards
        profiles = [{"id": "first", "jdk": 21}, {"id": "second", "jdk": 25}]
        output = self.base / "ci-evidence"

        def run(profile, env, log):
            log.write_text("interrupted")
            return {**profile, "returncode": 143, "log": str(log)}

        with patch.object(shards, "profiles", return_value=profiles), \
                patch.object(shards, "run_profile", side_effect=run) as execute, \
                patch.object(sys, "argv", ["shards", "--shard", "0", "--shard-count", "1",
                                           "--output", str(output)]), \
                patch("sys.stdout", new_callable=io.StringIO):
            self.assertEqual(shards.main(), 143)
        self.assertEqual(execute.call_count, 1)
        self.assertEqual(artifacts.read_json(output / "summary.json")["completed"], 1)

    def test_ci_entry_reuses_slot_and_retires_successful_and_failed_profile_work(self):
        from scripts.ci import mobile_android_shards as shards
        fixture = self.base / "fixture.py"
        fixture.write_text(
            "from pathlib import Path\nimport sys\n"
            "p=Path(sys.argv[sys.argv.index('--work-dir')+1]); p.mkdir()\n"
            "(p/'generated.class').write_bytes(b'fixture')\n"
            "print('CI fixture diagnostic')\nsys.exit(int(sys.argv[-1]))\n")
        env = dict(os.environ, RUNNER_TEMP=str(self.base / "runner"),
                   NEVERD_JAVA21_HOME="/jdk21", NEVERD_JAVA25_HOME="/jdk25")
        requested = self.base / "precious"; requested.mkdir()
        (requested / "keep.java").write_text("source outside owned store")
        counts = []
        for code in (0, 1, 0):
            profile = {"id": "android-owned-fixture", "jdk": 21, "timeout_seconds": 5,
                       "argv": [str(fixture), "--work-dir", str(requested), str(code)]}
            result = shards.run_profile(profile, env, self.base / "profile.log")
            self.assertEqual(result["returncode"], code)
            self.assertIn("CI fixture diagnostic", (self.base / "profile.log").read_text())
            evidence = artifacts.read_json(Path(result["evidence"]))
            self.assertTrue(evidence["retired"])
            counts.append(len([p for p in Path(env["RUNNER_TEMP"]).rglob("*") if p.is_file()]))
        self.assertEqual(len(set(counts)), 1)
        self.assertEqual((requested / "keep.java").read_text(), "source outside owned store")

    def test_sigterm_stops_child_before_retiring_work(self):
        ready = self.base / "ready"
        script = ("from pathlib import Path; import os,sys,time; "
                  "Path(sys.argv[1]).write_text(str(os.getpid())); "
                  "print('interrupt diagnostic',flush=True); time.sleep(30)")
        command = [sys.executable, str(Path(artifacts.__file__)), "--root", str(self.root),
                   "--work-root", str(self.scratch), "run", "--slot", "interrupted", "--",
                   sys.executable, "-c", script, str(ready)]
        with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as process:
            try:
                self.wait_file(ready, process)
                process.send_signal(signal.SIGTERM)
                stdout, stderr = process.communicate(timeout=8)
                self.assertEqual(process.returncode, 143, (stdout, stderr))
                self.assertTrue(self.state("interrupted")["retired"])
                self.assertEqual(self.state("interrupted")["phase"], "interrupted")
                with self.assertRaises(ProcessLookupError):
                    os.kill(int(ready.read_text()), 0)
            finally:
                if process.poll() is None:
                    process.kill(); process.wait()

    def test_sigkill_is_recovered_on_next_start_instead_of_claiming_finally(self):
        ready = self.base / "ready"
        attached = self.base / "native-output-parent"
        script = (
            "from scripts.mobile_android_artifacts import ArtifactStore; "
            "from pathlib import Path; import sys,time; "
            "s=ArtifactStore(Path(sys.argv[1]),Path(sys.argv[2])); "
            "p=Path(sys.argv[4]); j=s.begin('abandoned',success_readers=['audit'],outputs=[p]); "
            "p.mkdir(); (p/'partial.dex').write_bytes(b'partial native staging'); "
            "(j.work/'unused.class').write_bytes(b'owned artifact'); "
            "Path(sys.argv[3]).write_text('ready'); time.sleep(30)"
        )
        with subprocess.Popen([sys.executable, "-c", script, str(self.root), str(self.scratch), str(ready), str(attached)]) as process:
            try:
                self.wait_file(ready, process)
                self.assertTrue((self.scratch / "abandoned/unused.class").exists())
                process.kill(); process.wait(timeout=5)
                self.assertEqual(process.returncode, -signal.SIGKILL)
                self.assertTrue((self.scratch / "abandoned/unused.class").exists())
                artifacts.ArtifactStore(self.root, self.scratch)
                self.assertFalse((self.scratch / "abandoned").exists())
                self.assertFalse(attached.exists())
                self.assertEqual(self.state("abandoned")["phase"], "abandoned")
                self.assertTrue(self.state("abandoned")["retired"])
                self.assertEqual(self.state("abandoned")["readers"], [])
                self.assertEqual(self.evidence("abandoned")["files"], 3)
            finally:
                if process.poll() is None:
                    process.kill(); process.wait()

    def test_sigkill_of_driver_preserves_surviving_child_until_next_recovery(self):
        ready = self.base / "child-ready"
        child = ("from pathlib import Path; import os,sys,time; "
                 "Path(sys.argv[1]).write_text(str(os.getpid())); time.sleep(30)")
        command = [sys.executable, str(Path(artifacts.__file__)), "--root", str(self.root),
                   "--work-root", str(self.scratch), "run", "--slot", "child", "--",
                   sys.executable, "-c", child, str(ready)]
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        pid = None
        try:
            self.wait_file(ready, process)
            pid = int(ready.read_text())
            process.kill(); process.communicate(timeout=5)
            artifacts.ArtifactStore(self.root, self.scratch)
            self.assertTrue((self.scratch / "child").is_dir())
            self.assertEqual(self.state("child")["phase"], "running")
            os.killpg(pid, signal.SIGTERM)
            deadline = time.monotonic() + 5
            while artifacts.group_alive(pid) and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertFalse(artifacts.group_alive(pid), "owned child did not exit")
            artifacts.ArtifactStore(self.root, self.scratch)
            self.assertFalse((self.scratch / "child").exists())
            self.assertTrue(self.state("child")["retired"])
        finally:
            if process.poll() is None:
                process.kill(); process.communicate()
            if pid is not None and artifacts.group_alive(pid):
                os.killpg(pid, signal.SIGKILL)

    def test_symlink_target_and_source_are_preserved_during_owned_cleanup(self):
        job = self.store.begin("source")
        (job.work / "external").symlink_to(self.sdk)
        job.finish("failed", 1)
        self.assertTrue(self.sdk.is_file())
        self.assertFalse(job.work.exists())


if __name__ == "__main__":
    unittest.main()
