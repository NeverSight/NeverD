#!/usr/bin/env python3
"""Validate NeverD's localized documentation matrix and Markdown links."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import unicodedata
from collections import defaultdict
from pathlib import Path
from urllib.parse import unquote


REPO_ROOT = Path(__file__).resolve().parents[1]
LOCALES = (
    "ar",
    "de",
    "es",
    "fr",
    "it",
    "ja",
    "ko",
    "ru",
    "zh-CN",
    "zh-TW",
)
ARCHITECTURE_DOCS = (
    Path("docs/architecture.md"),
    *(Path(f"docs/architecture.{locale}.md") for locale in LOCALES),
)
ARCHITECTURE_CAPABILITY_TOKENS = {
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
GUIDE_STEMS = ("evm", "sbf")
SBF_GUIDE_DOCS = (
    Path("docs/sbf.md"),
    *(Path(f"docs/sbf.{locale}.md") for locale in LOCALES),
)
SBF_TESTING_DOCS = (
    Path("docs/testing.md"),
    *(Path(f"docs/testing.{locale}.md") for locale in LOCALES),
)
SBF_UPSTREAM_SOURCES_PATH = Path("include/neverd/sbf/runtime/SBFUpstreamSources.def")
SBF_PROTOCOL_LIMITS_PATH = Path("include/neverd/sbf/SBFProtocolLimits.def")
SBF_ANALYSIS_LIMITS_PATH = Path("include/neverd/sbf/analysis/SBFAnalysisLimits.def")
SBF_SOURCE_STATUSES_PATH = Path("include/neverd/sbf/emit/SBFSourceStatuses.def")
SBF_COMPARISON_TOOLS_PATH = Path("unittests/sbf/SBFComparisonTools.def")
SBF_GUIDE_EVIDENCE_TOKENS = (
    "2026-08-24",
    "23/23",
    "(Version,Opcode)",
    "508",
    "58",
    "566",
    "1,411",
    "1,955",
    "RuntimeVersionPolicy::ChainProfile",
    "RuntimeVersionPolicy::UpstreamToolchain",
    "SBF_RUNTIME_VERSION",
    "RuntimeFeatureMask",
    "RuntimeFeatureDisposition",
    "RuntimeBranch",
    "FoldedBranch",
    "FeatureSnapshot",
    "syscall_parameter_address_restrictions",
    "account_data_direct_mapping",
    "disable_deploy_of_alloc_free_syscall",
    "enable_bpf_loader_set_authority_checked_ix",
    "remove_bpf_loader_incorrect_program_id",
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
    "loader_v3_minimum_extend_program_size",
    "SBFValidationRules.def",
    "SBFFaultCodes.def",
    "SBFSourceStatuses.def",
    "SBFEdgeKinds.def",
    "SBFAnalysisLimits.def",
    "callgraph-budget=fail-closed",
    "CallGraphOutputByteBudget",
    '{"nodes":[],"edges":[]}',
    "neverd_last_error()",
    "CPI/PDA",
    "MaxModeledScratchBytes",
    "ScratchFlowRetainedByteBudget",
    "ScratchRecoveryPrecision::BlockLocal",
    "recovery scratch-precision=block-local",
    "SBFOfficialOracleProtocol.def",
    "SBFOfficialVerifierCases.def",
    "SBFOfficialELFMutations.def",
    "SBFOfficialExecutionConstants.def",
    "SBFComparisonTools.def",
    "NeverDSBFExternalOracleTests",
    "NeverDSBFAgaveConformanceTests",
    "--print-pinned-revision",
    "--print-test-vectors-revision",
    "--print-toolchain",
    "NEVERD_SBPF_ORACLE",
    "NEVERD_AGAVE_CONFORMANCE_ROOT",
    "NEVERD_AGAVE_CONFORMANCE_REVISION",
    "raw-first",
    "10,000",
    "C-like-pdg",
    "38",
    "89",
    "441305159",
    "433055669",
    "487238699",
    "VirtualAddressSpaceAdjustments",
)
SBF_TESTING_EVIDENCE_TOKENS = (
    "2026-08-24",
    "23/23",
    "(Version,Opcode)",
    "508",
    "58",
    "566",
    "1,411",
    "1,955",
    "RuntimeVersionPolicy::ChainProfile",
    "RuntimeVersionPolicy::UpstreamToolchain",
    "SBF_RUNTIME_VERSION",
    "SBFFaultCodes.def",
    "SBFSourceStatuses.def",
    "SBFOfficialOracleProtocol.def",
    "SBFOfficialVerifierCases.def",
    "SBFOfficialELFMutations.def",
    "SBFOfficialExecutionConstants.def",
    "NeverDSBFExternalOracleTests",
    "NeverDSBFAgaveConformanceTests",
    "--print-pinned-revision",
    "--print-test-vectors-revision",
    "--print-toolchain",
    "NEVERD_SBPF_ORACLE",
    "NEVERD_AGAVE_CONFORMANCE_ROOT",
    "NEVERD_AGAVE_CONFORMANCE_REVISION",
    "sol_compat_elf_loader_v1",
    "10,000",
)
SBF_TESTING_TARGET_TOKENS = (
    "NeverDSBFMetadataTests",
    "NeverDSBFProgramImageTests",
    "NeverDSBFLoaderTests",
    "NeverDSBFAnalyzerTests",
    "NeverDSBFVerifierTests",
    "NeverDSBFISAConformanceTests",
    "NeverDSBFAgaveConformanceTests",
    "NeverDSBFSemanticTests",
    "NeverDSBFEmitterTests",
    "NeverDSBFLLVMEmitterTests",
    "NeverDSBFLLVMDifferentialTests",
    "NeverDSBFSourceDifferentialTests",
    "NeverDSBFMalformedCorpusTests",
    "NeverDSBFUpstreamConformanceTests",
    "NeverDSBFExternalOracleTests",
    "NeverDSBFSolanaModelTests",
    "NeverDSBFIntegrationTests",
)
SBF_TESTING_ARTIFACT_MARKERS = ("23", "ELF")
SBF_HOST_API_TOKENS = (
    "#include <stdint.h>",
    "typedef enum neverd_sbf_status",
    "typedef uint32_t neverd_sbf_status_v2;",
    "neverd_sbf_status",
    "neverd_sbf_status_v2 neverd_sbf_program_v2",
    "NEVERD_SBF_INVALID_BRANCH",
    "neverd_sbf_environment",
    "neverd_sbf_environment_v2",
    "neverd_sbf_syscall_invocation",
    "NEVERD_SBF_RUNTIME_FEATURE_",
    "base.syscall",
    "syscall_with_features",
    "runtime_features",
    "SbfEnvironment",
    "SbfEnvironmentV2",
    "SbfError",
    "SbfErrorV2",
    "non_exhaustive",
    "SbfRuntimeFeatures",
    "SbfSyscallInvocation",
    "SbfSyscallOutcomeV2",
    "syscall_outcome",
    "SbfErrorV2::UnknownSyscall",
    "-> Result<u64, SbfErrorV2> {",
    "let _ = (hash, args);",
    "Some(SbfRuntimeFeatures::from_bits(0))",
    "neverd_sbf_program_v2",
    "neverd_sbf_set_idl(session, idl_json);",
)
SBF_RUST_PROSE_MARKERS = (
    "`neverd_sbf_program`",
    "`SbfEnvironment`",
    "`v1-result-abi`",
    "`Result`",
    "`Some(SbfRuntimeFeatures::from_bits(0))`",
    "`explicit-empty-snapshot`",
    "`syscall_outcome`",
    "`result-host-bridge`",
    "`SbfErrorV2`",
    "`#[non_exhaustive]`",
    "`non-exhaustive-wildcard`",
)
SBF_C_PROSE_MARKER_GROUPS = (
    (
        "v1-load-store-nonzero",
        "NEVERD_SBF_MEMORY_ACCESS",
        "load",
        "store",
    ),
    (
        "v1-syscall-nonzero",
        "NEVERD_SBF_UNKNOWN_SYSCALL",
        "syscall",
    ),
    (
        "internal-invalid-instruction",
        "InvalidRegister",
        "InvalidBranch",
        "NEVERD_SBF_INVALID_INSTRUCTION",
    ),
    (
        "v2-exact-status",
        "neverd_sbf_program_v2",
        "neverd_sbf_status_v2",
    ),
    (
        "operation-specific-fallback",
        "fallback",
        "neverd_sbf_program_v2",
    ),
    (
        "feature-aware-null-base-syscall",
        "syscall_with_features",
        "base.syscall",
        "int",
    ),
)
SBF_SCRATCH_PROSE_MARKERS = (
    "SBFAnalysisLimits.def",
    "MaxModeledScratchBytes",
    "ScratchFlowRetainedByteBudget",
    "ScratchRecoveryPrecision::BlockLocal",
    "recovery scratch-precision=block-local",
)
SBF_CONFORMANCE_COMMAND_LINES = (
    "NEVERD_SBPF_ROOT=/path/to/sbpf \\",
    "NEVERD_AGAVE_CONFORMANCE_ROOT=/path/to/firedancer-test-vectors \\",
    "  cmake --build build --target check-neverd-sbf",
)
SBF_TESTING_OWNERSHIP_MARKERS = (
    (
        "NeverDSBFISAConformanceTests",
        "v0",
        "v4",
        "manifest",
    ),
    (
        "NeverDSBFExternalOracleTests",
        "Anza",
    ),
    (
        "NeverDSBFUpstreamConformanceTests",
        "ELF",
        "Anza",
    ),
)
SBF_DIFFERENTIAL_HEADING_HINTS = (
    "differential",
    "different",
    "differenz",
    "difer",
    "différ",
    "تفاض",
    "差分",
    "차등",
    "дифферен",
)
SBF_TESTING_EVIDENCE_PROSE_MARKERS = (
    "NeverDSBFAgaveConformanceTests",
    "Firedancer",
    "1,955",
    "1,399",
    "556",
    "sol_compat_elf_loader_v1",
    "entry_pc",
    "text_off",
    "text_cnt",
    "rodata_hash",
    "calldests_hash",
)
SBF_TESTING_RELEASE_COMMAND = (
    "cmake --build build-release --target check-neverd-sbf --parallel 4"
)
SBF_EXECUTION_MATRIX_MARKERS = (
    "(Version,Opcode)",
    "508",
    "58",
    "566",
    "1,411",
    "41",
)
SBF_STALE_EVIDENCE_TOKENS = (
    "2026-08-10",
    "71425d0de59e0bff048c6be8f4a8a9bc655916e2",
    "cae40aa610fdbdb313209bc1eec737079eb59688",
    "全部 20 个制品",
    "20/20",
    "typedef uint32_t neverd_sbf_status;",
)


def execution_matrix_marker_present(text: str, marker: str) -> bool:
    """Match the standalone 41 total without treating 1,411 as 41."""
    if marker == "41":
        return re.search(r"(?<![0-9,])41(?![0-9,])", text) is not None
    return marker in text


GUIDE_REQUIRED_TOKENS = {
    "evm": (
        "frontier",
        "fusaka",
        "--language=c",
        "--language=solidity",
        "EVMImmediateKinds.def",
        "EVMDecodeStatuses.def",
        "EVMOpcodes.def",
        "EVMCalls.def",
        "EVMPrecompiles.def",
        "EVMRecoveredFacts.def",
        "EVMMetadataFields.def",
        "EVMBytecodeContainers.def",
        "eip-7702",
        "eip-7951",
        "vyper",
        "EVMUpstreamOpcodePolicy.def",
        "audit_evm_opcode_metadata.py",
        "Instruction.def",
        "TableGen",
        "_BitInt",
        "MaxAbstractValuesPerSlot",
        "MaxStackHeightVariants",
        "Overdefined",
        "Semantics.h",
        "neverd_evm_set_hardfork",
        "NEVERD_OUTPUT_SOLIDITY",
        "Anvil",
    ),
    "sbf": (
        "| v0 |",
        "| v1 |",
        "| v2 |",
        "| v3 |",
        "| v4 |",
        "--language=c",
        "--language=rust",
        "llvm::verifyModule",
        "neverd_sbf_set_version",
        "NEVERD_OUTPUT_RUST",
        "R_BPF_64_64",
        "sol_invoke_signed_rust",
        "Anchor IDL",
        "ProgramImage",
        "SBFArgumentRegisters.def",
        "SBFProtocolLimits.def",
        "SBFUpstreamSources.def",
        "SBFUpstreamManifest.def",
        "SBFUpstreamOpcodes.def",
        "SBFVersionFeatures.def",
        "SBFKnownAddresses.def",
        "SBFAnchorNames.def",
        "SBFAnchorNamespaces.def",
        "SBFAccountLayout.def",
        "SBFRuntimeFeatures.def",
        "SBFLoaders.def",
        "SBFSyscallLifecycle.def",
        "SBFSyscallRegistration.def",
        "--sbf-cluster",
        "neverd_sbf_set_cluster",
        "simd-0321",
        "abi-v0",
        "abi-v1",
        "SBFLints.def",
        "SBFSyscallMemory.def",
        "SBFCPIABI.def",
        "SBFProgramInstructions.def",
        "kMaxModeledScratchBytes",
        "sol_memcpy_",
    ),
}
TESTING_REQUIRED_TOKENS = (
    "git fetch",
    "build/evm-opcode-audit/go-ethereum.git",
    "audit_evm_opcode_metadata.py",
    "--geth-root",
    "EVMUpstreamOpcodePolicy.def",
    "EVMAnalyzer.StackHeightDomain",
    "EVMAnalyzer.WholeProgram",
    "EVMAnalyzer.MediumIR",
    "EVMAnalyzer.HighIR",
    "RecoversStorageAndEventFactsFromTypedOperands",
    "NeverDSBFProgramImageTests",
    "NeverDSBFMalformedCorpusTests",
    "NeverDSBFISAConformanceTests",
    "NeverDSBFUpstreamConformanceTests",
    "NeverDSBFLLVMDifferentialTests",
    "NeverDSBFSourceDifferentialTests",
    "NeverDSBFSolanaModelTests",
    "build-sbf-asan-ubsan",
    "ASAN_OPTIONS",
    "UBSAN_OPTIONS",
)
MEMORY_SAFETY_REQUIRED_TOKENS = (
    "neverd audit",
    "neverd hunt",
    "UNKNOWN",
    "UNSAFE",
    "name_source",
    "`import`",
    "`pdb` / `dwarf` / `map`",
    "SafetySinks.def",
    "SafetySources.def",
    "GetCommandLineA/W",
    "--sinks",
    "--max-paths",
    "--solver-conflicts",
    "capacity_kind",
    "corroboration",
    "neverd_session_audit_json",
    "neverd_session_hunt_json",
)
ENGLISH_DOCS = (
    Path("README.md"),
    Path("CONTRIBUTING.md"),
    Path("docs/README.md"),
    Path("docs/architecture.md"),
    Path("docs/memory-safety.md"),
    Path("docs/python-plugins.md"),
    Path("docs/roadmap/README.md"),
    Path("docs/testing.md"),
    Path("docs/windows-exception-reconstruction.md"),
    *(Path(f"docs/{stem}.md") for stem in GUIDE_STEMS),
)


def localized_paths(locale: str) -> tuple[Path, ...]:
    return (
        Path(f"docs/i18n/README.{locale}.md"),
        Path(f"docs/i18n/CONTRIBUTING.{locale}.md"),
        Path(f"docs/README.{locale}.md"),
        Path(f"docs/architecture.{locale}.md"),
        Path(f"docs/memory-safety.{locale}.md"),
        Path(f"docs/python-plugins.{locale}.md"),
        Path(f"docs/roadmap/README.{locale}.md"),
        Path(f"docs/testing.{locale}.md"),
        Path(f"docs/windows-exception-reconstruction.{locale}.md"),
        Path(f"docs/evm.{locale}.md"),
        Path(f"docs/sbf.{locale}.md"),
    )


LOCALIZED_DOCS = tuple(path for locale in LOCALES for path in localized_paths(locale))
MARKDOWN_DOCS = ENGLISH_DOCS + LOCALIZED_DOCS
PROHIBITED_STAGED_PREFIXES = ("docs/superpowers/",)

LINK_RE = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")
HEADING_RE = re.compile(r"^(#{1,6})\s+(.+?)\s*#*\s*$")
EXPLICIT_ANCHOR_RE = re.compile(
    r"<a\s+(?:[^>]*?\s)?(?:id|name)=[\"']([^\"']+)[\"'][^>]*>",
    re.IGNORECASE,
)
FENCE_RE = re.compile(r"^\s*(`{3,}|~{3,})")
SBF_TESTING_ROW_RE = re.compile(r"^\| `unittests/sbf` \|.*$", re.MULTILINE)


class RepositoryView:
    """Read either the working tree or the exact Git index snapshot."""

    def __init__(self, use_index: bool) -> None:
        self.use_index = use_index
        self._text_cache: dict[Path, str] = {}
        self._index_cache: dict[Path, str | None] = {}
        self._index_exists_cache: dict[Path, bool] = {}

    @staticmethod
    def relative(path: Path) -> Path:
        return path.relative_to(REPO_ROOT) if path.is_absolute() else path

    def index_text(self, path: Path) -> str | None:
        relative_path = self.relative(path)
        if relative_path not in self._index_cache:
            result = subprocess.run(
                ("git", "show", f":{relative_path.as_posix()}"),
                cwd=REPO_ROOT,
                check=False,
                capture_output=True,
                text=True,
            )
            self._index_cache[relative_path] = (
                result.stdout if result.returncode == 0 else None
            )
        return self._index_cache[relative_path]

    def read_text(self, path: Path) -> str:
        relative_path = self.relative(path)
        if relative_path not in self._text_cache:
            if self.use_index:
                indexed = self.index_text(relative_path)
                if indexed is None:
                    raise FileNotFoundError(relative_path)
                self._text_cache[relative_path] = indexed
            else:
                self._text_cache[relative_path] = (REPO_ROOT / relative_path).read_text(
                    encoding="utf-8"
                )
        return self._text_cache[relative_path]

    def index_exists(self, path: Path) -> bool:
        relative_path = self.relative(path)
        if relative_path not in self._index_exists_cache:
            if self.index_text(relative_path) is not None:
                self._index_exists_cache[relative_path] = True
            else:
                result = subprocess.run(
                    (
                        "git",
                        "ls-files",
                        "--cached",
                        "--",
                        relative_path.as_posix(),
                    ),
                    cwd=REPO_ROOT,
                    check=True,
                    capture_output=True,
                    text=True,
                )
                self._index_exists_cache[relative_path] = bool(result.stdout.strip())
        return self._index_exists_cache[relative_path]

    def exists(self, path: Path) -> bool:
        relative_path = self.relative(path)
        if self.use_index:
            return self.index_exists(relative_path)
        return (REPO_ROOT / relative_path).exists()


def display_path(path: Path) -> str:
    return path.as_posix()


def report(errors: list[str], message: str) -> None:
    errors.append(message)


def token_present(text: str, token: str) -> bool:
    """Match numeric evidence as a count, not as a substring of a hash."""
    if re.fullmatch(r"[0-9][0-9,']*(?:/[0-9][0-9,']*)?", token):
        pattern = rf"(?<![0-9A-Za-z]){re.escape(token)}(?![0-9A-Za-z])"
        return re.search(pattern, text) is not None
    return token in text


def comma_grouped_numeric_literal(literal: str) -> str:
    """Format an apostrophe-grouped C++ decimal literal for prose."""
    value = int(literal.replace("'", ""))
    return f"{value:,}"


def require_tokens(
    path: Path,
    tokens: tuple[str, ...],
    errors: list[str],
    view: RepositoryView,
) -> None:
    text = view.read_text(path)
    for token in tokens:
        if not token_present(text, token):
            report(errors, f"{display_path(path)}: missing required token {token!r}")


def sbf_c_status_bodies(errors: list[str], view: RepositoryView) -> tuple[str, str]:
    """Render the documented C v1/v2 domains from the source ABI registry."""
    try:
        source = view.read_text(SBF_SOURCE_STATUSES_PATH)
    except FileNotFoundError as error:
        report(errors, f"missing SBF source-status authority: {error}")
        return "", ""

    v1_error_names = set(
        re.findall(
            r"^[ \t]*SBF_SOURCE_C_V1_ERROR\(\s*"
            r"([A-Za-z_][A-Za-z0-9_]*)\s*\)",
            source,
            flags=re.MULTILINE,
        )
    )
    invocation_re = re.compile(
        r"^[ \t]*SBF_SOURCE_(SUCCESS|ERROR)\s*\((.*?)\)\s*$",
        flags=re.MULTILINE | re.DOTALL,
    )
    v1_rows: list[str] = []
    v2_rows: list[str] = []
    seen_names: set[str] = set()
    for index, invocation in enumerate(invocation_re.finditer(source), start=1):
        kind = invocation.group(1)
        fields = tuple(
            re.sub(r"\s+", "", field) for field in invocation.group(2).split(",")
        )
        expected_field_count = 4 if kind == "SUCCESS" else 5
        if len(fields) != expected_field_count:
            report(
                errors,
                f"{display_path(SBF_SOURCE_STATUSES_PATH)}: malformed "
                f"SBF_SOURCE_{kind} row {index}",
            )
            continue
        logical_name, _fault_name, c_name, c_value = fields[:4]
        if logical_name in seen_names:
            report(
                errors,
                f"{display_path(SBF_SOURCE_STATUSES_PATH)}: duplicate status "
                f"{logical_name!r}",
            )
            continue
        seen_names.add(logical_name)
        row = f"{c_name} = {c_value},"
        if kind == "SUCCESS" or logical_name in v1_error_names:
            v1_rows.append(row)
        else:
            v2_rows.append(row)

    missing_v1_names = v1_error_names - seen_names
    for logical_name in sorted(missing_v1_names):
        report(
            errors,
            f"{display_path(SBF_SOURCE_STATUSES_PATH)}: missing status row "
            f"for C v1 member {logical_name!r}",
        )
    if not v1_rows:
        report(
            errors,
            f"{display_path(SBF_SOURCE_STATUSES_PATH)}: no C status rows",
        )
    return "\n".join(v1_rows), "\n".join(v2_rows)


def validate_sbf_c_status_contract(
    errors: list[str],
    path: Path,
    text: str,
    v1_status_body: str,
    v2_status_body: str,
) -> None:
    """Keep the documented C v1/v2 status domains byte-for-byte aligned."""
    c_sources = re.findall(r"```c\s*\n(.*?)\n```", text, flags=re.DOTALL)
    source = next(
        (
            candidate
            for candidate in c_sources
            if "typedef enum neverd_sbf_status" in candidate
        ),
        None,
    )
    if source is None:
        return

    v1_match = re.search(
        r"typedef enum neverd_sbf_status \{\n(.*?)\n\} neverd_sbf_status;",
        source,
        flags=re.DOTALL,
    )
    v1_body = (
        "\n".join(line.strip() for line in v1_match.group(1).splitlines())
        if v1_match
        else ""
    )
    if v1_body != v1_status_body:
        report(
            errors,
            f"{display_path(path)}: C v1 status enum must match SBFSourceStatuses.def",
        )

    v2_match = re.search(
        r"typedef uint32_t neverd_sbf_status_v2;\n(?:/\*.*?\*/\n)?enum \{\n(.*?)\n\};",
        source,
        flags=re.DOTALL,
    )
    v2_body = (
        "\n".join(line.strip() for line in v2_match.group(1).splitlines())
        if v2_match
        else ""
    )
    if v2_body != v2_status_body:
        report(
            errors,
            f"{display_path(path)}: C v2 status extensions must match "
            "SBFSourceStatuses.def",
        )


def validate_architecture_semantics(errors: list[str], view: RepositoryView) -> None:
    for path in ARCHITECTURE_DOCS:
        text = view.read_text(path)
        for capability_id, tokens in ARCHITECTURE_CAPABILITY_TOKENS.items():
            for token in tokens:
                if token not in text:
                    report(
                        errors,
                        f"{display_path(path)}: architecture capability "
                        f"{capability_id!r} missing required token {token!r}",
                    )


def validate_def_active_content(
    errors: list[str],
    path: Path,
    source: str,
    invocations: tuple[re.Match[str], ...],
) -> None:
    """Reject executable-looking .def content outside recognized invocations."""
    residue = list(source)
    for invocation in invocations:
        for position in range(*invocation.span()):
            if residue[position] != "\n":
                residue[position] = " "
    active_source = "".join(residue)
    active_source = re.sub(r"/\*.*?\*/", "", active_source, flags=re.DOTALL)
    active_source = re.sub(r"//[^\n]*", "", active_source)
    active_source = re.sub(r"^[ \t]*\#.*$", "", active_source, flags=re.MULTILINE)
    unknown_lines = [
        (line_number, line.strip())
        for line_number, line in enumerate(active_source.splitlines(), start=1)
        if line.strip()
    ]
    if unknown_lines:
        line_number, content = unknown_lines[0]
        report(
            errors,
            f"{display_path(path)}:{line_number}: unknown active content {content!r}",
        )


def sbf_upstream_source_revisions(
    errors: list[str], view: RepositoryView
) -> dict[str, str]:
    """Strictly parse every source and toolchain row in the provenance registry."""
    try:
        upstream = view.read_text(SBF_UPSTREAM_SOURCES_PATH)
    except FileNotFoundError as error:
        report(errors, f"missing SBF upstream authority: {error}")
        return {}

    source_invocation_re = re.compile(
        r"^[ \t]*SBF_UPSTREAM_SOURCE\s*\((.*?)\)\s*$",
        flags=re.MULTILINE | re.DOTALL,
    )
    toolchain_invocation_re = re.compile(
        r"^[ \t]*SBF_UPSTREAM_TOOLCHAIN\s*\((.*?)\)\s*$",
        flags=re.MULTILINE | re.DOTALL,
    )
    source_row_re = re.compile(
        r'\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*"([^"\n]+)"\s*,'
        r'\s*"((?:[0-9a-f]{40})?)"\s*'
    )
    toolchain_row_re = re.compile(
        r'\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*"([^"\n]+)"\s*,'
        r'\s*"([^"\n]+)"\s*'
    )
    source_invocations = tuple(source_invocation_re.finditer(upstream))
    toolchain_invocations = tuple(toolchain_invocation_re.finditer(upstream))
    revisions: dict[str, str] = {}
    source_names: set[str] = set()
    for index, invocation in enumerate(source_invocations, start=1):
        row = source_row_re.fullmatch(invocation.group(1))
        if row is None:
            report(
                errors,
                f"{display_path(SBF_UPSTREAM_SOURCES_PATH)}: malformed "
                f"SBF_UPSTREAM_SOURCE row {index}",
            )
            continue
        source_id, source_name, revision = row.groups()
        if source_id in revisions:
            report(
                errors,
                f"{display_path(SBF_UPSTREAM_SOURCES_PATH)}: duplicate source ID "
                f"{source_id!r}",
            )
            continue
        if source_name in source_names:
            report(
                errors,
                f"{display_path(SBF_UPSTREAM_SOURCES_PATH)}: duplicate source name "
                f"{source_name!r}",
            )
            continue
        revisions[source_id] = revision
        source_names.add(source_name)

    toolchain_ids: set[str] = set()
    toolchain_names: set[str] = set()
    for index, invocation in enumerate(toolchain_invocations, start=1):
        row = toolchain_row_re.fullmatch(invocation.group(1))
        if row is None:
            report(
                errors,
                f"{display_path(SBF_UPSTREAM_SOURCES_PATH)}: malformed "
                f"SBF_UPSTREAM_TOOLCHAIN row {index}",
            )
            continue
        toolchain_id, toolchain_name, _version = row.groups()
        if toolchain_id in toolchain_ids:
            report(
                errors,
                f"{display_path(SBF_UPSTREAM_SOURCES_PATH)}: duplicate "
                f"toolchain ID {toolchain_id!r}",
            )
            continue
        if toolchain_name in toolchain_names:
            report(
                errors,
                f"{display_path(SBF_UPSTREAM_SOURCES_PATH)}: duplicate "
                f"toolchain name {toolchain_name!r}",
            )
            continue
        toolchain_ids.add(toolchain_id)
        toolchain_names.add(toolchain_name)

    validate_def_active_content(
        errors,
        SBF_UPSTREAM_SOURCES_PATH,
        upstream,
        (*source_invocations, *toolchain_invocations),
    )
    if not revisions:
        report(
            errors,
            f"{display_path(SBF_UPSTREAM_SOURCES_PATH)}: no upstream sources",
        )
    return revisions


def sbf_protocol_limits(
    errors: list[str],
    view: RepositoryView,
    upstream_sources: dict[str, str],
) -> dict[str, tuple[str, str]]:
    """Strictly parse protocol values and bind each to a pinned source row."""
    try:
        source = view.read_text(SBF_PROTOCOL_LIMITS_PATH)
    except FileNotFoundError as error:
        report(errors, f"missing SBF evidence authority: {error}")
        return {}

    invocation_re = re.compile(
        r"^[ \t]*SBF_PROTOCOL_LIMIT\s*\((.*?)\)\s*$",
        flags=re.MULTILINE | re.DOTALL,
    )
    row_re = re.compile(
        r"\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([0-9][0-9']*)\s*,"
        r"\s*([A-Za-z_][A-Za-z0-9_]*)\s*"
    )
    invocations = tuple(invocation_re.finditer(source))
    limits: dict[str, tuple[str, str]] = {}
    for index, invocation in enumerate(invocations, start=1):
        row = row_re.fullmatch(invocation.group(1))
        if row is None:
            report(
                errors,
                f"{display_path(SBF_PROTOCOL_LIMITS_PATH)}: malformed "
                f"SBF_PROTOCOL_LIMIT row {index}",
            )
            continue
        name, value, source_id = row.groups()
        if name in limits:
            report(
                errors,
                f"{display_path(SBF_PROTOCOL_LIMITS_PATH)}: duplicate protocol "
                f"limit {name!r}",
            )
            continue
        limits[name] = (value, source_id)
        if source_id not in upstream_sources:
            report(
                errors,
                f"{display_path(SBF_PROTOCOL_LIMITS_PATH)}: protocol limit "
                f"{name!r} references unknown source {source_id!r}",
            )
        elif not upstream_sources[source_id]:
            report(
                errors,
                f"{display_path(SBF_PROTOCOL_LIMITS_PATH)}: protocol limit "
                f"{name!r} references unpinned source {source_id!r}",
            )

    validate_def_active_content(errors, SBF_PROTOCOL_LIMITS_PATH, source, invocations)
    if not limits:
        report(
            errors,
            f"{display_path(SBF_PROTOCOL_LIMITS_PATH)}: no protocol limits",
        )
    return limits


def sbf_comparison_tools(
    errors: list[str], view: RepositoryView
) -> tuple[tuple[str, str, str], ...]:
    """Return the ID, display name, and exact revision from the audit registry."""
    try:
        source = view.read_text(SBF_COMPARISON_TOOLS_PATH)
    except FileNotFoundError as error:
        report(errors, f"missing SBF comparison authority: {error}")
        return ()

    invocation_re = re.compile(
        r"^[ \t]*SBF_COMPARISON_TOOL\s*\((.*?)\)\s*$",
        re.MULTILINE | re.DOTALL,
    )
    row_re = re.compile(
        r'\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*"([^"\n]+)"\s*,'
        r'\s*"([0-9a-f]{40})"\s*'
    )
    invocations = tuple(invocation_re.finditer(source))
    tools: list[tuple[str, str, str]] = []
    seen_ids: set[str] = set()
    seen_names: set[str] = set()
    for index, invocation in enumerate(invocations, start=1):
        row = row_re.fullmatch(invocation.group(1))
        if row is None:
            report(
                errors,
                f"{display_path(SBF_COMPARISON_TOOLS_PATH)}: malformed "
                f"SBF_COMPARISON_TOOL row {index}",
            )
            continue
        tool_id, display_name, revision = row.groups()
        if tool_id in seen_ids:
            report(
                errors,
                f"{display_path(SBF_COMPARISON_TOOLS_PATH)}: duplicate tool ID "
                f"{tool_id!r}",
            )
            continue
        if display_name in seen_names:
            report(
                errors,
                f"{display_path(SBF_COMPARISON_TOOLS_PATH)}: duplicate display "
                f"name {display_name!r}",
            )
            continue
        seen_ids.add(tool_id)
        seen_names.add(display_name)
        tools.append((tool_id, display_name, revision))

    validate_def_active_content(errors, SBF_COMPARISON_TOOLS_PATH, source, invocations)

    if not tools:
        report(
            errors,
            f"{display_path(SBF_COMPARISON_TOOLS_PATH)}: no pinned comparison tools",
        )
    return tuple(tools)


def sbf_comparison_evidence_tokens(
    errors: list[str], view: RepositoryView
) -> tuple[str, ...]:
    return tuple(
        token
        for _tool_id, display_name, revision in sbf_comparison_tools(errors, view)
        for token in (display_name, revision)
    )


SBF_COMPARISON_REVISION_RE = re.compile(r"(?<![0-9a-f])([0-9a-f]{40})(?![0-9a-f])")


def comparison_tool_revision_is_paired(
    text: str,
    display_name: str,
    revision: str,
    all_display_names: tuple[str, ...],
) -> bool:
    """Require every local name-then-revision claim to use the right object."""
    registered_name_re = re.compile(
        "|".join(
            re.escape(name) for name in sorted(all_display_names, key=len, reverse=True)
        )
    )
    candidates: list[str] = []
    prose = without_markdown_fences(text)
    segments = re.split(r"\n(?=- )|\n{2,}", prose)
    for segment in segments:
        registered_names = tuple(registered_name_re.finditer(segment))
        for occurrence in re.finditer(re.escape(display_name), segment):
            following = SBF_COMPARISON_REVISION_RE.search(segment, occurrence.end())
            if following is None:
                continue
            next_name = next(
                (
                    candidate
                    for candidate in registered_names
                    if candidate.start() > occurrence.start()
                ),
                None,
            )
            if next_name is not None and next_name.start() < following.start():
                continue
            candidates.append(following.group(1))
    return bool(candidates) and all(candidate == revision for candidate in candidates)


def validate_sbf_comparison_pairs(
    errors: list[str],
    path: Path,
    text: str,
    tools: tuple[tuple[str, str, str], ...],
) -> None:
    """Keep every display name bound to its own pinned repository object."""
    display_names = tuple(display_name for _id, display_name, _revision in tools)
    for _tool_id, display_name, revision in tools:
        # The ordinary token check owns missing-name/revision diagnostics. Pair
        # checking is the stronger guard once both independent tokens exist.
        if display_name not in text or revision not in text:
            continue
        if comparison_tool_revision_is_paired(
            text, display_name, revision, display_names
        ):
            continue
        report(
            errors,
            f"{display_path(path)}: comparison tool {display_name!r} "
            f"is not paired with revision {revision!r}",
        )


def sbf_authority_evidence_token_sets(
    errors: list[str], view: RepositoryView
) -> tuple[tuple[str, ...], tuple[str, ...]]:
    """Return common and guide-only evidence from production authorities."""
    upstream_sources = sbf_upstream_source_revisions(errors, view)
    revisions = tuple(revision for revision in upstream_sources.values() if revision)
    limit_records = sbf_protocol_limits(errors, view, upstream_sources)

    if not revisions:
        report(
            errors,
            f"{display_path(SBF_UPSTREAM_SOURCES_PATH)}: no pinned revisions",
        )

    limits = {name: value for name, (value, _source) in limit_records.items()}
    required_limits = (
        "InstructionByteCount",
        "MaxProgramAccountDataSize",
        "LegacyProgramInstructionCount",
    )
    missing_limits = tuple(name for name in required_limits if name not in limits)
    for name in missing_limits:
        report(
            errors,
            f"{display_path(SBF_PROTOCOL_LIMITS_PATH)}: missing {name}",
        )
    if missing_limits:
        return revisions, revisions

    deployable_value = int(limits["MaxProgramAccountDataSize"].replace("'", ""))
    deployable_limit = f"{deployable_value:_}".replace("_", "'")
    legacy_value = int(limits["LegacyProgramInstructionCount"].replace("'", ""))
    legacy_limit = f"{legacy_value:,}"
    instruction_bytes = int(limits["InstructionByteCount"].replace("'", ""))
    if instruction_bytes == 0 or deployable_value % instruction_bytes != 0:
        report(
            errors,
            f"{display_path(SBF_PROTOCOL_LIMITS_PATH)}: deployable byte limit "
            "is not divisible by InstructionByteCount",
        )
        common = (*revisions, deployable_limit, legacy_limit)
        return common, common
    max_instruction_count = f"{deployable_value // instruction_bytes:,}"
    common = (*revisions, deployable_limit, legacy_limit)
    return common, (*common, max_instruction_count)


def sbf_authority_evidence_tokens(
    errors: list[str], view: RepositoryView
) -> tuple[str, ...]:
    """Read evidence shared by the SBF guide and testing documentation."""
    common, _guide = sbf_authority_evidence_token_sets(errors, view)
    return common


def sbf_guide_authority_evidence_tokens(
    errors: list[str], view: RepositoryView
) -> tuple[str, ...]:
    """Read common evidence plus guide-only derived instruction limits."""
    _common, guide = sbf_authority_evidence_token_sets(errors, view)
    return guide


def sbf_analysis_evidence_tokens(
    errors: list[str], view: RepositoryView
) -> tuple[str, ...]:
    """Read documented host-analysis budgets from their typed registry."""
    try:
        source = view.read_text(SBF_ANALYSIS_LIMITS_PATH)
    except FileNotFoundError as error:
        report(errors, f"missing SBF analysis-limit authority: {error}")
        return ()
    limits = dict(
        re.findall(
            r"\bSBF_ANALYSIS_LIMIT\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,"
            r"\s*([0-9][0-9']*)\s*\)",
            source,
        )
    )
    required_limits = (
        "MaxModeledScratchBytes",
        "ScratchFlowRetainedByteBudget",
    )
    missing_limits = tuple(name for name in required_limits if name not in limits)
    for name in missing_limits:
        report(
            errors,
            f"{display_path(SBF_ANALYSIS_LIMITS_PATH)}: missing {name}",
        )
    return tuple(
        comma_grouped_numeric_literal(limits[name])
        for name in required_limits
        if name in limits
    )


def sbf_guide_evidence_tokens(
    errors: list[str], view: RepositoryView
) -> tuple[str, ...]:
    return (
        *SBF_GUIDE_EVIDENCE_TOKENS,
        *sbf_guide_authority_evidence_tokens(errors, view),
        *sbf_analysis_evidence_tokens(errors, view),
        *sbf_comparison_evidence_tokens(errors, view),
    )


def sbf_testing_evidence_tokens(
    errors: list[str], view: RepositoryView
) -> tuple[str, ...]:
    return (
        *SBF_TESTING_EVIDENCE_TOKENS,
        *sbf_authority_evidence_tokens(errors, view),
    )


def validate_sbf_evidence(errors: list[str], view: RepositoryView) -> None:
    """Keep translated SBF claims pinned to reproducible, stable evidence."""
    authority_tokens, guide_authority_tokens = sbf_authority_evidence_token_sets(
        errors, view
    )
    analysis_tokens = sbf_analysis_evidence_tokens(errors, view)
    comparison_tools = sbf_comparison_tools(errors, view)
    comparison_tokens = tuple(
        token
        for _tool_id, display_name, revision in comparison_tools
        for token in (display_name, revision)
    )
    for path in SBF_GUIDE_DOCS:
        require_tokens(
            path,
            (
                *SBF_GUIDE_EVIDENCE_TOKENS,
                *guide_authority_tokens,
                *analysis_tokens,
                *comparison_tokens,
            ),
            errors,
            view,
        )
        validate_sbf_comparison_pairs(
            errors, path, view.read_text(path), comparison_tools
        )
    for path in SBF_TESTING_DOCS:
        require_tokens(
            path,
            (*SBF_TESTING_EVIDENCE_TOKENS, *authority_tokens),
            errors,
            view,
        )

    for path in (*SBF_GUIDE_DOCS, *SBF_TESTING_DOCS):
        text = view.read_text(path)
        for token in SBF_STALE_EVIDENCE_TOKENS:
            if token in text:
                report(
                    errors,
                    f"{display_path(path)}: stale SBF evidence token {token!r}",
                )


def validate_sbf_testing_rows(errors: list[str], view: RepositoryView) -> None:
    """Keep the summary row in every testing translation aligned with CMake."""
    for path in SBF_TESTING_DOCS:
        rows = SBF_TESTING_ROW_RE.findall(view.read_text(path))
        if not rows:
            report(
                errors,
                f"{display_path(path)}: missing SBF testing summary row",
            )
            continue
        row = rows[0]
        for token in SBF_TESTING_TARGET_TOKENS:
            if token not in row:
                report(
                    errors,
                    f"{display_path(path)}: SBF testing row missing target {token!r}",
                )
        for marker in SBF_TESTING_ARTIFACT_MARKERS:
            if marker not in row:
                report(
                    errors,
                    f"{display_path(path)}: SBF testing row missing artifact "
                    f"marker {marker!r}",
                )


def validate_sbf_host_api(errors: list[str], view: RepositoryView) -> None:
    """Keep generated C/Rust host examples tied to the emitted API names."""
    v1_status_body, v2_status_body = sbf_c_status_bodies(errors, view)
    for path in SBF_GUIDE_DOCS:
        text = view.read_text(path)
        require_tokens(path, SBF_HOST_API_TOKENS, errors, view)
        validate_sbf_c_status_contract(
            errors, path, text, v1_status_body, v2_status_body
        )


def markdown_fenced_blocks(text: str) -> tuple[tuple[str, str], ...]:
    """Return Markdown fenced blocks as (info-string, body) pairs."""
    blocks: list[tuple[str, str]] = []
    opening: tuple[str, int, str, list[str]] | None = None
    for line in text.splitlines():
        if opening is None:
            match = FENCE_RE.match(line)
            if not match:
                continue
            marker = match.group(1)
            info = line[match.end() :].strip().lower()
            opening = (marker[0], len(marker), info, [])
            continue
        marker_character, minimum_length, info, body = opening
        stripped = line.lstrip()
        if (
            stripped.startswith(marker_character)
            and len(stripped) >= minimum_length
            and set(stripped) == {marker_character}
        ):
            blocks.append((info, "\n".join(body)))
            opening = None
            continue
        body.append(line)
    return tuple(blocks)


def without_markdown_fences(text: str) -> str:
    """Remove fenced samples so prose checks cannot be satisfied by code."""
    output: list[str] = []
    in_fence = False
    marker_character = ""
    minimum_length = 0
    for line in text.splitlines():
        if not in_fence:
            match = FENCE_RE.match(line)
            if match:
                marker = match.group(1)
                marker_character = marker[0]
                minimum_length = len(marker)
                in_fence = True
            else:
                output.append(line)
            continue
        stripped = line.lstrip()
        if (
            stripped.startswith(marker_character)
            and len(stripped) >= minimum_length
            and set(stripped) == {marker_character}
        ):
            in_fence = False
    return "\n".join(output)


def rust_host_prose(text: str) -> str | None:
    match = re.search(
        r"^## [^\n]*Rust[^\n]*\n(.*?)(?=^## |\Z)",
        text,
        flags=re.IGNORECASE | re.MULTILINE | re.DOTALL,
    )
    return None if match is None else without_markdown_fences(match.group(1))


def validate_sbf_rust_host_prose(errors: list[str], view: RepositoryView) -> None:
    """Require the localized Rust ABI contract in prose, outside code fences."""
    for path in SBF_GUIDE_DOCS[1:]:
        prose = rust_host_prose(view.read_text(path))
        if prose is None:
            report(
                errors,
                f"{display_path(path)}: missing Rust host prose section",
            )
            continue
        for token in SBF_RUST_PROSE_MARKERS:
            if token not in prose:
                report(
                    errors,
                    f"{display_path(path)}: Rust host prose missing required marker "
                    f"{token!r}",
                )
        ordered_groups = (
            (
                "`neverd_sbf_program`",
                "`SbfEnvironment`",
                "`v1-result-abi`",
            ),
            (
                "`Some(SbfRuntimeFeatures::from_bits(0))`",
                "`explicit-empty-snapshot`",
            ),
            ("`syscall_outcome`", "`result-host-bridge`"),
            (
                "`SbfErrorV2`",
                "`#[non_exhaustive]`",
                "`non-exhaustive-wildcard`",
            ),
        )
        for group in ordered_groups:
            if not all(token in prose for token in group):
                continue
            positions = [prose.find(token) for token in group]
            if any(position < 0 for position in positions) or positions != sorted(
                positions
            ):
                report(
                    errors,
                    f"{display_path(path)}: Rust host prose markers are out of order",
                )


def c_host_prose(text: str) -> str | None:
    headings = list(re.finditer(r"^##\s+([^\n]+)\n", text, flags=re.MULTILINE))
    for index, heading in enumerate(headings):
        title = heading.group(1)
        lowered = title.casefold()
        if "c" not in lowered or "host" not in lowered:
            continue
        end = len(text)
        for following in headings[index + 1 :]:
            end = following.start()
            break
        return without_markdown_fences(text[heading.end() : end])
    return None


def validate_sbf_c_host_prose(errors: list[str], view: RepositoryView) -> None:
    """Require localized C v1/v2 behavior in prose, not only in fenced C."""
    for path in SBF_GUIDE_DOCS[1:]:
        prose = c_host_prose(view.read_text(path))
        if prose is None:
            report(
                errors,
                f"{display_path(path)}: missing C host prose section",
            )
            continue
        for group in SBF_C_PROSE_MARKER_GROUPS:
            if not any(
                all(token.casefold() in paragraph.casefold() for token in group)
                for paragraph in prose.split("\n\n")
            ):
                report(
                    errors,
                    f"{display_path(path)}: C host prose missing same-paragraph "
                    "marker group "
                    f"{group!r}",
                )


def validate_sbf_c_api_examples(errors: list[str], view: RepositoryView) -> None:
    """Keep the optional IDL setter inside the generated C example fence."""
    for path in SBF_GUIDE_DOCS:
        if not any(
            "neverd_sbf_set_idl(session, idl_json);" in body
            for info, body in markdown_fenced_blocks(view.read_text(path))
            if info == "c"
        ):
            report(
                errors,
                f"{display_path(path)}: C host example missing fenced IDL setter",
            )


def validate_sbf_scratch_prose(errors: list[str], view: RepositoryView) -> None:
    """Keep the scratch policy contract together in a localized prose paragraph."""
    markers = (
        *SBF_SCRATCH_PROSE_MARKERS,
        *sbf_analysis_evidence_tokens(errors, view),
    )
    for path in SBF_GUIDE_DOCS:
        paragraphs = without_markdown_fences(view.read_text(path)).split("\n\n")
        if not any(
            all(marker in paragraph for marker in markers) for paragraph in paragraphs
        ):
            report(
                errors,
                f"{display_path(path)}: scratch policy markers are not co-located",
            )


def validate_sbf_conformance_commands(errors: list[str], view: RepositoryView) -> None:
    """Require the reproducible SBF command and both pinned external roots."""
    test_vectors_revision = sbf_upstream_source_revisions(errors, view).get(
        "FiredancerTestVectors"
    )
    command_lines = SBF_CONFORMANCE_COMMAND_LINES
    if test_vectors_revision:
        command_lines = (
            *command_lines[:2],
            f"NEVERD_AGAVE_CONFORMANCE_REVISION={test_vectors_revision} \\",
            *command_lines[2:],
        )
    for path in SBF_GUIDE_DOCS:
        blocks = markdown_fenced_blocks(view.read_text(path))
        if not any(
            all(line in body.splitlines() for line in command_lines)
            for info, body in blocks
            if info in {"bash", "sh", "shell"}
        ):
            report(
                errors,
                f"{display_path(path)}: missing exact SBF conformance command block",
            )


def validate_sbf_execution_matrix_structure(
    errors: list[str], view: RepositoryView
) -> None:
    """Keep the additional execution matrix totals together in one prose block."""
    for path in (*SBF_GUIDE_DOCS, *SBF_TESTING_DOCS):
        paragraphs = without_markdown_fences(view.read_text(path)).split("\n\n")
        if not any(
            all(
                execution_matrix_marker_present(re.sub(r"\s+", " ", paragraph), marker)
                for marker in SBF_EXECUTION_MATRIX_MARKERS
            )
            for paragraph in paragraphs
        ):
            report(
                errors,
                f"{display_path(path)}: execution matrix totals are not co-located",
            )


def validate_sbf_evidence_table_continuity(
    errors: list[str], view: RepositoryView
) -> None:
    """Reject evidence rows stranded after prose interrupts their Markdown table."""
    for path in SBF_GUIDE_DOCS:
        lines = view.read_text(path).splitlines()
        in_fence = False
        for index, line in enumerate(lines):
            if FENCE_RE.match(line):
                in_fence = not in_fence
                continue
            if in_fence:
                continue
            if not line.lstrip().startswith("|"):
                continue
            previous = index - 1
            while previous >= 0 and not lines[previous].strip():
                previous -= 1
            next_line = index + 1
            while next_line < len(lines) and not lines[next_line].strip():
                next_line += 1
            next_text = lines[next_line].strip() if next_line < len(lines) else ""
            is_new_table_header = bool(re.search(r"-{3,}", next_text))
            if (
                previous < 0 or not lines[previous].lstrip().startswith("|")
            ) and not is_new_table_header:
                report(
                    errors,
                    f"{display_path(path)}:{index + 1}: evidence table row is "
                    "not contiguous with a Markdown header or preceding row",
                )


def validate_sbf_testing_ownership(errors: list[str], view: RepositoryView) -> None:
    """Keep ownership claims in the localized SBF testing prose structural."""
    for path in SBF_TESTING_DOCS:
        text = view.read_text(path)
        headings = list(
            re.finditer(r"^(#{1,6})\s+([^\n]+)\n", text, flags=re.MULTILINE)
        )
        sbf_sections: list[str] = []
        for index, heading in enumerate(headings):
            title = heading.group(2).casefold()
            if (
                len(heading.group(1)) != 3
                or "solana" not in title
                or "sbf" not in title
                or not any(hint in title for hint in SBF_DIFFERENTIAL_HEADING_HINTS)
            ):
                continue
            end = len(text)
            for following in headings[index + 1 :]:
                if len(following.group(1)) <= 3:
                    end = following.start()
                    break
            candidate = without_markdown_fences(text[heading.end() : end])
            if "NeverDSBFSemanticTests" in candidate:
                sbf_sections.append(candidate)
        prose = next(
            (
                candidate
                for candidate in sbf_sections
                if all(
                    any(group[0] in paragraph for paragraph in candidate.split("\n\n"))
                    for group in SBF_TESTING_OWNERSHIP_MARKERS
                )
            ),
            None,
        )
        if prose is None:
            report(
                errors,
                f"{display_path(path)}: missing SBF testing ownership section",
            )
            continue
        for marker_group in SBF_TESTING_OWNERSHIP_MARKERS:
            if not any(
                all(token.casefold() in paragraph.casefold() for token in marker_group)
                for paragraph in prose.split("\n\n")
            ):
                report(
                    errors,
                    f"{display_path(path)}: SBF ownership prose missing same-paragraph "
                    f"marker group {marker_group!r}",
                )


def validate_sbf_testing_evidence_prose(
    errors: list[str], view: RepositoryView
) -> None:
    """Require loader fixture identity and all observable ELF fields in prose."""
    test_vectors_revision = sbf_upstream_source_revisions(errors, view).get(
        "FiredancerTestVectors"
    )
    markers = SBF_TESTING_EVIDENCE_PROSE_MARKERS
    if test_vectors_revision:
        markers = (*markers, test_vectors_revision)
    for path in SBF_TESTING_DOCS:
        prose = without_markdown_fences(view.read_text(path))
        if not any(
            all(token in paragraph for token in markers)
            for paragraph in prose.split("\n\n")
        ):
            report(
                errors,
                f"{display_path(path)}: SBF evidence prose lacks one paragraph with "
                "Agave ownership, fixture identity, and all ELF evidence fields",
            )


def validate_sbf_testing_release_commands(
    errors: list[str], view: RepositoryView
) -> None:
    """Require one focused SBF release aggregate command in a fenced block."""
    for path in SBF_TESTING_DOCS:
        matches = [
            line.strip()
            for info, body in markdown_fenced_blocks(view.read_text(path))
            if info in {"bash", "sh", "shell"}
            for line in body.splitlines()
            if line.strip() == SBF_TESTING_RELEASE_COMMAND
        ]
        if len(matches) != 1:
            report(
                errors,
                f"{display_path(path)}: expected one exact fenced SBF release "
                f"aggregate command, found {len(matches)}",
            )


def strip_heading_markup(text: str) -> str:
    text = re.sub(r"<[^>]+>", "", text)
    text = re.sub(r"!\[([^\]]*)\]\([^)]+\)", r"\1", text)
    text = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", text)
    return text.replace("`", "").replace("*", "").replace("~", "")


def github_slug_base(heading: str) -> str:
    heading = strip_heading_markup(heading).strip().casefold()
    characters: list[str] = []
    for character in heading:
        category = unicodedata.category(character)
        if category.startswith("P") and character not in "-_":
            continue
        if category.startswith("C"):
            continue
        characters.append(character)
    return re.sub(r"\s+", "-", "".join(characters))


def markdown_anchors(path: Path, view: RepositoryView) -> set[str]:
    anchors: set[str] = set()
    occurrences: defaultdict[str, int] = defaultdict(int)
    in_fence = False
    fence_marker = ""
    for line in view.read_text(path).splitlines():
        stripped = line.lstrip()
        if stripped.startswith(("```", "~~~")):
            marker = stripped[:3]
            if not in_fence:
                in_fence = True
                fence_marker = marker
            elif marker == fence_marker:
                in_fence = False
            continue
        if in_fence:
            continue
        for explicit in EXPLICIT_ANCHOR_RE.findall(line):
            anchors.add(unquote(explicit))
        match = HEADING_RE.match(line)
        if not match:
            continue
        base = github_slug_base(match.group(2))
        suffix = occurrences[base]
        occurrences[base] += 1
        anchors.add(base if suffix == 0 else f"{base}-{suffix}")
    return anchors


def link_target(raw_target: str) -> str:
    target = raw_target.strip()
    if target.startswith("<") and target.endswith(">"):
        return target[1:-1]
    if " " in target:
        target = target.split(" ", 1)[0]
    return target


def validate_links(
    paths: tuple[Path, ...], errors: list[str], view: RepositoryView
) -> None:
    anchor_cache: dict[Path, set[str]] = {}
    for relative_path in paths:
        source = REPO_ROOT / relative_path
        for raw_target in LINK_RE.findall(view.read_text(relative_path)):
            target = link_target(raw_target)
            if not target or target.startswith(("http://", "https://", "mailto:")):
                continue
            path_text, separator, fragment = target.partition("#")
            if path_text.startswith("/"):
                continue
            destination = (
                source
                if not path_text
                else (source.parent / unquote(path_text)).resolve()
            )
            try:
                destination_relative = destination.relative_to(REPO_ROOT)
            except ValueError:
                report(
                    errors,
                    f"{display_path(relative_path)}: link escapes repository: {target}",
                )
                continue
            if not view.exists(destination_relative):
                report(
                    errors,
                    f"{display_path(relative_path)}: missing link target: {target}",
                )
                continue
            if not separator or not fragment or destination.suffix.lower() != ".md":
                continue
            anchors = anchor_cache.setdefault(
                destination_relative,
                markdown_anchors(destination_relative, view),
            )
            decoded_fragment = unquote(fragment)
            if decoded_fragment not in anchors:
                report(
                    errors,
                    f"{display_path(relative_path)}: missing Markdown anchor "
                    f"{decoded_fragment!r} in "
                    f"{display_path(destination.relative_to(REPO_ROOT))}",
                )


def validate_markdown_structure(
    paths: tuple[Path, ...], errors: list[str], view: RepositoryView
) -> None:
    for relative_path in paths:
        opening: tuple[str, int, int] | None = None
        for line_number, line in enumerate(
            view.read_text(relative_path).splitlines(), start=1
        ):
            match = FENCE_RE.match(line)
            if not match:
                continue
            marker = match.group(1)
            if opening is None:
                opening = (marker[0], len(marker), line_number)
                continue
            marker_character, minimum_length, _ = opening
            if marker[0] == marker_character and len(marker) >= minimum_length:
                opening = None
        if opening is not None:
            _, _, line_number = opening
            report(
                errors,
                f"{display_path(relative_path)}:{line_number}: unclosed Markdown fence",
            )


def validate_matrix(errors: list[str], view: RepositoryView) -> None:
    for path in MARKDOWN_DOCS:
        if not view.exists(path):
            report(
                errors,
                f"missing localized documentation file: {display_path(path)}",
            )
    if errors:
        return

    validate_architecture_semantics(errors, view)
    validate_sbf_evidence(errors, view)
    validate_sbf_testing_rows(errors, view)
    validate_sbf_host_api(errors, view)
    validate_sbf_rust_host_prose(errors, view)
    validate_sbf_c_host_prose(errors, view)
    validate_sbf_c_api_examples(errors, view)
    validate_sbf_scratch_prose(errors, view)
    validate_sbf_conformance_commands(errors, view)
    validate_sbf_execution_matrix_structure(errors, view)
    validate_sbf_evidence_table_continuity(errors, view)
    validate_sbf_testing_ownership(errors, view)
    validate_sbf_testing_evidence_prose(errors, view)
    validate_sbf_testing_release_commands(errors, view)

    selector_tokens = {
        stem: (f"{stem}.md", *(f"{stem}.{locale}.md" for locale in LOCALES))
        for stem in GUIDE_STEMS
    }
    for stem in GUIDE_STEMS:
        require_tokens(
            Path(f"docs/{stem}.md"),
            (*selector_tokens[stem], *GUIDE_REQUIRED_TOKENS[stem]),
            errors,
            view,
        )

    for locale in LOCALES:
        (
            project_readme,
            _contributing,
            index,
            _architecture,
            memory_safety,
            _python_plugins,
            roadmap,
            testing,
            _windows_exception,
            evm_guide,
            sbf_guide,
        ) = localized_paths(locale)
        require_tokens(
            project_readme,
            (
                "EVM256",
                "Solana SBF",
                "v0-v4",
                "--language=solidity",
                "--language=rust",
                f"../evm.{locale}.md",
                f"../sbf.{locale}.md",
            ),
            errors,
            view,
        )
        require_tokens(
            index,
            (f"evm.{locale}.md", f"sbf.{locale}.md"),
            errors,
            view,
        )
        require_tokens(
            roadmap,
            (
                "v0-v4",
                "Solidity",
                "Rust",
                f"../evm.{locale}.md",
                f"../sbf.{locale}.md",
            ),
            errors,
            view,
        )
        require_tokens(
            testing,
            (
                "NeverDEVMOpcodeTests",
                "NeverDEVMSemanticTests",
                "NeverDEVMIntegrationTests",
                "NeverDSBFMetadataTests",
                "NeverDSBFSemanticTests",
                "NeverDSBFIntegrationTests",
                "-R 'EVM'",
                *TESTING_REQUIRED_TOKENS,
            ),
            errors,
            view,
        )
        require_tokens(
            memory_safety,
            MEMORY_SAFETY_REQUIRED_TOKENS,
            errors,
            view,
        )
        require_tokens(
            evm_guide,
            (*selector_tokens["evm"], *GUIDE_REQUIRED_TOKENS["evm"]),
            errors,
            view,
        )
        require_tokens(
            sbf_guide,
            (*selector_tokens["sbf"], *GUIDE_REQUIRED_TOKENS["sbf"]),
            errors,
            view,
        )


def validate_staged(errors: list[str]) -> None:
    result = subprocess.run(
        ("git", "diff", "--cached", "--name-only"),
        cwd=REPO_ROOT,
        check=True,
        capture_output=True,
        text=True,
    )
    staged = tuple(line for line in result.stdout.splitlines() if line)
    prohibited = sorted(
        path
        for path in staged
        if path.startswith(PROHIBITED_STAGED_PREFIXES) or "/plans/" in f"/{path}"
    )
    if prohibited:
        report(errors, "plan documents must not be staged: " + ", ".join(prohibited))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check-staged",
        action="store_true",
        help="validate the Git index snapshot and reject staged plan documents",
    )
    arguments = parser.parse_args()

    errors: list[str] = []
    if arguments.check_staged:
        validate_staged(errors)
    view = RepositoryView(use_index=arguments.check_staged)
    validate_matrix(errors, view)
    existing_docs = tuple(path for path in MARKDOWN_DOCS if view.exists(path))
    validate_links(existing_docs, errors, view)
    validate_markdown_structure(existing_docs, errors, view)

    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        return 1
    print(
        f"localized documentation check passed: {len(MARKDOWN_DOCS)} Markdown files, "
        f"{len(LOCALES)} locales"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
