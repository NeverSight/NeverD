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
        self._base = i18n.RepositoryView(use_index=False)

    def read_text(self, path: Path) -> str:
        if path in self._texts:
            return self._texts[path]
        return self._base.read_text(path)


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
    @staticmethod
    def _sbf_evidence_tokens() -> tuple[tuple[str, ...], tuple[str, ...]]:
        errors: list[str] = []
        view = i18n.RepositoryView(use_index=False)
        guide = i18n.sbf_guide_evidence_tokens(errors, view)
        testing = i18n.sbf_testing_evidence_tokens(errors, view)
        if errors:
            raise AssertionError(errors)
        return guide, testing

    def test_every_public_document_family_is_registered(self) -> None:
        self.assertEqual(
            set(i18n.ENGLISH_DOCS),
            {
                Path("README.md"),
                Path("CONTRIBUTING.md"),
                Path("docs/README.md"),
                Path("docs/architecture.md"),
                Path("docs/memory-safety.md"),
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
                                f"{path.as_posix()}: architecture capability "
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

        self.assertIn(
            f"{path.as_posix()}: architecture capability "
            f"'translation.runtime-contract' "
            f"missing required token {token!r}",
            errors,
        )

    def test_sbf_evidence_guard_rejects_each_stale_provenance_token(self) -> None:
        guide_tokens, testing_tokens = self._sbf_evidence_tokens()
        complete_guide = "\n".join(guide_tokens)
        complete_testing = "\n".join(testing_tokens)
        paths = (*i18n.SBF_GUIDE_DOCS, *i18n.SBF_TESTING_DOCS)

        for path in paths:
            base = complete_guide if path in i18n.SBF_GUIDE_DOCS else complete_testing
            for stale_token in i18n.SBF_STALE_EVIDENCE_TOKENS:
                with self.subTest(path=path, stale_token=stale_token):
                    texts = {
                        candidate: (
                            complete_guide
                            if candidate in i18n.SBF_GUIDE_DOCS
                            else complete_testing
                        )
                        for candidate in paths
                    }
                    texts[path] = f"{base}\n{stale_token}\n"
                    errors: list[str] = []
                    i18n.validate_sbf_evidence(errors, _PathTextView(texts))
                    self.assertEqual(
                        errors,
                        [
                            f"{path.as_posix()}: stale SBF evidence token "
                            f"{stale_token!r}"
                        ],
                    )

    def test_sbf_guide_tokens_cover_observable_runtime_feature_contract(self) -> None:
        guide_tokens, _testing_tokens = self._sbf_evidence_tokens()
        required = {
            "RuntimeFeatureMask",
            "RuntimeFeatureDisposition",
            "FeatureSnapshot",
            "SBFAnalysisLimits.def",
            "SBFOfficialExecutionConstants.def",
            "CPI/PDA",
            "MaxModeledScratchBytes",
            "1,024",
            "ScratchFlowRetainedByteBudget",
            "8,388,608",
            "ScratchRecoveryPrecision::BlockLocal",
            "recovery scratch-precision=block-local",
            "disable_deploy_of_alloc_free_syscall",
            "simplify_alt_bn128_syscall_error_codes",
            "abort_on_invalid_curve",
            "deplete_cu_meter_on_vm_failure",
            "fix_alt_bn128_multiplication_input_length",
            "raise_cpi_nesting_limit_to_8",
            "increase_cpi_account_info_limit",
            "poseidon_enforce_padding",
            "fix_alt_bn128_pairing_length_check",
            "alt_bn128_little_endian",
            "enable_alt_bn128_g2_syscalls",
            "ezBPF",
            "88829078a6d7682a2baed0d696d500401c263750",
            "SBFOfficialExecutionConstants.def",
            "r2ghidra-solana",
            "radare2-solana",
            "SBPF-3-1",
            "eca0b8e2d307e00991e289b8f9b0f45743819f1b",
            "292d845681be377cadc9959a74c2cadeb6e7f412",
            "0e602c93007faa96bccb8e1e12040954ff108b6f",
            "C-like-pdg",
            "38",
            "89",
            "441305159",
            "433055669",
            "487238699",
            "VirtualAddressSpaceAdjustments",
        }

        self.assertLessEqual(required, set(guide_tokens))
        self.assertLessEqual(
            set(i18n.SBF_SCRATCH_PROSE_MARKERS),
            set(i18n.SBF_GUIDE_EVIDENCE_TOKENS),
        )
        for natural_language_fragment in (
            "scratch consumer",
            "whole-CFG fixed point",
            "analysis policy",
            "protocol limits",
            "program point",
            "logical retained estimate",
            "block-local replay",
            "sound",
            "same-block stores",
            "half-converged must-facts",
            "strict ELF differential",
            "CPI target built-in",
            "external runtime",
            "RPC activation audit",
        ):
            self.assertNotIn(natural_language_fragment, i18n.SBF_GUIDE_EVIDENCE_TOKENS)

    def test_sbf_execution_matrix_tokens_preserve_separate_case_totals(self) -> None:
        required = {
            "(Version,Opcode)",
            "508",
            "58",
            "566",
            "1,411",
            "41",
        }
        self.assertEqual(set(i18n.SBF_EXECUTION_MATRIX_MARKERS), required)
        self.assertIn("508", required)
        self.assertIn("58", required)
        self.assertIn("566", required)
        self.assertIn("1,411", required)
        self.assertIn("41", required)
        self.assertNotIn("41-case", i18n.SBF_EXECUTION_MATRIX_MARKERS)

    def test_sbf_evidence_guard_rejects_missing_stable_tokens(self) -> None:
        guide_tokens, testing_tokens = self._sbf_evidence_tokens()
        complete_guide = "\n".join(guide_tokens)
        complete_testing = "\n".join(testing_tokens)
        paths = (*i18n.SBF_GUIDE_DOCS, *i18n.SBF_TESTING_DOCS)

        families = (
            (i18n.SBF_GUIDE_DOCS, guide_tokens, complete_guide),
            (
                i18n.SBF_TESTING_DOCS,
                testing_tokens,
                complete_testing,
            ),
        )
        for family_paths, required_tokens, complete_text in families:
            for path in family_paths:
                for missing_token in required_tokens:
                    with self.subTest(path=path, missing_token=missing_token):
                        texts = {
                            candidate: (
                                complete_guide
                                if candidate in i18n.SBF_GUIDE_DOCS
                                else complete_testing
                            )
                            for candidate in paths
                        }
                        texts[path] = "\n".join(
                            line
                            for line in complete_text.splitlines()
                            if line != missing_token
                        )
                        errors: list[str] = []
                        i18n.validate_sbf_evidence(errors, _PathTextView(texts))
                        self.assertEqual(
                            errors,
                            [
                                f"{path.as_posix()}: missing required token "
                                f"{missing_token!r}"
                            ],
                        )

    def test_sbf_production_authority_supplies_pins_and_protocol_limits(
        self,
    ) -> None:
        guide_tokens, testing_tokens = self._sbf_evidence_tokens()
        expected = {
            "2510663bb8d894e8e3094be351e4bb4b604f1f84",
            "ef210d67f2fabeee1730498188fa78854260c679",
            "122f32e571ce39face4beffaccea733e37c207fd",
            "68bb4af40235562e8852fa23d5727e49c2a0b862",
            "10'485'760",
            "65,536",
            "1,310,720",
        }
        self.assertLessEqual(expected, set(guide_tokens))
        self.assertLessEqual(expected - {"1,310,720"}, set(testing_tokens))
        self.assertNotIn("1,310,720", testing_tokens)

        checker_source = Path(i18n.__file__).read_text(encoding="utf-8")
        for token in expected:
            self.assertNotIn(f'"{token}"', checker_source)

    def test_sbf_authority_parser_follows_typed_def_values(self) -> None:
        upstream_revision = "a" * 40
        view = _PathTextView(
            {
                i18n.SBF_UPSTREAM_SOURCES_PATH: (
                    'SBF_UPSTREAM_SOURCE(Example, "example",\n'
                    f'                    "{upstream_revision}")\n'
                ),
                i18n.SBF_PROTOCOL_LIMITS_PATH: (
                    "SBF_PROTOCOL_LIMIT(InstructionByteCount, 8, Example)\n"
                    "SBF_PROTOCOL_LIMIT(LegacyProgramInstructionCount, "
                    "1'234, Example)\n"
                    "SBF_PROTOCOL_LIMIT(MaxProgramAccountDataSize, "
                    "5'678'912, Example)\n"
                ),
            }
        )
        errors: list[str] = []
        tokens = i18n.sbf_authority_evidence_tokens(errors, view)
        guide_tokens = i18n.sbf_guide_authority_evidence_tokens(errors, view)
        self.assertEqual(errors, [])
        self.assertEqual(
            tokens,
            (upstream_revision, "5'678'912", "1,234"),
        )
        self.assertEqual(
            guide_tokens,
            (upstream_revision, "5'678'912", "1,234", "709,864"),
        )

    def test_sbf_upstream_authority_rejects_a_truncated_revision(self) -> None:
        view = _PathTextView(
            {
                i18n.SBF_UPSTREAM_SOURCES_PATH: (
                    'SBF_UPSTREAM_SOURCE(Example, "example", "' + "a" * 39 + '")\n'
                )
            }
        )
        errors: list[str] = []

        revisions = i18n.sbf_upstream_source_revisions(errors, view)

        self.assertEqual(revisions, {})
        self.assertTrue(
            any("malformed SBF_UPSTREAM_SOURCE row" in error for error in errors),
            errors,
        )

    def test_sbf_protocol_authority_rejects_an_unknown_source(self) -> None:
        revision = "a" * 40
        view = _PathTextView(
            {
                i18n.SBF_UPSTREAM_SOURCES_PATH: (
                    f'SBF_UPSTREAM_SOURCE(Example, "example", "{revision}")\n'
                ),
                i18n.SBF_PROTOCOL_LIMITS_PATH: (
                    "SBF_PROTOCOL_LIMIT(InstructionByteCount, 8, Typo)\n"
                ),
            }
        )
        errors: list[str] = []
        revisions = i18n.sbf_upstream_source_revisions(errors, view)

        limits = i18n.sbf_protocol_limits(errors, view, revisions)

        self.assertEqual(limits, {"InstructionByteCount": ("8", "Typo")})
        self.assertTrue(
            any("references unknown source 'Typo'" in error for error in errors),
            errors,
        )

    def test_sbf_analysis_budget_parser_follows_typed_def_values(self) -> None:
        view = _PathTextView(
            {
                i18n.SBF_ANALYSIS_LIMITS_PATH: (
                    "SBF_ANALYSIS_LIMIT(MaxModeledScratchBytes, 2'048)\n"
                    "SBF_ANALYSIS_LIMIT(ScratchFlowRetainedByteBudget, "
                    "16'777'216)\n"
                )
            }
        )
        errors: list[str] = []

        tokens = i18n.sbf_analysis_evidence_tokens(errors, view)

        self.assertEqual(errors, [])
        self.assertEqual(tokens, ("2,048", "16,777,216"))
        checker_source = Path(i18n.__file__).read_text(encoding="utf-8")
        self.assertNotIn('"1,024"', checker_source)
        self.assertNotIn('"8,388,608"', checker_source)

    def test_sbf_c_status_parser_follows_typed_def_values(self) -> None:
        view = _PathTextView(
            {
                i18n.SBF_SOURCE_STATUSES_PATH: (
                    "SBF_SOURCE_SUCCESS(Ok, None, STATUS_OK, 3)\n"
                    "SBF_SOURCE_C_V1_ERROR(Bad)\n"
                    "SBF_SOURCE_ERROR(Bad, Bad, STATUS_BAD, 7, 0)\n"
                    "SBF_SOURCE_ERROR(New, New, STATUS_NEW, 11, 1)\n"
                )
            }
        )
        errors: list[str] = []

        v1_body, v2_body = i18n.sbf_c_status_bodies(errors, view)

        self.assertEqual(errors, [])
        self.assertEqual(v1_body, "STATUS_OK = 3,\nSTATUS_BAD = 7,")
        self.assertEqual(v2_body, "STATUS_NEW = 11,")
        checker_source = Path(i18n.__file__).read_text(encoding="utf-8")
        self.assertNotIn('"NEVERD_SBF_OK = 0,"', checker_source)
        self.assertNotIn('"NEVERD_SBF_INVALID_BRANCH = 10,"', checker_source)

    def test_sbf_comparison_authority_is_not_hardcoded_in_checker(self) -> None:
        errors: list[str] = []
        tools = i18n.sbf_comparison_tools(errors, i18n.RepositoryView(use_index=False))
        self.assertEqual(errors, [])

        checker_source = Path(i18n.__file__).read_text(encoding="utf-8")
        for _tool_id, display_name, revision in tools:
            with self.subTest(display_name=display_name):
                self.assertNotIn(f'"{display_name}"', checker_source)
                self.assertNotIn(f'"{revision}"', checker_source)

    def test_sbf_comparison_tool_parser_follows_typed_def_values(self) -> None:
        first_revision = "a" * 40
        second_revision = "b" * 40
        view = _PathTextView(
            {
                i18n.SBF_COMPARISON_TOOLS_PATH: (
                    "// Rows may span lines, as LLVM-style .def files do.\n"
                    'SBF_COMPARISON_TOOL(First, "First tool",\n'
                    f'                    "{first_revision}")\n'
                    'SBF_COMPARISON_TOOL(Second, "Second tool", '
                    f'"{second_revision}")\n'
                )
            }
        )
        errors: list[str] = []

        tools = i18n.sbf_comparison_tools(errors, view)

        self.assertEqual(errors, [])
        self.assertEqual(
            tools,
            (
                ("First", "First tool", first_revision),
                ("Second", "Second tool", second_revision),
            ),
        )

    def test_sbf_comparison_authority_mutation_invalidates_every_guide(
        self,
    ) -> None:
        base = i18n.RepositoryView(use_index=False)
        source = base.read_text(i18n.SBF_COMPARISON_TOOLS_PATH)
        parse_errors: list[str] = []
        tools = i18n.sbf_comparison_tools(parse_errors, base)
        self.assertEqual(parse_errors, [])
        self.assertTrue(tools)

        _tool_id, display_name, _revision = tools[0]
        mutated_name = f"{display_name}-authority-mutation"
        mutated_source = source.replace(f'"{display_name}"', f'"{mutated_name}"', 1)
        self.assertNotEqual(mutated_source, source)

        errors: list[str] = []
        i18n.validate_sbf_evidence(
            errors,
            _OverlayView({i18n.SBF_COMPARISON_TOOLS_PATH: mutated_source}),
        )

        self.assertEqual(
            errors,
            [
                f"{path.as_posix()}: missing required token {mutated_name!r}"
                for path in i18n.SBF_GUIDE_DOCS
            ],
        )

    def test_sbf_comparison_authority_rejects_unknown_active_rows(self) -> None:
        base = i18n.RepositoryView(use_index=False)
        source = base.read_text(i18n.SBF_COMPARISON_TOOLS_PATH)
        mutated_source = source.replace(
            "SBF_COMPARISON_TOOL(Blueshift,",
            "SBF_COMPARISON_TOO(Blueshift,",
            1,
        )
        self.assertNotEqual(mutated_source, source)

        errors: list[str] = []
        i18n.sbf_comparison_tools(
            errors,
            _OverlayView({i18n.SBF_COMPARISON_TOOLS_PATH: mutated_source}),
        )

        self.assertEqual(len(errors), 1)
        self.assertIn("unknown active content", errors[0])
        self.assertIn("SBF_COMPARISON_TOO", errors[0])

    def test_sbf_comparison_authority_keeps_tool_revision_pairs(self) -> None:
        base = i18n.RepositoryView(use_index=False)
        source = base.read_text(i18n.SBF_COMPARISON_TOOLS_PATH)
        parse_errors: list[str] = []
        tools = i18n.sbf_comparison_tools(parse_errors, base)
        self.assertEqual(parse_errors, [])
        self.assertGreaterEqual(len(tools), 2)
        first = tools[0]
        second = tools[1]
        placeholder = "f" * 40
        swapped_source = (
            source.replace(first[2], placeholder, 1)
            .replace(second[2], first[2], 1)
            .replace(placeholder, second[2], 1)
        )

        errors: list[str] = []
        i18n.validate_sbf_evidence(
            errors,
            _OverlayView({i18n.SBF_COMPARISON_TOOLS_PATH: swapped_source}),
        )

        pair_errors = [
            error for error in errors if "is not paired with revision" in error
        ]
        self.assertEqual(len(pair_errors), 2 * len(i18n.SBF_GUIDE_DOCS))
        for path in i18n.SBF_GUIDE_DOCS:
            self.assertTrue(
                any(
                    path.as_posix() in error and first[1] in error
                    for error in pair_errors
                )
            )
            self.assertTrue(
                any(
                    path.as_posix() in error and second[1] in error
                    for error in pair_errors
                )
            )

    def test_sbf_comparison_pair_rejects_a_correct_pin_hiding_a_wrong_one(
        self,
    ) -> None:
        errors: list[str] = []
        tools = i18n.sbf_comparison_tools(errors, i18n.RepositoryView(use_index=False))
        self.assertEqual(errors, [])
        first = tools[0]
        wrong_revision = "0" * 40
        self.assertNotEqual(wrong_revision, first[2])
        text = (
            f"- {first[1]} at {wrong_revision}\n"
            "  This is a deliberately wrong local claim.\n\n"
            f"- {first[1]} at {first[2]}\n"
        )

        self.assertFalse(
            i18n.comparison_tool_revision_is_paired(
                text,
                first[1],
                first[2],
                tuple(tool[1] for tool in tools),
            )
        )

    def test_sbf_testing_row_guard_rejects_missing_target_or_artifact_marker(
        self,
    ) -> None:
        complete_row = (
            "| `unittests/sbf` | "
            + " ".join(i18n.SBF_TESTING_TARGET_TOKENS)
            + " | "
            + " ".join(i18n.SBF_TESTING_ARTIFACT_MARKERS)
            + " |\n"
        )
        path = i18n.SBF_TESTING_DOCS[0]
        texts = {candidate: complete_row for candidate in i18n.SBF_TESTING_DOCS}

        missing_target = i18n.SBF_TESTING_TARGET_TOKENS[-1]
        texts[path] = complete_row.replace(missing_target, "")
        errors: list[str] = []
        i18n.validate_sbf_testing_rows(errors, _PathTextView(texts))
        self.assertEqual(
            errors,
            [f"{path.as_posix()}: SBF testing row missing target {missing_target!r}"],
        )

        for missing_marker in i18n.SBF_TESTING_ARTIFACT_MARKERS:
            with self.subTest(missing_marker=missing_marker):
                texts[path] = complete_row.replace(missing_marker, "")
                errors = []
                i18n.validate_sbf_testing_rows(errors, _PathTextView(texts))
                self.assertEqual(
                    errors,
                    [
                        f"{path.as_posix()}: SBF testing row missing artifact "
                        f"marker {missing_marker!r}"
                    ],
                )

    def test_sbf_host_api_guard_rejects_missing_generated_symbol(self) -> None:
        complete_text = "\n".join(i18n.SBF_HOST_API_TOKENS)
        path = i18n.SBF_GUIDE_DOCS[0]
        texts = {candidate: complete_text for candidate in i18n.SBF_GUIDE_DOCS}
        for missing_token in (
            "neverd_sbf_environment_v2",
            "let _ = (hash, args);",
        ):
            with self.subTest(missing_token=missing_token):
                texts[path] = complete_text.replace(missing_token, "")
                errors: list[str] = []
                i18n.validate_sbf_host_api(errors, _OverlayView(texts))
                self.assertEqual(
                    errors,
                    [f"{path.as_posix()}: missing required token {missing_token!r}"],
                )

    def test_sbf_c_status_guard_rejects_mixed_v1_v2_domains(self) -> None:
        path = i18n.SBF_GUIDE_DOCS[0]
        source = i18n.RepositoryView(use_index=False).read_text(path)
        cases = (
            (
                source.replace(
                    "  NEVERD_SBF_EXECUTION_OVERRUN = 8,\n} neverd_sbf_status;",
                    "  NEVERD_SBF_EXECUTION_OVERRUN = 8,\n"
                    "  NEVERD_SBF_INVALID_REGISTER = 9,\n"
                    "} neverd_sbf_status;",
                    1,
                ),
                "C v1 status enum must match SBFSourceStatuses.def",
            ),
            (
                source.replace(
                    "  NEVERD_SBF_INVALID_BRANCH = 10,\n};",
                    "  NEVERD_SBF_INVALID_BRANCH = 11,\n};",
                    1,
                ),
                "C v2 status extensions must match SBFSourceStatuses.def",
            ),
        )
        for broken_text, message in cases:
            with self.subTest(message=message):
                errors: list[str] = []
                v1_body, v2_body = i18n.sbf_c_status_bodies(
                    errors, i18n.RepositoryView(use_index=False)
                )
                self.assertEqual(errors, [])
                i18n.validate_sbf_c_status_contract(
                    errors, path, broken_text, v1_body, v2_body
                )
                self.assertEqual(errors, [f"{path.as_posix()}: {message}"])

    def test_sbf_c_host_contract_uses_language_neutral_semantic_markers(self) -> None:
        self.assertEqual(
            tuple(group[0] for group in i18n.SBF_C_PROSE_MARKER_GROUPS),
            (
                "v1-load-store-nonzero",
                "v1-syscall-nonzero",
                "internal-invalid-instruction",
                "v2-exact-status",
                "operation-specific-fallback",
                "feature-aware-null-base-syscall",
            ),
        )

    def test_sbf_rust_prose_mutation_is_wired_into_matrix(self) -> None:
        path = Path("docs/sbf.zh-CN.md")
        source = i18n.RepositoryView(use_index=False).read_text(path)
        marker = "`v1-result-abi`"
        self.assertEqual(source.count(marker), 1)
        broken = source.replace(marker, "", 1)
        errors: list[str] = []
        i18n.validate_matrix(errors, _OverlayView({path: broken}))
        self.assertEqual(
            [error for error in errors if error.startswith(f"{path.as_posix()}:")],
            [f"{path.as_posix()}: Rust host prose missing required marker {marker!r}"],
        )

    def test_sbf_conformance_command_mutation_is_wired_into_matrix(self) -> None:
        path = Path("docs/sbf.zh-CN.md")
        source = i18n.RepositoryView(use_index=False).read_text(path)
        line = i18n.SBF_CONFORMANCE_COMMAND_LINES[1]
        self.assertEqual(source.count(line), 1)
        broken = source.replace(line + "\n", "", 1)
        errors: list[str] = []
        i18n.validate_matrix(errors, _OverlayView({path: broken}))
        self.assertEqual(
            [error for error in errors if error.startswith(f"{path.as_posix()}:")],
            [f"{path.as_posix()}: missing exact SBF conformance command block"],
        )

    def test_sbf_ownership_section_mutation_is_wired_into_matrix(self) -> None:
        path = Path("docs/testing.zh-CN.md")
        source = i18n.RepositoryView(use_index=False).read_text(path)
        headings = list(
            i18n.re.finditer(r"^(#{1,6})\s+([^\n]+)\n", source, flags=i18n.re.MULTILINE)
        )
        ownership_paragraph = None
        for index, heading in enumerate(headings):
            if len(heading.group(1)) != 3:
                continue
            end = len(source)
            for following in headings[index + 1 :]:
                if len(following.group(1)) <= 3:
                    end = following.start()
                    break
            body = i18n.without_markdown_fences(source[heading.end() : end])
            ownership_paragraph = next(
                (
                    paragraph
                    for paragraph in body.split("\n\n")
                    if "NeverDSBFISAConformanceTests" in paragraph
                    and "NeverDSBFUpstreamConformanceTests" in paragraph
                ),
                None,
            )
            if ownership_paragraph is not None:
                break
        self.assertIsNotNone(ownership_paragraph)
        broken = source.replace(ownership_paragraph, "", 1)
        errors: list[str] = []
        i18n.validate_matrix(errors, _OverlayView({path: broken}))
        self.assertEqual(
            [error for error in errors if error.startswith(f"{path.as_posix()}:")],
            [f"{path.as_posix()}: missing SBF testing ownership section"],
        )

    def test_sbf_c_host_prose_mutation_cannot_be_satisfied_by_code(self) -> None:
        path = Path("docs/sbf.zh-CN.md")
        source = i18n.RepositoryView(use_index=False).read_text(path)
        heading = i18n.re.search(
            r"^## [^\n]*C[^\n]*host[^\n]*\n",
            source,
            flags=i18n.re.IGNORECASE | i18n.re.MULTILINE,
        )
        self.assertIsNotNone(heading)
        next_heading = i18n.re.search(
            r"^## ", source[heading.end() :], flags=i18n.re.MULTILINE
        )
        end = (
            heading.end() + next_heading.start()
            if next_heading is not None
            else len(source)
        )
        section = source[heading.end() : end]
        prose = i18n.without_markdown_fences(section)
        semantic_paragraph = next(
            paragraph
            for paragraph in prose.split("\n\n")
            if all(
                token.casefold() in paragraph.casefold()
                for token in i18n.SBF_C_PROSE_MARKER_GROUPS[0]
            )
        )
        self.assertIn("NEVERD_SBF_MEMORY_ACCESS", semantic_paragraph)
        # Remove the prose paragraph while leaving the generated code intact;
        # the validator must not accept fenced API tokens as its evidence.
        broken_section = section.replace(semantic_paragraph, "", 1)
        broken = source[: heading.end()] + broken_section + source[end:]
        errors: list[str] = []
        i18n.validate_matrix(errors, _OverlayView({path: broken}))
        c_errors = [
            error
            for error in errors
            if error.startswith(f"{path.as_posix()}: C host prose")
        ]
        self.assertTrue(c_errors)

    def test_sbf_c_idl_setter_must_stay_inside_the_c_fence(self) -> None:
        path = Path("docs/sbf.zh-CN.md")
        source = i18n.RepositoryView(use_index=False).read_text(path)
        setter = "neverd_sbf_set_idl(session, idl_json);"
        self.assertEqual(source.count(setter), 1)
        broken = source.replace(setter, "", 1)
        errors: list[str] = []
        i18n.validate_sbf_c_api_examples(errors, _OverlayView({path: broken}))
        self.assertEqual(
            errors,
            [f"{path.as_posix()}: C host example missing fenced IDL setter"],
        )

    def test_sbf_scratch_policy_mutation_rejects_missing_stable_marker(self) -> None:
        path = Path("docs/sbf.zh-CN.md")
        source = i18n.RepositoryView(use_index=False).read_text(path)
        marker = "ScratchFlowRetainedByteBudget"
        self.assertEqual(source.count(marker), 1)
        broken = source.replace(marker, "", 1)
        errors: list[str] = []
        i18n.validate_sbf_scratch_prose(errors, _OverlayView({path: broken}))
        self.assertEqual(
            errors,
            [f"{path.as_posix()}: scratch policy markers are not co-located"],
        )

    def test_sbf_testing_evidence_requires_accept_reject_counts_together(self) -> None:
        path = Path("docs/testing.zh-CN.md")
        source = i18n.RepositoryView(use_index=False).read_text(path)
        marker = "1,399"
        self.assertEqual(source.count(marker), 1)
        broken = source.replace(marker, "", 1)
        errors: list[str] = []
        i18n.validate_sbf_testing_evidence_prose(errors, _OverlayView({path: broken}))
        self.assertEqual(
            errors,
            [
                f"{path.as_posix()}: SBF evidence prose lacks one paragraph with "
                "Agave ownership, fixture identity, and all ELF evidence fields"
            ],
        )

    def test_sbf_execution_matrix_accepts_wrapped_localized_prose_and_rejects_missing_41(
        self,
    ) -> None:
        complete = (
            "508 active `(Version,Opcode)` cases plus 58 boundary cases = 566 "
            "exact execution cases; 1,411 verifier probes and 41-case strict\n"
            "ELF differential remain separate."
        )
        texts = {
            path: complete for path in (*i18n.SBF_GUIDE_DOCS, *i18n.SBF_TESTING_DOCS)
        }
        errors: list[str] = []
        i18n.validate_sbf_execution_matrix_structure(errors, _PathTextView(texts))
        self.assertEqual(errors, [])

        broken_path = i18n.SBF_GUIDE_DOCS[1]
        texts[broken_path] = complete.replace("41-case", "")
        errors = []
        i18n.validate_sbf_execution_matrix_structure(errors, _PathTextView(texts))
        self.assertEqual(
            errors,
            [f"{broken_path.as_posix()}: execution matrix totals are not co-located"],
        )

    def test_sbf_evidence_table_rejects_a_row_after_intervening_prose(self) -> None:
        valid = (
            "| Evidence | Contract |\n"
            "|----------|----------|\n"
            "| First | checked |\n"
            "| Second | checked |\n"
        )
        broken_path = i18n.SBF_GUIDE_DOCS[0]
        texts = {path: valid for path in i18n.SBF_GUIDE_DOCS}
        texts[broken_path] = valid + "\nprose interruption\n\n| Orphan | row |\n"
        errors: list[str] = []
        i18n.validate_sbf_evidence_table_continuity(errors, _PathTextView(texts))
        self.assertEqual(
            errors,
            [
                f"{broken_path.as_posix()}:8: evidence table row is not contiguous "
                "with a Markdown header or preceding row"
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
        workflow = (i18n.REPO_ROOT / ".github" / "workflows" / "ci.yml").read_text(
            encoding="utf-8"
        )
        self.assertIn('"$PYTHON" scripts/check_docs_i18n.py', workflow)


if __name__ == "__main__":
    unittest.main()
