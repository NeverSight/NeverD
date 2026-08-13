from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest import mock

from scripts import check_source_provenance as provenance


class ProvenanceScanTests(unittest.TestCase):
    """The checker has to be trusted before its verdict is worth anything.

    Both directions matter and they fail differently.  A miss lets a leak
    through quietly; a false positive gets the whole check switched off by the
    next person it inconveniences, which is worse because it looks like nothing
    is wrong.
    """

    def scan(self, name: str, text: str) -> list[str]:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
            with mock.patch.object(provenance, "REPO_ROOT", root):
                return provenance.scan([name])

    def test_catches_a_developer_path(self) -> None:
        findings = self.scan("a.cpp", '// see /Users/someone/notes.txt\n')
        self.assertEqual(len(findings), 1)
        self.assertIn("private-path", findings[0])

    def test_catches_a_directory_kept_out_of_the_repository(self) -> None:
        findings = self.scan("a.md", "read local_docs/design.md\n")
        self.assertEqual(len(findings), 1)
        self.assertIn("private-path", findings[0])

    def test_catches_an_unattributed_project_name(self) -> None:
        findings = self.scan("a.cpp", "// the way Triton does it\n")
        self.assertEqual(len(findings), 1)
        self.assertIn("foreign-project", findings[0])

    def test_catches_borrowed_terminology(self) -> None:
        findings = self.scan("a.h", "/// each varnode is addressed by byte\n")
        self.assertEqual(len(findings), 1)
        self.assertIn("foreign-terminology", findings[0])

    def test_leaves_ordinary_english_alone(self) -> None:
        # Every line here tripped an earlier draft of the patterns.  "reported
        # from" and "exported from" contain "ported from"; "setup-codex"
        # contains "p-code"; "derived from" is how anyone describes a computed
        # value.  A checker that flags these is a checker nobody runs twice.
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
        allowed = provenance.Allowance(
            path="docs/*.md", rule="foreign-project", reason="reviewed"
        )
        with mock.patch.object(provenance, "ALLOWED", (allowed,)):
            self.assertEqual(self.scan("docs/x.md", "Triton\n"), [])
            # The allowance names one rule, and says nothing about the others.
            self.assertEqual(len(self.scan("docs/x.md", "/Users/someone\n")), 1)

    def test_skips_files_it_cannot_read_as_text(self) -> None:
        self.assertEqual(self.scan("a.png", "Triton\n"), [])

    def test_exempts_itself_and_its_own_tests(self) -> None:
        # Both files necessarily contain examples of what is being looked for,
        # so a check that scanned them could never pass.
        self.assertIn("scripts/check_source_provenance.py", provenance.EXEMPT)
        self.assertIn(
            "scripts/tests/test_check_source_provenance.py", provenance.EXEMPT
        )


class ProvenanceOfThisRepositoryTests(unittest.TestCase):
    def test_no_finding_outside_developer_tooling(self) -> None:
        """NeverD's own sources carry no provenance markers.

        ``.agents`` holds assistant tooling rather than anything NeverD builds
        or ships, so it is reported separately: what matters for this assertion
        is that nothing in the product itself is flagged.
        """

        findings = [
            finding
            for finding in provenance.scan(provenance.tracked_files())
            if not finding.startswith(".agents/")
        ]
        self.assertEqual(findings, [], "\n".join(findings))


if __name__ == "__main__":
    unittest.main()
