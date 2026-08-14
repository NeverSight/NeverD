from __future__ import annotations

from pathlib import Path
import unittest

from scripts import check_docs_i18n as i18n


class _TextView:
    """Answer reads from memory, so a helper can be probed on its own."""

    def __init__(self, text: str) -> None:
        self._text = text

    def read_text(self, path: Path) -> str:
        return self._text


class LocalizedDocumentationMatrixTests(unittest.TestCase):
    def test_every_public_document_family_is_registered(self) -> None:
        self.assertEqual(
            set(i18n.ENGLISH_DOCS),
            {
                Path("README.md"),
                Path("CONTRIBUTING.md"),
                Path("docs/README.md"),
                Path("docs/architecture.md"),
                Path("docs/python-plugins.md"),
                Path("docs/roadmap/README.md"),
                Path("docs/testing.md"),
                Path("docs/windows-exception-reconstruction.md"),
                Path("docs/evm.md"),
                Path("docs/sbf.md"),
            },
        )

    def test_repository_documentation_matrix_is_valid(self) -> None:
        """Exercise the same working-tree validation that the CLI performs."""
        errors: list[str] = []
        view = i18n.RepositoryView(use_index=False)
        i18n.validate_matrix(errors, view)
        existing = tuple(path for path in i18n.MARKDOWN_DOCS if view.exists(path))
        i18n.validate_links(existing, errors, view)
        i18n.validate_markdown_structure(existing, errors, view)
        self.assertEqual(errors, [])

    def test_every_english_document_has_a_counterpart_in_every_locale(self) -> None:
        self.assertEqual(
            set(i18n.LOCALES),
            {"ar", "de", "es", "fr", "it", "ja", "ko", "ru", "zh-CN", "zh-TW"},
        )
        for locale in i18n.LOCALES:
            localized = i18n.localized_paths(locale)
            with self.subTest(locale=locale):
                self.assertEqual(len(localized), len(i18n.ENGLISH_DOCS))
                for path in localized:
                    self.assertTrue(path.name.endswith(f".{locale}.md"), path)
                    self.assertIn(path, i18n.MARKDOWN_DOCS)

    # Headings a reader can write in any script have to slug the same way the
    # links to them are spelled, and a heading inside a sample is not a heading.
    def test_anchors_skip_fenced_samples_and_number_repeated_headings(self) -> None:
        anchors = i18n.markdown_anchors(
            Path("docs/example.md"),
            _TextView(
                "# 概要 / Overview\n"
                "```\n"
                "# not a heading\n"
                "```\n"
                "## Notes\n"
                "## Notes\n"
                '<a id="recovered-facts"></a>\n'
            ),
        )
        self.assertEqual(
            anchors, {"概要-overview", "notes", "notes-1", "recovered-facts"}
        )

    def test_an_unclosed_fence_is_reported_at_the_line_that_opened_it(self) -> None:
        errors: list[str] = []
        i18n.validate_markdown_structure(
            (Path("docs/example.md"),),
            errors,
            _TextView("intro\n\n```bash\nctest --test-dir build\n"),
        )
        self.assertEqual(errors, ["docs/example.md:3: unclosed Markdown fence"])

    # A check nobody runs drifts until it is red for reasons nobody chose, so
    # the workflow's own text is what says this one is still wired in.
    def test_continuous_integration_runs_the_check(self) -> None:
        workflow = (
            i18n.REPO_ROOT / ".github" / "workflows" / "ci.yml"
        ).read_text(encoding="utf-8")
        self.assertIn('"$PYTHON" scripts/check_docs_i18n.py', workflow)


if __name__ == "__main__":
    unittest.main()
