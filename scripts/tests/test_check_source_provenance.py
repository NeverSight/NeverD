from __future__ import annotations

import contextlib
import io
import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from scripts import check_source_provenance as provenance


class ExternalPolicyTests(unittest.TestCase):
    @staticmethod
    def run_checker(
        root: Path, argv: list[str], environ: dict[str, str]
    ) -> tuple[int, str, str]:
        stdout = io.StringIO()
        stderr = io.StringIO()
        with (
            mock.patch.object(provenance, "REPO_ROOT", root),
            contextlib.redirect_stdout(stdout),
            contextlib.redirect_stderr(stderr),
        ):
            result = provenance.main(argv, environ=environ)
        return result, stdout.getvalue(), stderr.getvalue()

    def test_external_policy_detects_a_configured_term(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            root = base / "repo"
            root.mkdir()
            (root / "sample.cpp").write_text(
                "// example-engine implementation\n", encoding="utf-8"
            )
            policy = base / "policy.json"
            policy.write_text(
                json.dumps(
                    {
                        "schema": 1,
                        "foreign_projects": ["example-engine"],
                    }
                ),
                encoding="utf-8",
            )

            result, _, stderr = self.run_checker(
                root,
                ["sample.cpp"],
                {provenance.EXTERNAL_POLICY_ENV: str(policy)},
            )

        self.assertEqual(result, 1)
        self.assertIn("foreign-project", stderr)
        self.assertNotIn("example-engine", stderr)

    def test_unset_external_policy_uses_only_generic_defaults(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "sample.cpp").write_text(
                "// example-engine implementation\n", encoding="utf-8"
            )

            result, stdout, stderr = self.run_checker(root, ["sample.cpp"], {})

        self.assertEqual(result, 0)
        self.assertIn("source provenance check passed", stdout)
        self.assertEqual(stderr, "")

    def test_missing_external_policy_fails_closed_without_echoing_its_path(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            root = base / "repo"
            root.mkdir()
            (root / "sample.cpp").write_text("int value;\n", encoding="utf-8")
            missing = base / "confidential-policy.json"

            result, stdout, stderr = self.run_checker(
                root,
                ["sample.cpp"],
                {provenance.EXTERNAL_POLICY_ENV: str(missing)},
            )

        self.assertEqual(result, 2)
        self.assertEqual(stdout, "")
        self.assertIn("source provenance policy", stderr)
        self.assertNotIn(str(missing), stderr)

    def test_malformed_external_policy_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            root = base / "repo"
            root.mkdir()
            (root / "sample.cpp").write_text("int value;\n", encoding="utf-8")
            policy = base / "policy.json"
            policy.write_text('{"schema": 1,', encoding="utf-8")

            result, stdout, stderr = self.run_checker(
                root,
                ["sample.cpp"],
                {provenance.EXTERNAL_POLICY_ENV: str(policy)},
            )

        self.assertEqual(result, 2)
        self.assertEqual(stdout, "")
        self.assertIn("valid UTF-8 JSON", stderr)

    def test_invalid_external_policy_shapes_fail_closed(self) -> None:
        invalid_documents = (
            [],
            {},
            {"schema": False, "foreign_projects": ["example-engine"]},
            {"schema": 2, "foreign_projects": ["example-engine"]},
            {"schema": 1, "unsupported": ["example-engine"]},
            {"schema": 1, "foreign_projects": "example-engine"},
            {"schema": 1, "foreign_projects": [" example-engine"]},
            {"schema": 1, "foreign_projects": ["three token phrase"]},
            {
                "schema": 1,
                "foreign_projects": ["example-engine"],
                "foreign_terminology": ["EXAMPLE-ENGINE"],
            },
            {"schema": 1, "foreign_projects": []},
        )

        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            root = base / "repo"
            root.mkdir()
            (root / "sample.cpp").write_text("int value;\n", encoding="utf-8")
            policy = base / "policy.json"

            for document in invalid_documents:
                with self.subTest(document=document):
                    policy.write_text(json.dumps(document), encoding="utf-8")
                    result, stdout, stderr = self.run_checker(
                        root,
                        ["sample.cpp"],
                        {provenance.EXTERNAL_POLICY_ENV: str(policy)},
                    )
                    self.assertEqual(result, 2)
                    self.assertEqual(stdout, "")
                    self.assertIn("source provenance policy", stderr)

    def test_policy_file_inside_repository_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "sample.cpp").write_text("int value;\n", encoding="utf-8")
            policy = root / "policy.json"
            policy.write_text(
                json.dumps({"schema": 1, "foreign_projects": ["example-engine"]}),
                encoding="utf-8",
            )

            result, stdout, stderr = self.run_checker(
                root,
                ["sample.cpp"],
                {provenance.EXTERNAL_POLICY_ENV: str(policy)},
            )

        self.assertEqual(result, 2)
        self.assertEqual(stdout, "")
        self.assertIn("outside the repository", stderr)
        self.assertNotIn(str(policy), stderr)

    def test_non_utf8_external_policy_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            root = base / "repo"
            root.mkdir()
            (root / "sample.cpp").write_text("int value;\n", encoding="utf-8")
            policy = base / "policy.json"
            policy.write_bytes(b"\xff")

            result, stdout, stderr = self.run_checker(
                root,
                ["sample.cpp"],
                {provenance.EXTERNAL_POLICY_ENV: str(policy)},
            )

        self.assertEqual(result, 2)
        self.assertEqual(stdout, "")
        self.assertIn("valid UTF-8 JSON", stderr)


class ProvenanceScanTests(unittest.TestCase):
    """The checker has to be trusted before its verdict is worth anything.

    Both directions matter and they fail differently.  A miss lets a leak
    through quietly; a false positive gets the whole check switched off by the
    next person it inconveniences, which is worse because it looks like nothing
    is wrong.
    """

    @staticmethod
    def term_rule(name: str, term: str) -> provenance.Rule:
        return provenance.Rule(
            name=name,
            explanation="test policy term",
            terms=frozenset({term.casefold()}),
        )

    def scan(
        self,
        name: str,
        text: str,
        rules: tuple[provenance.Rule, ...] | None = None,
    ) -> list[str]:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
            with mock.patch.object(provenance, "REPO_ROOT", root):
                if rules is None:
                    return provenance.scan([name])
                return provenance.scan([name], rules)

    def test_catches_a_developer_path(self) -> None:
        findings = self.scan("a.cpp", "// see /Users/someone/notes.txt\n")
        self.assertEqual(len(findings), 1)
        self.assertIn("private-path", findings[0])

    def test_catches_a_unicode_developer_path(self) -> None:
        findings = self.scan("a.cpp", "// see /Users/张三/notes.txt\n")
        self.assertEqual(len(findings), 1)
        self.assertIn("private-path", findings[0])

    def test_catches_a_directory_kept_out_of_the_repository(self) -> None:
        rule = self.term_rule("private-path", "private_workspace")
        findings = self.scan("a.md", "read private_workspace/design.md\n", (rule,))
        self.assertEqual(len(findings), 1)
        self.assertIn("private-path", findings[0])

    def test_catches_an_unattributed_project_name(self) -> None:
        rule = self.term_rule("foreign-project", "example-engine")
        findings = self.scan("a.cpp", "// the way example-engine does it\n", (rule,))
        self.assertEqual(len(findings), 1)
        self.assertIn("foreign-project", findings[0])

    def test_non_latin_text_does_not_hide_a_project_name(self) -> None:
        rule = self.term_rule("foreign-project", "example-engine")
        findings = self.scan("a.cpp", "// example-engine을 사용\n", (rule,))
        self.assertEqual(len(findings), 1)
        self.assertIn("foreign-project", findings[0])

    def test_scans_assembly_and_typescript_sources(self) -> None:
        self.assertEqual(len(self.scan("a.s", "/Users/someone/input\n")), 1)
        self.assertEqual(len(self.scan("a.ts", "/Users/someone/input\n")), 1)

    def test_catches_borrowed_terminology(self) -> None:
        rule = self.term_rule("foreign-terminology", "borrowed-node")
        findings = self.scan(
            "a.h", "/// each borrowed-node is addressed by byte\n", (rule,)
        )
        self.assertEqual(len(findings), 1)
        self.assertIn("foreign-terminology", findings[0])

    def test_leaves_ordinary_english_alone(self) -> None:
        # Every line here tripped an earlier draft of the patterns.  "reported
        # from" and "exported from" contain a shorter suspicious phrase;
        # "derived from" is how anyone describes a computed value.  A checker
        # that flags these is a checker nobody runs twice.
        self.assertEqual(
            self.scan(
                "a.cpp",
                "// a failure reported from several worker threads\n"
                "// a descriptor imported from libobjc\n"
                "// helpers not exported from neverd_shared\n"
                "// run setup-codex-hook.sh first\n"
                "// the address derived from the frame pointer\n",
            ),
            [],
        )

    def test_honours_a_reviewed_allowance(self) -> None:
        rule = self.term_rule("foreign-project", "example-engine")
        allowed = provenance.Allowance(
            path="docs/*.md", rule="foreign-project", reason="reviewed"
        )
        with mock.patch.object(provenance, "ALLOWED", (allowed,)):
            self.assertEqual(self.scan("docs/x.md", "example-engine\n", (rule,)), [])
            # The allowance names one rule, and says nothing about the others.
            self.assertEqual(len(self.scan("docs/x.md", "/Users/someone\n")), 1)

    def test_skips_files_it_cannot_read_as_text(self) -> None:
        rule = self.term_rule("foreign-project", "example-engine")
        self.assertEqual(self.scan("a.png", "example-engine\n", (rule,)), [])

    def test_exempts_itself_and_its_own_tests(self) -> None:
        # Both files necessarily contain examples of what is being looked for,
        # so a check that scanned them could never pass.
        self.assertIn("scripts/check_source_provenance.py", provenance.EXEMPT)
        self.assertIn(
            "scripts/tests/test_check_source_provenance.py", provenance.EXEMPT
        )


class ProvenanceOfThisRepositoryTests(unittest.TestCase):
    @staticmethod
    def commit(root: Path, message: str) -> None:
        subprocess.run(
            (
                "git",
                "-c",
                "user.name=NeverD Test",
                "-c",
                "user.email=test@example.invalid",
                "commit",
                "-q",
                "-m",
                message,
            ),
            cwd=root,
            check=True,
        )

    def test_worktree_inventory_includes_untracked_but_not_ignored_files(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(("git", "init", "-q"), cwd=root, check=True)
            (root / ".gitignore").write_text("ignored.cpp\n", encoding="utf-8")
            (root / "tracked.cpp").write_text("tracked\n", encoding="utf-8")
            (root / "pending.cpp").write_text("pending\n", encoding="utf-8")
            (root / "ignored.cpp").write_text("ignored\n", encoding="utf-8")
            subprocess.run(
                ("git", "add", ".gitignore", "tracked.cpp"), cwd=root, check=True
            )

            with mock.patch.object(provenance, "REPO_ROOT", root):
                paths = provenance.worktree_files()

            self.assertIn(".gitignore", paths)
            self.assertIn("tracked.cpp", paths)
            self.assertIn("pending.cpp", paths)
            self.assertNotIn("ignored.cpp", paths)

    def test_worktree_patch_scans_deleted_lines(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(("git", "init", "-q"), cwd=root, check=True)
            path = root / "source.cpp"
            path.write_text("// private-token\n", encoding="utf-8")
            subprocess.run(("git", "add", "source.cpp"), cwd=root, check=True)
            self.commit(root, "initial")
            path.write_text("int value;\n", encoding="utf-8")
            rule = ProvenanceScanTests.term_rule("private-path", "private-token")

            with mock.patch.object(provenance, "REPO_ROOT", root):
                findings = provenance.scan_worktree_patch((rule,))

            self.assertEqual(len(findings), 1)
            self.assertIn("source.cpp@worktree-deletion", findings[0])

    def test_recent_history_scans_content_removed_by_a_commit(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(("git", "init", "-q"), cwd=root, check=True)
            path = root / "source.cpp"
            path.write_text("// private-token\n", encoding="utf-8")
            subprocess.run(("git", "add", "source.cpp"), cwd=root, check=True)
            self.commit(root, "initial")
            path.write_text("int value;\n", encoding="utf-8")
            subprocess.run(("git", "add", "source.cpp"), cwd=root, check=True)
            self.commit(root, "remove private text")
            rule = ProvenanceScanTests.term_rule("private-path", "private-token")

            with mock.patch.object(provenance, "REPO_ROOT", root):
                findings = provenance.scan_recent_history(1, (rule,))

            self.assertEqual(len(findings), 1)
            self.assertIn("source.cpp@", findings[0])

    def test_ci_fetches_and_scans_recent_history(self) -> None:
        workflow = (provenance.REPO_ROOT / ".github/workflows/ci.yml").read_text(
            encoding="utf-8"
        )
        self.assertIn("fetch-depth: 32", workflow)
        self.assertIn(
            "scripts/check_source_provenance.py --history-commits 16", workflow
        )

    def test_no_unreviewed_finding_in_pending_worktree(self) -> None:
        """Tracked and pending source have no unreviewed provenance finding."""

        findings = provenance.scan(provenance.worktree_files())
        self.assertEqual(findings, [], "\n".join(findings))


if __name__ == "__main__":
    unittest.main()
