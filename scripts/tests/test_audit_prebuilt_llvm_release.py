import copy
import subprocess
import unittest
from pathlib import Path
from urllib.error import URLError

from scripts import audit_prebuilt_llvm_release as audit


ROOT = Path(__file__).resolve().parents[2]
CONSUMER = ROOT / "cmake" / "NeverDLLVMPrebuilt.cmake"
WORKFLOW = ROOT / ".github" / "workflows" / "prebuilt-llvm-audit.yml"


class PrebuiltLlvmReleaseAuditTests(unittest.TestCase):
    def setUp(self):
        self.pins = audit.PinSet(
            repository="example/llvm-project",
            tag="neverd-llvm-v23.0.0-r1",
            commit="a" * 40,
            assets=(
                audit.AssetPin("neverd-llvm-linux-x86_64.tar.xz", "1" * 64),
                audit.AssetPin("neverd-llvm-macos-arm64.tar.xz", "2" * 64),
                audit.AssetPin("neverd-llvm-windows-x64.zip", "3" * 64),
            ),
        )
        self.release = {
            "tag_name": self.pins.tag,
            "target_commitish": self.pins.commit,
            "assets": [
                {
                    "name": asset.name,
                    "digest": f"sha256:{asset.digest}",
                    "browser_download_url": (
                        "https://github.com/example/llvm-project/releases/download/"
                        f"{self.pins.tag}/{asset.name}"
                    ),
                }
                for asset in self.pins.assets
            ]
            + [
                {
                    "name": f"{asset.name}.sha256",
                    "digest": "sha256:" + "f" * 64,
                    "browser_download_url": (
                        "https://github.com/example/llvm-project/releases/download/"
                        f"{self.pins.tag}/{asset.name}.sha256"
                    ),
                }
                for asset in self.pins.assets
            ],
        }
        self.sidecars = {
            f"{asset.name}.sha256": f"{asset.digest}  {asset.name}\n"
            for asset in self.pins.assets
        }
        self.tag_ref = {
            "object": {"type": "commit", "sha": self.pins.commit}
        }

    def test_current_consumer_pins_are_complete_and_match_the_gitlink(self):
        pins = audit.parse_pins(CONSUMER.read_text(encoding="utf-8"))

        self.assertEqual(pins.repository, "NeverSight/llvm-project")
        self.assertEqual(len(pins.assets), 3)
        gitlink = subprocess.run(
            ["git", "ls-tree", "HEAD", "third_party/llvm-project"],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        self.assertEqual(gitlink.returncode, 0, gitlink.stdout)
        self.assertEqual(audit.audit_gitlink(pins, gitlink.stdout), pins.commit)

    def test_matching_release_and_sidecars_pass(self):
        result = audit.audit_release(
            self.pins, self.release, self.tag_ref, self.sidecars
        )

        self.assertEqual(result, tuple(asset.name for asset in self.pins.assets))

    def test_pinned_commit_must_match_the_consumer_gitlink(self):
        matching_tree = (
            f"160000 commit {self.pins.commit}\tthird_party/llvm-project\n"
        )
        self.assertEqual(
            audit.audit_gitlink(self.pins, matching_tree), self.pins.commit
        )

        drifting_tree = (
            f"160000 commit {'b' * 40}\tthird_party/llvm-project\n"
        )
        with self.assertRaisesRegex(audit.AuditError, "submodule gitlink"):
            audit.audit_gitlink(self.pins, drifting_tree)

        with self.assertRaisesRegex(audit.AuditError, "expected one LLVM gitlink"):
            audit.audit_gitlink(self.pins, "")

    def test_release_metadata_or_checksum_drift_fails_closed(self):
        cases = []

        wrong_commit = copy.deepcopy(self.release)
        wrong_commit["target_commitish"] = "b" * 40
        cases.append((wrong_commit, self.tag_ref, self.sidecars, "target commit"))

        wrong_tag_ref = copy.deepcopy(self.tag_ref)
        wrong_tag_ref["object"]["sha"] = "b" * 40
        cases.append(
            (self.release, wrong_tag_ref, self.sidecars, "Git tag target")
        )

        wrong_digest = copy.deepcopy(self.release)
        wrong_digest["assets"][0]["digest"] = "sha256:" + "0" * 64
        cases.append((wrong_digest, self.tag_ref, self.sidecars, "archive digest"))

        missing_sidecar = copy.deepcopy(self.release)
        missing_sidecar["assets"] = [
            asset
            for asset in missing_sidecar["assets"]
            if asset["name"] != "neverd-llvm-linux-x86_64.tar.xz.sha256"
        ]
        cases.append(
            (missing_sidecar, self.tag_ref, self.sidecars, "missing release asset")
        )

        wrong_sidecars = dict(self.sidecars)
        wrong_sidecars["neverd-llvm-linux-x86_64.tar.xz.sha256"] = (
            f"{'0' * 64}  neverd-llvm-linux-x86_64.tar.xz\n"
        )
        cases.append(
            (self.release, self.tag_ref, wrong_sidecars, "checksum sidecar")
        )

        for release, tag_ref, sidecars, diagnostic in cases:
            with self.subTest(diagnostic=diagnostic):
                with self.assertRaisesRegex(audit.AuditError, diagnostic):
                    audit.audit_release(self.pins, release, tag_ref, sidecars)

    def test_duplicate_asset_names_fail_closed(self):
        release = copy.deepcopy(self.release)
        release["assets"].append(copy.deepcopy(release["assets"][0]))

        with self.assertRaisesRegex(audit.AuditError, "duplicate release asset"):
            audit.audit_release(self.pins, release, self.tag_ref, self.sidecars)

    def test_network_reads_retry_transient_failures(self):
        attempts = 0
        delays = []

        class Response:
            def __enter__(self):
                return self

            def __exit__(self, *unused):
                return False

            def read(self, limit):
                self.limit = limit
                return b"ok"

        response = Response()

        def open_url(request, timeout):
            nonlocal attempts
            attempts += 1
            if attempts < 3:
                raise URLError("transient")
            self.assertEqual(timeout, 30)
            return response

        payload = audit._read_url(
            "https://api.github.com/example",
            headers={},
            limit=4,
            opener=open_url,
            sleeper=delays.append,
        )

        self.assertEqual(payload, b"ok")
        self.assertEqual(attempts, 3)
        self.assertEqual(delays, [1, 2])
        self.assertEqual(response.limit, 5)

    def test_scheduled_workflow_runs_tests_and_live_audit(self):
        source = WORKFLOW.read_text(encoding="utf-8")

        for expected in (
            "name: Prebuilt LLVM Audit",
            "    branches:\n      - dev",
            "  pull_request:",
            "  schedule:",
            "    - cron: '17 */6 * * *'",
            "  workflow_dispatch:",
            "  contents: read",
            "runs-on: ubuntu-24.04",
            "timeout-minutes: 5",
            "actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1",
            "persist-credentials: false",
            "actions/setup-python@5fda3b95a4ea91299a34e894583c3862153e4b97",
            "python-version: '3.12'",
            "python -m unittest -v scripts.tests.test_audit_prebuilt_llvm_release",
            "python scripts/audit_prebuilt_llvm_release.py",
            "GITHUB_TOKEN: ${{ secrets.GITHUB_TOKEN }}",
        ):
            with self.subTest(expected=expected):
                self.assertIn(expected, source)


if __name__ == "__main__":
    unittest.main()
