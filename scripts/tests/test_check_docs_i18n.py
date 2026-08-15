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


class _PathTextView:
    """Answer reads from path-keyed text for cross-document validation."""

    def __init__(self, texts: dict[Path, str]) -> None:
        self._texts = texts

    def read_text(self, path: Path) -> str:
        return self._texts[path]


class _OverlayView:
    """Overlay selected working-tree files while preserving the real matrix."""

    def __init__(self, overrides: dict[Path, str]) -> None:
        self._overrides = overrides
        self._base = i18n.RepositoryView(use_index=False)

    def read_text(self, path: Path) -> str:
        if path in self._overrides:
            return self._overrides[path]
        return self._base.read_text(path)

    def exists(self, path: Path) -> bool:
        return self._base.exists(path)


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

    def test_every_architecture_locale_rejects_each_missing_semantic_token(
        self,
    ) -> None:
        capability_tokens = {
            "translation.runtime-contract": (
                "`TranslationObjectCompilerV1`",
                "`TranslationObjectRequestV1`",
                "`ProvenSemanticAndLLVM`",
                "`neverd_translate_x86_64_block_to_aarch64_object_v1`",
                "`translate_x86_64_block_to_aarch64_object`",
                "`neverd translate-object`",
            ),
            "translation.executable-engine": (
                "`verifyTranslationLinkGraphV1`",
                "`linkTranslationObjectV1`",
                "`NativeTranslationSessionV1`",
            ),
            "exception.rewrite.end-to-end": (
                "`__unwind_info`",
                "`__TEXT,__unwind_info`",
                "`__LINKEDIT`",
            ),
            "exception.itanium.ada-d": (
                "`Exception_Id`",
                "`ClassInfo`",
                "`std::type_info`",
                "`invoke`",
                "`landingpad`",
            ),
        }
        architecture_paths = (
            Path("docs/architecture.md"),
            *(Path(f"docs/architecture.{locale}.md") for locale in i18n.LOCALES),
        )
        self.assertEqual(i18n.ARCHITECTURE_CAPABILITY_TOKENS, capability_tokens)
        self.assertEqual(i18n.ARCHITECTURE_DOCS, architecture_paths)

        all_tokens = tuple(
            token for tokens in capability_tokens.values() for token in tokens
        )
        complete_text = "\n".join(all_tokens)
        for path in architecture_paths:
            for capability_id, tokens in capability_tokens.items():
                for missing_token in tokens:
                    with self.subTest(
                        path=path,
                        capability_id=capability_id,
                        missing_token=missing_token,
                    ):
                        texts = dict.fromkeys(architecture_paths, complete_text)
                        texts[path] = "\n".join(
                            token for token in all_tokens if token != missing_token
                        )
                        errors: list[str] = []
                        i18n.validate_architecture_semantics(
                            errors, _PathTextView(texts)
                        )
                        self.assertEqual(
                            errors,
                            [
                                f"{path}: architecture capability "
                                f"{capability_id!r} missing required token "
                                f"{missing_token!r}"
                            ],
                        )

    def test_matrix_runs_the_architecture_semantic_guard(self) -> None:
        path = Path("docs/architecture.zh-CN.md")
        token = "`neverd translate-object`"
        text = i18n.RepositoryView(use_index=False).read_text(path)
        self.assertIn(token, text)
        errors: list[str] = []

        i18n.validate_matrix(errors, _OverlayView({path: text.replace(token, "")}))

        self.assertEqual(
            errors,
            [
                f"{path}: architecture capability 'translation.runtime-contract' "
                f"missing required token {token!r}"
            ],
        )

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
