#!/usr/bin/env python3
"""Validate NeverD's localized documentation matrix and Markdown links."""

from __future__ import annotations

import argparse
import hashlib
import json
import posixpath
import re
import subprocess
import sys
import unicodedata
from collections import Counter, defaultdict
from functools import lru_cache
from pathlib import Path, PurePath
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
    *(Path(f"docs/{locale}/architecture.md") for locale in LOCALES),
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
GUIDE_STEMS = ("evm", "sbf", "android", "ios")
MACOS_GUIDE_STEMS = ("macos-hvf", "darwin-emulation")
EMULATION_DOC_INVENTORY = Path("scripts/EmulationDocumentation.def")
EMULATION_GUIDE_STEMS = tuple(re.findall(
    r'NEVERD_EMULATION_DOC_GUIDE\(\s*"([^\"]+)"\s*\)',
    (REPO_ROOT / EMULATION_DOC_INVENTORY).read_text(encoding="utf-8"),
))
SBF_GUIDE_DOCS = (
    Path("docs/sbf.md"),
    *(Path(f"docs/{locale}/sbf.md") for locale in LOCALES),
)
SBF_TESTING_DOCS = (
    Path("docs/testing.md"),
    *(Path(f"docs/{locale}/testing.md") for locale in LOCALES),
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


EVM_PUBLIC_BOUNDARY_TOKENS = (
    "EVMAnalysisLimits.def",
    "EVMInterpreterLimits.def",
    "EVMABIParserLimits.def",
    "EVMABITableLimits.def",
    "`Code`/`Fork`/`Instructions`/`JumpDestinations`",
    "lowerToMedIR",
    "llvm::Error",
    "Any/Exact/Excluded",
    "XOR(selector, constant)",
    "MaxHighDispatchCandidates",
    "MaxHighRecoveredArguments",
    "MaxHighDiagnostics",
    "MaxHighDiagnosticBytes",
    "MaxHighReferenceVisits",
    "MaxHighMemoryTransferCells",
    "MaxHighMemoryValueVisits",
    "MaxSteps",
    "MaxMemoryBytes",
    "MaxTraceEntries",
    "MaxLogEntries",
    "MaxLogDataBytes",
    "MaxHostReturnDataBytes",
    "MaxPersistentStateEntries",
    "StepLimit",
    "ArrayRef",
    "lower_bound",
    "APInt",
)
EVM_UPSTREAM_CLOSURE_TOKENS = (
    "EVM_GETH_RULE_FIELD",
    "MappedForkSelector",
    "NoOpcodeAllocation",
    "ExcludedSelectorExpectedError",
    "EVMEIP8024Immediates.def",
    "go -overlay",
    "operation.execute",
    "DUPN",
    "SWAPN",
    "EXCHANGE",
    "EVM_HARDFORK_LATEST",
    "EVMUpstreamForkAliases.def",
    "BPO5",
    "IsUBT",
    "audit_unix_time",
    "MainnetChainConfig.LatestFork",
    "official-fresh-fetch",
    "GOTOOLCHAIN=local",
)
EVM_LOW_DIAGNOSTIC_TOKENS = (
    "MaxLowDiagnostics",
    "MaxLowDiagnosticBytes",
    "ERC-1167",
)
EVM_UPSTREAM_LIVE_RESULT_TOKENS = (
    "02b73d4ea7181464175e0a6cbecc0a3a2655a562",
    "schema_version=3",
    "audit_unix_time=1787534659",
    "remote=https://github.com/ethereum/go-ethereum.git",
    "ref=HEAD",
    "Go 1.24.0",
    "stack_limit=1024",
    "diagnostics=[]",
    "21 fork tables",
    "20 Rules probes",
    "15 mapped/4 no-op/1 expected-error",
    "upstream BPO2",
    "NeverD Fusaka",
    "23 table targets",
    "Amsterdam/Bogota",
    "1536 candidate executions",
    "6 missing-operand cases",
    "three handler symbols",
    "sandbox-exec",
    "go run",
    "bubblewrap",
)
EVM_UPSTREAM_SCHEMA3_TOKENS = (
    "--manifest-output",
    "canonical fork jump tables",
    "mainnet active/scheduled jump tables",
    "inactive",
    "partial",
    "3x256",
    "input/collection/string hard limits",
    "bounded diagnostic output",
    "explicit truncated marker",
    "digest",
    "process group",
    "process tree",
    ".def parser",
)
EVM_FINAL_RUNTIME_IR_TOKENS = (
    "MaxCalldataBytes",
    "MaxHostEnvironmentEntries",
    "BlockHashes",
    "Balances",
    "CodeHashes",
    "ExternalCode",
    "BlobHashes",
    "MaxExternalCodeBytes",
    "MaxHighRegionBlockReferences",
    "EVMLowFaultKinds.def",
    "InvalidJumpDestination",
    "end-of-code JUMPI",
    "canonical decode replay",
    "lowerCanonicalLowToMedIR",
    "recoverCanonicalHighIR",
    "const execute preflight",
    "capability-root",
    "go env",
    "go mod init",
    "go mod edit",
    "go mod tidy",
    "go mod download",
    "resolved GOROOT",
    "host HOME/workspace",
    "audit_unix_time=1787534659",
    "67/67",
    "C++ Opcode 10/10",
    "`/` broad bind",
)
EVM_FUNCTION_SCOPE_GUIDE_TOKENS = (
    "exact singleton selector",
    "SelectorEquality",
    "definite edge",
    "shared body/tail-call",
)
EVM_FUNCTION_SCOPE_TEST_TOKENS = (
    "`EQ`",
    "`raw XOR`",
    "`arguments`",
    "`mutability`",
    "`return shape`",
    "`region`",
)
GUIDE_REQUIRED_TOKENS = {
    "ios": (
        "neverd mobile", "IPA", ".app", "Mach-O", "C++20",
        "NeverDMobileTests", "LLVMSwiftDemangle", "llvm-swift-demangle",
        '"execution": "builtin"', '"version": "6.3.3"', "--artifact", "CFBundleExecutable",
        "--arch", "--metadata-only", "--max-func", "--timeout", "--max-files",
        "--max-bytes", "--json", "cryptid != 0", "2147483648", "20000", "300",
        "sources/objc.m", "sources/swift.swift", "metadata/objc-methods.json",
        "metadata/swift-signatures.json", "metadata/swift-methods.json",
        "schema_version", "unrecovered_method_count", "coverage_status",
        "unclassified_symbol_count", "not-callable", "source_units",
        "method_entries", "method_identities", "{entry, mangled_symbol}",
        "--format=objc-methods", "--format=swift-methods", "--source-signatures",
        "neverd_objc_methods_json", "neverd_swift_methods_json", "neverd_free_string",
        "test_mobile_ios_backend.py", "test_mobile_swift_backend.py", "--setup-only",
        "@synchronized", "required_cflags", "-fexceptions",
    ),
    "android": (
        "neverd mobile", ".apk", ".dex", ".smali", "C++20",
        "NeverDMobileTests",
        "--platform=android", "--timeout", "--max-files", "--max-bytes", "--json",
        "report.json", "schema_version", "input_code_files", "dex_count",
        "smali_count", "java_source_count", "java_sources",
        "--metadata-only", "check-neverd-mobile",
        "test_mobile_android_internal.py", "metadata/android-methods.json",
        "android_method_recovery", "declaration_only_method_count",
        "--list-classes", "--class-prefix", "--find-refs", "--query",
        "--exact", "--owner", "dex-envelope-and-class-identities",
        "dex-code-references", "pc_code_units", "target_utf16",
        "code_scan_complete", "defined_method_count", "scanned_method_count",
        "scanned_code_item_count", "matching_pool_entries", "JSON Lines",
        "2147483648", "20000", "300",
    ),
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
        "EVMUpstreamSemanticsPolicy.def",
        "audit_evm_opcode_metadata.py",
        "git fetch",
        "--depth=1",
        "--force",
        "bare",
        "authority",
        "local_docs",
        "GIT_CONFIG_NOSYSTEM",
        "GIT_CONFIG_GLOBAL",
        "GIT_CONFIG_*",
        "GIT_*",
        "GIT_ATTR_NOSYSTEM",
        "core.attributesFile",
        "core.hooksPath",
        "objects/info/alternates",
        "refs/replace",
        "GIT_NO_REPLACE_OBJECTS",
        "operation.undefined",
        "HasCost",
        "EVM_GETH_ACTIVE_WITHOUT_COST",
        "LookupInstructionSet",
        "base_min_stack",
        "net_stack_delta",
        "NeverDEVMDecoderPropertyTests",
        "KnownFunctionVariantInfo",
        "MaxAbstractInstructionTransfers",
        "EVMForkSemantics.def",
        "ExecutionFaultKind::ResourceExhausted",
        "HasPersistentStateSnapshot",
        *EVM_PUBLIC_BOUNDARY_TOKENS,
        *EVM_UPSTREAM_CLOSURE_TOKENS,
        *EVM_LOW_DIAGNOSTIC_TOKENS,
        *EVM_UPSTREAM_LIVE_RESULT_TOKENS,
        *EVM_UPSTREAM_SCHEMA3_TOKENS,
        *EVM_FINAL_RUNTIME_IR_TOKENS,
        *EVM_FUNCTION_SCOPE_GUIDE_TOKENS,
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
    "benchmark_mobile_inventory.py",
    "benchmark_mobile_references.py",
    "NEVERD_REFERENCE_TEST_BINARY",
    "--no-payload-lookalikes",
    "git fetch",
    "--depth=1",
    "--force",
    "bare",
    "authority",
    "local_docs",
    "GIT_CONFIG_NOSYSTEM",
    "GIT_CONFIG_GLOBAL",
    "GIT_CONFIG_*",
    "GIT_*",
    "GIT_ATTR_NOSYSTEM",
    "core.attributesFile",
    "core.hooksPath",
    "objects/info/alternates",
    "refs/replace",
    "GIT_NO_REPLACE_OBJECTS",
    "operation.undefined",
    "HasCost",
    "EVM_GETH_ACTIVE_WITHOUT_COST",
    "https://github.com/ethereum/go-ethereum.git",
    "audit_evm_opcode_metadata.py",
    "EVMUpstreamOpcodePolicy.def",
    "EVMUpstreamSemanticsPolicy.def",
    "LookupInstructionSet",
    "base_min_stack",
    "net_stack_delta",
    "NeverDEVMDecoderPropertyTests",
    "KnownFunctionVariantInfo",
    "MaxAbstractInstructionTransfers",
    "EVMForkSemantics.def",
    "ExecutionFaultKind::ResourceExhausted",
    "HasPersistentStateSnapshot",
    *EVM_PUBLIC_BOUNDARY_TOKENS,
    *EVM_UPSTREAM_CLOSURE_TOKENS,
    *EVM_LOW_DIAGNOSTIC_TOKENS,
    *EVM_UPSTREAM_LIVE_RESULT_TOKENS,
    *EVM_UPSTREAM_SCHEMA3_TOKENS,
    *EVM_FINAL_RUNTIME_IR_TOKENS,
    *EVM_FUNCTION_SCOPE_TEST_TOKENS,
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
UNPACK_REQUIRED_TOKENS = (
    "neverd unpack",
    "neverd_unpack_json",
    "Session.unpack",
    "windows-pe64-v1",
    "NeverDUnpackExecutionTests",
)
ENGLISH_DOCS = (
    Path("README.md"),
    Path("CONTRIBUTING.md"),
    Path("ATTRIBUTION.md"),
    Path("docs/README.md"),
    Path("docs/architecture.md"),
    Path("docs/memory-safety.md"),
    Path("docs/plugins.md"),
    Path("docs/python-plugins.md"),
    Path("docs/roadmap.md"),
    Path("docs/testing.md"),
    Path("docs/emulation.md"),
    Path("docs/cpu-execution.md"),
    Path("docs/process-emulation.md"),
    Path("docs/unpack.md"),
    Path("docs/solver.md"),
    Path("docs/windows-exception-reconstruction.md"),
    *(Path(f"docs/{stem}.md") for stem in GUIDE_STEMS),
    Path("docs/driver-emulation.md"),
    Path("docs/interpreter-recovery.md"),
    Path("docs/mobile.md"),
    *(Path(f"docs/{stem}.md") for stem in MACOS_GUIDE_STEMS),
)


def localized_paths(locale: str) -> tuple[Path, ...]:
    return (
        Path(f"docs/{locale}/project.md"),
        Path(f"docs/{locale}/CONTRIBUTING.md"),
        Path(f"docs/{locale}/ATTRIBUTION.md"),
        Path(f"docs/{locale}/README.md"),
        Path(f"docs/{locale}/architecture.md"),
        Path(f"docs/{locale}/memory-safety.md"),
        Path(f"docs/{locale}/plugins.md"),
        Path(f"docs/{locale}/python-plugins.md"),
        Path(f"docs/{locale}/roadmap.md"),
        Path(f"docs/{locale}/testing.md"),
        Path(f"docs/{locale}/windows-exception-reconstruction.md"),
        Path(f"docs/{locale}/evm.md"),
        Path(f"docs/{locale}/sbf.md"),
        Path(f"docs/{locale}/android.md"),
        Path(f"docs/{locale}/ios.md"),
        Path(f"docs/{locale}/driver-emulation.md"),
        Path(f"docs/{locale}/interpreter-recovery.md"),
        Path(f"docs/{locale}/emulation.md"),
        Path(f"docs/{locale}/cpu-execution.md"),
        Path(f"docs/{locale}/process-emulation.md"),
        Path(f"docs/{locale}/solver.md"),
        Path(f"docs/{locale}/mobile.md"),
        *(Path(f"docs/{locale}/{stem}.md") for stem in MACOS_GUIDE_STEMS),
        Path(f"docs/{locale}/unpack.md"),
    )


LOCALIZED_DOCS = tuple(path for locale in LOCALES for path in localized_paths(locale))
MARKDOWN_DOCS = ENGLISH_DOCS + LOCALIZED_DOCS + (
    Path("docs/driver-scheduling.md"),
    *(Path(f"docs/{locale}/driver-scheduling.md") for locale in LOCALES),
    *(Path(f"docs/{stem}.md") for stem in EMULATION_GUIDE_STEMS),
    *(Path(f"docs/{locale}/{stem}.md")
      for locale in LOCALES for stem in EMULATION_GUIDE_STEMS),
)
MOBILE_OVERVIEW_DOCS = (
    Path("docs/mobile.md"),
    *(Path(f"docs/{locale}/mobile.md") for locale in LOCALES),
)
PROHIBITED_STAGED_PREFIXES = ("docs/superpowers/",)
EVM_TESTS_CMAKE = Path("unittests/evm/CMakeLists.txt")

LINK_RE = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")
HEADING_RE = re.compile(r"^(#{1,6})\s+(.+?)\s*#*\s*$")
EXPLICIT_ANCHOR_RE = re.compile(
    r"<a\s+(?:[^>]*?\s)?(?:id|name)=[\"']([^\"']+)[\"'][^>]*>",
    re.IGNORECASE,
)
FENCE_RE = re.compile(r"^\s*(`{3,}|~{3,})")
SBF_TESTING_ROW_RE = re.compile(r"^\| `unittests/sbf` \|.*$", re.MULTILINE)
EVM_TEST_TARGET_RE = re.compile(
    r"add_neverd_unittest\(\s*(NeverDEVM[A-Za-z0-9_]*Tests)\b"
)


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
                encoding="utf-8",
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
                    encoding="utf-8",
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


@lru_cache(maxsize=1024)
def _numeric_evidence_pattern(token: str) -> re.Pattern[str] | None:
    if re.fullmatch(r"[0-9][0-9,']*(?:/[0-9][0-9,']*)?", token):
        pattern = rf"(?<![0-9A-Za-z]){re.escape(token)}(?![0-9A-Za-z])"
        return re.compile(pattern)
    return None


def token_present(text: str, token: str) -> bool:
    """Match numeric evidence as a count, not as a substring of a hash."""
    start = text.find(token)
    if start < 0:
        return False
    pattern = _numeric_evidence_pattern(token)
    # Keep the whole text for lookbehind, even when the first substring is
    # inside a hash and a later occurrence is the required standalone count.
    return pattern is None or pattern.search(text, start) is not None


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


def validate_language_selector(
    path: Path,
    stem: str,
    locale: str | None,
    errors: list[str],
    view: RepositoryView,
) -> None:
    """Require exact Markdown destinations for every guide language link."""
    first_line = view.read_text(path).splitlines()[0]
    actual = set(LINK_RE.findall(first_line))
    if locale is None:
        expected = {f"{stem}.md", *(f"{item}/{stem}.md" for item in LOCALES)}
    else:
        expected = {
            f"../{stem}.md",
            f"{stem}.md",
            *(f"../{item}/{stem}.md" for item in LOCALES if item != locale),
        }
    missing = sorted(expected - actual)
    if missing:
        report(
            errors,
            f"{display_path(path)}: language selector is missing exact link targets: "
            + ", ".join(missing),
        )


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


def evm_test_targets(errors: list[str], view: RepositoryView) -> tuple[str, ...]:
    """Return every registered EVM unit-test target in declaration order."""

    targets = EVM_TEST_TARGET_RE.findall(view.read_text(EVM_TESTS_CMAKE))
    if not targets:
        report(errors, f"{display_path(EVM_TESTS_CMAKE)}: no EVM test targets found")
        return ()
    duplicates = sorted(target for target in set(targets) if targets.count(target) != 1)
    if duplicates:
        report(
            errors,
            f"{display_path(EVM_TESTS_CMAKE)}: duplicate EVM test targets: "
            + ", ".join(duplicates),
        )
    return tuple(dict.fromkeys(targets))


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


def _comparison_claim_segments(
    text: str,
    all_display_names: tuple[str, ...],
) -> tuple[tuple[str, tuple[re.Match[str], ...]], ...]:
    registered_name_re = re.compile(
        "|".join(
            re.escape(name) for name in sorted(all_display_names, key=len, reverse=True)
        )
    )
    prose = without_markdown_fences(text)
    return tuple(
        (segment, tuple(registered_name_re.finditer(segment)))
        for segment in re.split(r"\n(?=- )|\n{2,}", prose)
    )


def _comparison_revision_is_paired(
    segments: tuple[tuple[str, tuple[re.Match[str], ...]], ...],
    display_name: str,
    revision: str,
) -> bool:
    candidates: list[str] = []
    for segment, registered_names in segments:
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


def comparison_tool_revision_is_paired(
    text: str,
    display_name: str,
    revision: str,
    all_display_names: tuple[str, ...],
) -> bool:
    """Require every local name-then-revision claim to use the right object."""
    return _comparison_revision_is_paired(
        _comparison_claim_segments(text, all_display_names), display_name, revision
    )


def validate_sbf_comparison_pairs(
    errors: list[str],
    path: Path,
    text: str,
    tools: tuple[tuple[str, str, str], ...],
) -> None:
    """Keep every display name bound to its own pinned repository object."""
    display_names = tuple(display_name for _id, display_name, _revision in tools)
    # Parse this document once for all tools. The result belongs to this
    # validation call, so another view or a changed document is read afresh.
    segments = _comparison_claim_segments(text, display_names)
    for _tool_id, display_name, revision in tools:
        # The ordinary token check owns missing-name/revision diagnostics. Pair
        # checking is the stronger guard once both independent tokens exist.
        if display_name not in text or revision not in text:
            continue
        if _comparison_revision_is_paired(segments, display_name, revision):
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


def validate_mobile_readme_entries(
    errors: list[str], view: RepositoryView, stem: str = "android"
) -> None:
    """Keep the mobile CLI row linked to each README's platform guide."""
    links = (
        (Path("README.md"), f"docs/{stem}.md"),
        *(
            (Path(f"docs/{locale}/project.md"), f"{stem}.md")
            for locale in LOCALES
        ),
    )
    for path, target in links:
        prose = without_markdown_fences(view.read_text(path))
        rows = re.findall(
            r"^[ \t]*\|[ \t]*`mobile`[ \t]*\|[^\n]*\|[ \t]*$",
            prose,
            re.MULTILINE,
        )
        if len(rows) != 1:
            report(
                errors,
                f"{display_path(path)}: expected one mobile CLI table row, "
                f"found {len(rows)}",
            )
        elif target not in {link_target(raw) for raw in LINK_RE.findall(rows[0])}:
            report(
                errors,
                f"{display_path(path)}: mobile CLI table row must link to {target!r}",
            )


def validate_mobile_examples(
    errors: list[str], view: RepositoryView, stem: str = "android"
) -> None:
    """Preserve fenced examples and the overview's inline CLI query commands."""
    languages = {"sh", "bash", "shell", "powershell", "json"}

    def examples(path: Path) -> tuple[tuple[str, str], ...]:
        return tuple(
            (info, body)
            for info, body in markdown_fenced_blocks(view.read_text(path))
            if info in languages
        )

    def inline_commands(path: Path) -> list[str]:
        prose = without_markdown_fences(view.read_text(path))
        return sorted(re.findall(r"`(neverd mobile [^`\n]+)`", prose))

    expected = examples(Path(f"docs/{stem}.md"))
    expected_inline = inline_commands(Path("docs/mobile.md")) if stem == "mobile" else None
    for locale in LOCALES:
        path = Path(f"docs/{locale}/{stem}.md")
        if examples(path) != expected:
            report(
                errors,
                f"{display_path(path)}: {stem} command and JSON fenced blocks "
                f"must match docs/{stem}.md exactly",
            )
        if expected_inline is not None and inline_commands(path) != expected_inline:
            report(
                errors,
                f"{display_path(path)}: mobile inline CLI commands "
                "must match docs/mobile.md exactly",
            )


def validate_mobile_overview_links(errors: list[str], view: RepositoryView) -> None:
    """Keep every overview reachable locally and across the language selector."""
    for path in MOBILE_OVERVIEW_DOCS:
        prose = without_markdown_fences(view.read_text(path))
        targets = {link_target(raw) for raw in LINK_RE.findall(prose)}
        for overview in MOBILE_OVERVIEW_DOCS:
            target = posixpath.relpath(overview.as_posix(), path.parent.as_posix())
            if target not in targets:
                report(
                    errors,
                    f"{display_path(path)}: mobile language selector must link to {target!r}",
                )
        for name in ("README.md", "android.md", "ios.md"):
            entry = path.parent / name
            prose = without_markdown_fences(view.read_text(entry))
            targets = {link_target(raw) for raw in LINK_RE.findall(prose)}
            if "mobile.md" not in targets:
                report(
                    errors,
                    f"{display_path(entry)}: must link to its local mobile.md overview",
                )


def validate_mobile_native_runtime(errors: list[str], view: RepositoryView) -> None:
    """Reject obsolete helper deployment and interpreter options in mobile docs."""
    guides = tuple(
        Path("docs") / directory / f"{stem}.md"
        for directory in ("", *LOCALES)
        for stem in ("android", "ios")
    )
    overviews = MOBILE_OVERVIEW_DOCS
    entries = (
        Path("README.md"), Path("docs/README.md"),
        *(Path(f"docs/{locale}/{name}.md")
          for locale in LOCALES for name in ("project", "README")),
    )
    obsolete = (
        "--python", "NEVERD_PYTHON", "`mobile/`", "--swift-demangle",
        "NEVERD_SWIFT_DEMANGLE", "xcrun --find swift-demangle",
        "--jadx", "NEVERD_JADX", "test_mobile_android_backend.py",
    )
    for path in (*guides, *overviews, *entries):
        text = view.read_text(path)
        for token in obsolete:
            if token in text:
                report(errors, f"{display_path(path)}: obsolete mobile runtime token {token!r}")
        if path.name == "ios.md" or path in overviews:
            prose = without_markdown_fences(text)
            if "LLVMSwiftDemangle" not in prose:
                report(errors, f"{display_path(path)}: builtin Swift signatures require LLVM component prose")
        if path in guides or path in overviews:
            if "C++20" not in without_markdown_fences(text):
                report(errors, f"{display_path(path)}: native mobile runtime requires C++20 prose")
        # Python plugin/SDK documentation remains valid elsewhere in a README.
        # Interpreter versions in the actual mobile introduction are obsolete.
        for line in without_markdown_fences(text).splitlines():
            mobile_entry = "neverd mobile" in line or (
                "android.md)" in line and line.lstrip().startswith("|")
            )
            python_component = path in guides and re.match(r"\s*\|\s*Python\s*\|", line)
            if python_component or (mobile_entry and re.search(r"\bPython\s+3\.10", line)):
                report(errors, f"{display_path(path)}: mobile introduction requires the native CLI, not Python 3.10")


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


def driver_report_profile(view: RepositoryView) -> str:
    source = view.read_text(Path("include/neverd/emulation/DriverProfileStrings.def"))
    profiles = re.findall(
        r'NEVERD_DRIVER_PROFILE_STRING\(ReportProfile,\s*"([^"]+)"\)', source
    )
    if len(profiles) != 1:
        raise ValueError("driver profile must have one ReportProfile definition")
    return profiles[0]


def validate_driver_framework_contract(
    path: Path, implemented: list[str], errors: list[str], view: RepositoryView
) -> None:
    """Validate the API inventory and caller-context routing in their own prose."""
    paragraphs = without_markdown_fences(view.read_text(path)).split("\n\n")
    inventories = []
    for paragraph in paragraphs:
        symbols = re.findall(r"`([^`]+)`", paragraph)
        if symbols and symbols[0] == implemented[0]:
            inventories.append((paragraph, set(symbols)))
    if len(inventories) != 1:
        report(errors, f"{display_path(path)}: expected one dedicated KMDF API inventory")
    else:
        _, documented = inventories[0]
        for symbol in sorted(set(implemented) - documented):
            report(errors, f"{display_path(path)}: KMDF API inventory missing {symbol!r}")
        for symbol in sorted(documented - set(implemented)):
            report(errors, f"{display_path(path)}: KMDF API inventory has unmodeled {symbol!r}")

    caller_tokens = (
        "`WdfDeviceInitSetIoInCallerContextCallback`",
        "`WdfDeviceEnqueueRequest`",
        "`WdfRequestRetrieveUnsafeUserInputBuffer`",
    )
    inventory_paragraphs = {paragraph for paragraph, _ in inventories}
    caller_paragraphs = [
        paragraph for paragraph in paragraphs
        if paragraph not in inventory_paragraphs
        and all(token in paragraph for token in caller_tokens)
    ]
    if len(caller_paragraphs) != 1:
        report(errors, f"{display_path(path)}: expected one caller-context routing paragraph")
    elif "`WdfDeviceConfigureRequestDispatching`" not in caller_paragraphs[0]:
        report(
            errors,
            f"{display_path(path)}: caller-context routing paragraph missing "
            "'WdfDeviceConfigureRequestDispatching'",
        )


def validate_driver_documents(errors: list[str], view: RepositoryView) -> None:
    """Keep execution examples, supported exports and locale entry points aligned."""
    english = Path("docs/driver-emulation.md")
    guides = (
        english,
        *(Path(f"docs/{locale}/driver-emulation.md") for locale in LOCALES),
    )
    source = view.read_text(english)
    examples = re.findall(r"```[^\n]*\n(.*?)```", source, re.DOTALL)
    register_example = Path("docs/examples/driver-register-bank-scenario.json")
    public_tests = view.read_text(
        Path("unittests/emulation/DriverScenarioPublicTests.cpp")
    )
    public_scenario = re.search(
        r'CAPIAndCLIExecuteRegisterBanksAcrossStopAndRestart.*?'
        r'const std::string Scenario = R"\((.*?)\)";',
        public_tests,
        re.DOTALL,
    )
    try:
        matches_public = public_scenario is not None and json.loads(
            view.read_text(register_example)
        ) == json.loads(public_scenario.group(1))
    except (json.JSONDecodeError, OSError):
        matches_public = False
    if not matches_public:
        report(errors, "driver register-bank example differs from public execution scenario")
    interrupt_example = Path("docs/examples/driver-interrupt-scenario.json")
    interrupt_scenario = re.search(
        r'CAPIAndCLIExecuteExplicitInterruptAndDpcCompletion.*?'
        r'const std::string Scenario = R"\((.*?)\)";',
        public_tests,
        re.DOTALL,
    )
    try:
        matches_interrupt = interrupt_scenario is not None and json.loads(
            view.read_text(interrupt_example)
        ) == json.loads(interrupt_scenario.group(1))
    except (json.JSONDecodeError, OSError):
        matches_interrupt = False
    if not matches_interrupt:
        report(errors, "driver interrupt example differs from public execution scenario")
    dma_example = Path("docs/examples/driver-dma-scenario.json")
    dma_scenario = re.search(
        r'CAPIAndCLIObserveDmaRamBeforeExplicitInterrupt.*?'
        r'const std::string Scenario\s*=\s*R"dma\((.*?)\)dma";',
        public_tests,
        re.DOTALL,
    )
    try:
        matches_dma = dma_scenario is not None and json.loads(
            view.read_text(dma_example)
        ) == json.loads(dma_scenario.group(1))
    except (json.JSONDecodeError, OSError):
        matches_dma = False
    if not matches_dma:
        report(errors, "driver DMA example differs from public execution scenario")
    channel_example = Path("docs/examples/driver-dma-channel-scenario.json")
    channel_scenario = re.search(
        r'CAPIAndCLIFlushChannelFragmentsBeforeCompletion.*?'
        r'const std::string Scenario\s*=\s*R"channel\((.*?)\)channel";',
        public_tests,
        re.DOTALL,
    )
    try:
        matches_channel = channel_scenario is not None and json.loads(
            view.read_text(channel_example)
        ) == json.loads(channel_scenario.group(1))
    except (json.JSONDecodeError, OSError):
        matches_channel = False
    if not matches_channel:
        report(errors, "driver DMA channel example differs from public execution scenario")
    seh_example = Path("docs/examples/driver-seh-scenario.json")
    seh_scenario = re.search(
        r'CAPIAndCLIResumeGenuineConstantExceptionHandler.*?'
        r'const std::string Scenario\s*=\s*R"seh\((.*?)\)seh";',
        public_tests,
        re.DOTALL,
    )
    try:
        matches_seh = seh_scenario is not None and json.loads(
            view.read_text(seh_example)
        ) == json.loads(seh_scenario.group(1))
    except (json.JSONDecodeError, OSError):
        matches_seh = False
    if not matches_seh:
        report(errors, "driver SEH example differs from public execution scenario")
    exports = re.findall(
        r"NEVERD_KERNEL_API\((\w+),",
        view.read_text(Path("lib/emulation/os/windows/kernel/KernelAPIs.def")),
    )
    exports = [name for name in exports if name != "Name"]
    exports += re.findall(
        r"NEVERD_KERNEL_REGISTRY_API\((\w+),",
        view.read_text(Path("lib/emulation/os/windows/kernel/KernelRegistryAPIs.def")),
    )
    exports += re.findall(
        r"NEVERD_KERNEL_DISPATCHER_API\((\w+),",
        view.read_text(Path("lib/emulation/os/windows/kernel/KernelDispatcherAPIs.def")),
    )
    framework_exports = re.findall(
        r"^NEVERD_FRAMEWORK_API\((\w+),",
        view.read_text(Path("lib/emulation/os/windows/kernel/KernelFrameworkAPIs.def")),
        re.MULTILINE,
    )
    if not framework_exports:
        report(errors, "implemented KMDF API inventory is empty")
    exports += framework_exports
    # The 458-entry identity inventory also names unmodeled traps. Only the
    # implemented table and loader ABI inventories establish documented APIs.
    for inventory, macro in (
        ("KernelInterruptAPIs.def", "NEVERD_KERNEL_INTERRUPT_API"),
        ("KernelPoFxAPIs.def", "NEVERD_KERNEL_POFX_API"),
        ("KernelFrameworkLoaderAPIs.def", "NEVERD_FRAMEWORK_LOADER_API"),
    ):
        exports += re.findall(
            rf"{macro}\((\w+),",
            view.read_text(Path("lib/emulation/os/windows/kernel") / inventory),
        )
    # These are adapter-bound indirect methods, not kernel import names. Only
    # implemented entries belong to the supported API documentation inventory.
    exports += re.findall(
        r"NEVERD_DMA_OPERATION\((\w+),[^\n]*, true\)",
        view.read_text(Path("lib/emulation/os/windows/kernel/KernelDMAOperations.def")),
    )
    required = (
        "NEVERD_ENABLE_DRIVER_EMULATION=ON",
        "BUILD_TESTING",
        "--scenario",
        "METHOD_BUFFERED",
        "METHOD_IN_DIRECT",
        "METHOD_OUT_DIRECT",
        "METHOD_NEITHER",
        "IoAllocateIrp", "IoFreeIrp", "ChargeQuota=FALSE",
        "IRP_MJ_INTERNAL_DEVICE_CONTROL", "internal_ioctl", "driver_allocated_irp",
        "NEVERD_WDM_OWNED_IRP_FIXTURE", "NEVERD_WDM_OWNED_IRP_CFG_FIXTURE",
        "driver-owned-irp-scenario.json",
        "driver-d2-power-scenario.json", "DeviceLifecycle::validateDevicePowerRequest",
        "IRP_MN_WAIT_WAKE", "PIRP", "REQUEST_POWER_COMPLETE",
        "STATUS_DEVICE_BUSY", "STATUS_INVALID_DEVICE_STATE",
        "IOCTL_INTERNAL_USB_SUBMIT_IDLE_NOTIFICATION",
        "NEVERD_WDM_WAIT_WAKE_FIXTURE", "NEVERD_WDM_WAIT_WAKE_CFG_FIXTURE",
        "driver-wdm-wait-wake-scenario.json",
        "driver-wdm-elevated-power-scenario.json", "CR8",
        "usb_idle", "usb_idle_permission", "usb_idle_members", "remote_wake",
        "independent_function", "composite_parent", "composite_function",
        "d2_irp", "completion_cause", "STATUS_POWER_STATE_INVALID",
        "NEVERD_WDM_USB_IDLE_FIXTURE", "NEVERD_WDM_USB_IDLE_CFG_FIXTURE",
        "NEVERD_KMDF_USB_IDLE_FIXTURE", "NEVERD_KMDF_USB_IDLE_CFG_FIXTURE",
        "driver-kmdf-usb-idle-scenario.json", "device_wake",
        "driver-kmdf-usb-pofx-scenario.json", "STATUS_WDF_BUSY",
        "WdfDeviceEnqueueRequest",
        "framework_usb_idle", "DriverManagedIdleTimeout",
        "WdfDeviceConfigureRequestDispatching",
        "MOVLHPS",
        "MOVS/REP MOVS",
        "MXCSR",
        "XSAVE",
        "driver-wdm-usb-idle-scenario.json",
        "DxState", "PowerDeviceMaximum", "IdleUsbSelectiveSuspend",
        "KMDF",
        "UMDF",
        "KMDF 1.33",
        "CFG",
        "XFG",
        "NEVERD_KMDF_FIXTURE",
        "NEVERD_KMDF_CFG_FIXTURE",
        "NEVERD_KMDF_CONTROL_FIXTURE",
        "NEVERD_KMDF_CONTROL_CFG_FIXTURE",
        "NEVERD_WDM_STACK_FIXTURE",
        "NEVERD_WDM_STACK_CFG_FIXTURE",
        "NEVERD_WDM_PNP_FIXTURE",
        "NEVERD_WDM_PNP_CFG_FIXTURE",
        "configuration.pnp_devices",
        "parent_id", "parent_pdo",
        "ArmForWakeIfChildrenAreArmedForWake", "IndicateChildWakeOnParentWake",
        "EvtDeviceArmWakeFromSxWithReason",
        "wake_source_device_id", "wake_source_pdo",
        "NEVERD_KMDF_CHILD_WAKE_FIXTURE", "NEVERD_KMDF_CHILD_WAKE_CFG_FIXTURE",
        "driver-kmdf-child-wake-scenario.json",
        "service_name", "wake_capabilities", "power_policy_events",
        "d3cold", "enabled_by_default", "wake_s0", "wake_sx", "wake_capable",
        "component_idle_state", "power_not_required",
        "SystemManagedIdleTimeout", "SystemManagedIdleTimeoutWithHint",
        "NEVERD_WDM_POFX_FIXTURE", "NEVERD_WDM_POFX_CFG_FIXTURE",
        "WdfDeviceWdmAssignPowerFrameworkSettings", "425",
        "EvtDeviceWdmPostPoFxRegisterDevice", "EvtDeviceWdmPrePoFxUnregisterDevice",
        "PoFxDeviceFlags", "DirectedPoFxEnabled", "WdfFalse",
        "NEVERD_KMDF_POFX_FIXTURE", "NEVERD_KMDF_POFX_CFG_FIXTURE",
        "driver-pofx-scenario.json", "driver-kmdf-pofx-scenario.json",
        "ReportInactiveOnPowerDown", "CanWakeDevice", "GS:[0x188]",
        "device_epoch", "framework_wait_wake",
        "driver-kmdf-power-policy-scenario.json",
        "resource_free",
        "initial_device_power",
        "initial_system_power",
        "device_id",
        "bus_completion",
        "delay_100ns",
        "query_remove",
        "cancel_remove",
        "query_stop",
        "cancel_stop",
        "surprise_removal",
        "STATUS_RESOURCE_REQUIREMENTS_CHANGED",
        "0x119",
        "add_device_status",
        "provider_present",
        "bus_received_at_100ns",
        "bus_completed_at_100ns",
        "add_device:<ID>",
        "STATUS_NOT_SUPPORTED",
        "STATUS_SUCCESS",
        "IoCopyCurrentIrpStackLocationToNext",
        "IoSkipCurrentIrpStackLocation",
        "IoSetCompletionRoutine",
        "STATUS_MORE_PROCESSING_REQUIRED",
        "FILE_OBJECT.DeviceObject",
        "ReferenceCount",
        "WDF_IO_QUEUE_CONFIG",
        "WDF_REQUEST_PARAMETERS",
        "D:P(A;;GA;;;WD)",
        "PASSIVE_LEVEL",
        "DISPATCH_LEVEL",
        "KernelMode",
        "Executive",
        "CPU0",
        "Increment=0",
        "Wait=FALSE",
        "STATUS_PENDING",
        "DelayedWorkQueue",
        "callback:N",
        "model_error",
        "STATUS_INVALID_DEVICE_REQUEST",
        "RegistryPath",
        "security_cookie",
        "neverd_emulate_driver_json",
        "neverd_emulate_driver_scenario_json",
        "neverd_driver_options_v1",
        "neverd_free_string",
        "neverd_last_error",
        "scenario_success",
        "output_hex",
        "information_hex",
        "configuration.registry",
        driver_report_profile(view), "defer_callback_drain",
        "PsCreateSystemThread", "PsTerminateSystemThread",
        "ObReferenceObjectByHandle", "ObfDereferenceObject",
        "KeEnterCriticalRegion", "KeLeaveCriticalRegion",
        "KeEnterGuardedRegion", "KeLeaveGuardedRegion",
        "KeAreApcsDisabled", "KeAreAllApcsDisabled",
        "WdfRequestRetrieveUnsafeUserInputBuffer",
        "WdfRequestRetrieveUnsafeUserOutputBuffer",
        "WdfRequestProbeAndLockUserBufferForRead",
        "WdfRequestProbeAndLockUserBufferForWrite", "WdfMemoryGetBuffer",
        "WdfRequestForwardToIoQueue", "WdfRequestRequeue",
        "WdfDeviceInitSetExclusive", "DO_EXCLUSIVE",
        "WdfInterruptCreate", "WdfInterruptQueueDpcForIsr",
        "WdfInterruptQueueWorkItemForIsr", "WdfInterruptSynchronize",
        "WdfInterruptAcquireLock", "WdfInterruptReleaseLock",
        "WdfInterruptEnable", "WdfInterruptDisable",
        "WdfInterruptWdmGetInterrupt", "WdfInterruptGetInfo",
        "WdfInterruptGetDevice", "WDFSPINLOCK", "WDFWAITLOCK",
        "WdfSpinLockCreate", "WdfSpinLockAcquire", "WdfSpinLockRelease",
        "WdfWaitLockCreate", "WdfWaitLockAcquire", "WdfWaitLockRelease",
        "WdfObjectAcquireLock", "WdfObjectReleaseLock",
        "__GSHandlerCheck", "__GSHandlerCheck_SEH",
        "WdfDeviceInitSetFileObjectConfig", "WdfFileObjectGetDevice",
        "WdfFileObjectWdmGetFileObject", "EvtDeviceFileCreate",
        "EvtFileCleanup", "EvtFileClose",
        "EvtIoCanceledOnQueue",
        "WdfIoQueueReadyNotify",
        "WdfIoQueueFindRequest", "WdfIoQueueRetrieveFoundRequest",
        "WdfIoQueueRetrieveNextRequest", "WdfIoQueueGetState",
        "WdfIoQueuePnpHeld", "WdfUseDefault",
        "EvtIoStop", "EvtIoResume", "WdfRequestStopAcknowledge",
        "CM_PARTIAL_RESOURCE_DESCRIPTOR", "MmMapIoSpace",
        "WdfIoQueueStop", "WdfIoQueueStopSynchronously", "WdfIoQueueStart",
        "WdfIoQueueDrain", "WdfIoQueueDrainSynchronously",
        "WdfIoQueuePurge", "WdfIoQueuePurgeSynchronously",
        "WdfIoQueueStopAndPurge", "WdfIoQueueStopAndPurgeSynchronously",
        "STATUS_WDF_PAUSED",
        "asynchronous_file",
        "ExRaiseStatus", "ExRaiseAccessViolation", "ExRaiseDatatypeMisalignment",
        "__C_specific_handler", "EXCEPTION_EXECUTE_HANDLER", "GetExceptionCode",
        "STATUS_ACCESS_VIOLATION", "STATUS_DATATYPE_MISALIGNMENT", "APC_LEVEL",
        "NEVERD_WDM_SEH_FIXTURE", "NEVERD_WDM_SEH_CFG_FIXTURE",
        "driver-seh-scenario.json", "ProbeForRead", "ProbeForWrite",
        "user_input_access", "user_output_access", "no_access",
        "user_unmap_after_dispatch", "requestor_process_id",
        "requestor_exit_after_dispatch", "IoGetRequestorProcessId",
        "IoGetRequestorProcess", "IoGetCurrentProcess", "PsGetProcessId",
        "PsGetCurrentProcessId", "KeStackAttachProcess",
        "KeUnstackDetachProcess", "KAPC_STATE",
        "KeInitializeSpinLock", "KeAcquireSpinLockRaiseToDpc",
        "KeReleaseSpinLock", "KeAcquireSpinLockAtDpcLevel",
        "KeReleaseSpinLockFromDpcLevel", "KeTryToAcquireSpinLockAtDpcLevel",
        "KeInitializeSemaphore", "KeReleaseSemaphore",
        "KeReadStateSemaphore", "STATUS_SEMAPHORE_LIMIT_EXCEEDED",
        "KeInitializeMutex", "KeReleaseMutex", "KeReadStateMutex",
        "STATUS_MUTANT_NOT_OWNED",
        "KfRaiseIrql", "KeLowerIrql",
        "configuration.user_page_access", "IRP.UserBuffer",
        "DriverDMA.h", "DriverDMA.def", "dma_events", "dma_transfers",
        "address_bits", "maximum_length", "map_registers", "alignment",
        "logical_base", "logical_length", "scatter_gather", "logical_address",
        "read_memory", "write_memory", "data_hex", "completed_at_100ns",
        "mapping", "adapter", "failure_reason", "DmaWritable",
        "NEVERD_WDM_DMA_FIXTURE", "NEVERD_WDM_DMA_CFG_FIXTURE",
        "driver-dma-scenario.json",
        "AllocateAdapterChannel", "MapTransfer", "FlushAdapterBuffers",
        "FreeMapRegisters", "KeFlushIoBuffers", "CurrentIrp",
        "NEVERD_WDM_DMA_CHANNEL_FIXTURE", "NEVERD_WDM_DMA_CHANNEL_CFG_FIXTURE",
        "driver-dma-channel-scenario.json",
        "DriverInterrupts.h", "DriverInterrupts.def", "interrupt_events",
        "after_100ns", "interrupt_id", "raw_vector", "raw_level", "raw_affinity",
        "translated_vector", "translated_level", "translated_affinity",
        "latched", "device_exclusive", "source_request_index", "event_index",
        "due_at_100ns", "occurred_at_100ns", "delivered_at_100ns", "returned_at_100ns",
        "interrupt_object", "return_value", "claimed", "undelivered_reason",
        "NEVERD_WDM_INTERRUPT_FIXTURE", "NEVERD_WDM_INTERRUPT_CFG_FIXTURE",
        "driver-interrupt-scenario.json",
        "register_bank", "resources", "raw_start", "translated_start", "registers",
        "read_only", "read_write", "DriverResources.h", "DriverResources.def",
        "CM_RESOURCE_LIST", "NEVERD_WDM_RESOURCE_FIXTURE",
        "NEVERD_WDM_RESOURCE_CFG_FIXTURE", "driver-register-bank-scenario.json",
        "STATUS_DELETE_PENDING", "Driver Verifier",
        "NEVERD_WDM_REMOVE_LOCK_FIXTURE", "NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE",
        "NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE", "NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE",
        "initial_reported_device_power", "requested_device_power",
        "power_type", "power_state", "power_action", "system_context",
        "response_index", "origin", "requested_device_object",
        "device_state_before", "device_state_after",
        "system_state_before", "system_state_after", "reported_device_power",
        "DO_POWER_PAGABLE", "DO_POWER_INRUSH",
        "NEVERD_WDM_POWER_FIXTURE", "NEVERD_WDM_POWER_CFG_FIXTURE",
        "driver-power-scenario.json",
        "STATUS_INTERNAL_ERROR",
        "WdfSynchronizationScopeNone",
        "ByteCount",
        "cancel_after_100ns",
        "cancel_requested_at_100ns",
        "STATUS_CANCELLED",
        "validate_windows_driver_sample.py",
        "sioctl-validation.json",
        "validate_zero_driver_sample.py",
        "zero-validation.json",
        *(f"`{name}`" for name in exports),
    )
    for guide in guides:
        require_tokens(guide, required, errors, view)
        if framework_exports:
            validate_driver_framework_contract(guide, framework_exports, errors, view)
        text = view.read_text(guide)
        if re.findall(r"```[^\n]*\n(.*?)```", text, re.DOTALL) != examples:
            report(
                errors,
                f"{display_path(guide)}: driver examples differ from the English execution contract",
            )
        selectors = tuple(
            posixpath.relpath(target.as_posix(), guide.parent.as_posix())
            for target in guides
        )
        require_tokens(guide, selectors, errors, view)
        require_tokens(guide.parent / "README.md", ("(driver-emulation.md)",), errors, view)
        require_tokens(
            guide.parent / "architecture.md",
            ("NEVERD_ENABLE_DRIVER_EMULATION", "`lib/emulation`", "(driver-emulation.md)",
             "KernelModelDeviceStack", "KernelModelIRPStack", "KernelGuestCall",
             "STATUS_MORE_PROCESSING_REQUIRED", "DriverPnp.h", "DeviceLifecycle.def",
             "devicePnpFinalStatusError",
             "KernelModelPnpDevices", "KernelModelPnpRequests", "KernelModelPnpCompletion",
             "KernelRemoveLocks", "DriverResources.h", "DriverResources.def",
             "KernelMMIO", "KernelModelResources", "UnicornBackend",
             "KernelResources", "KernelInterrupts", "DriverInterrupts.h",
             "DriverDMA.h", "DriverDMA.def", "KernelPhysicalMemory", "KernelDMA",
             "KernelDMAEvents", "KernelModelPhysicalMemory", "KernelModelDMA",
             "KernelModelDMATransfers", "DmaWritable",
             "KernelDMAChannels", "KernelModelDMAChannels", "DMAAdapterControl",
             "KernelGuestException", "X64SEH",
             "KernelModelInterruptEvents", "KernelModelInterrupts",
             "DriverPower.def", "DriverPowerOperation", "KernelModelPowerRequests",
             "KernelModelPowerCompletion"),
            errors,
            view,
        )
        require_tokens(
            guide.parent / "testing.md",
            (
                "NeverDDriverEmulationTests",
                "NeverDDriverEmulationPublicTests",
                "'^NeverDDriverEmulation'",
                "(driver-emulation.md)",
                "KernelSEHTests.cpp", "KernelExceptionTests.cpp",
                "DriverWDMSEHTests.cpp", "NEVERD_WDM_SEH_FIXTURE",
                "NEVERD_WDM_SEH_CFG_FIXTURE", "driver-seh-scenario.json",
                "test_driver_seh_integration.py", "NEVERD_TEST_WDM_SEH_FIXTURE",
                "NEVERD_TEST_WDM_SEH_CFG_FIXTURE",
                "KernelDeviceStackTests.cpp",
                "KernelIRPStackTests.cpp",
                "DriverWDMStackTests.cpp",
                "NEVERD_WDM_STACK_FIXTURE",
                "NEVERD_WDM_STACK_CFG_FIXTURE",
                "DriverResourceScenarioTests.cpp", "KernelMMIOTests.cpp",
                "DriverDMAScenarioTests.cpp", "KernelPhysicalMemoryTests.cpp",
                "BackendBackingTests.cpp", "KernelDMATests.cpp", "KernelDMABridgeTests.cpp",
                "SchedulerDMATests.cpp", "DriverWDMDMATests.cpp",
                "KernelDMAChannelTests.cpp", "KernelDMAChannelBridgeTests.cpp",
                "DriverWDMDMAChannelTests.cpp", "NEVERD_WDM_DMA_CHANNEL_FIXTURE",
                "NEVERD_WDM_DMA_CHANNEL_CFG_FIXTURE", "driver-dma-channel-scenario.json",
                "test_driver_dma_channel_integration.py",
                "NEVERD_TEST_WDM_DMA_CHANNEL_FIXTURE",
                "NEVERD_TEST_WDM_DMA_CHANNEL_CFG_FIXTURE",
                "NEVERD_WDM_DMA_FIXTURE", "NEVERD_WDM_DMA_CFG_FIXTURE",
                "driver-dma-scenario.json", "test_driver_dma_integration.py",
                "NEVERD_TEST_LIBNEVERD", "NEVERD_TEST_WDM_DMA_FIXTURE",
                "NEVERD_TEST_WDM_DMA_CFG_FIXTURE",
                "DriverInterruptScenarioTests.cpp", "KernelInterruptsTests.cpp",
                "KernelInterruptBridgeTests.cpp", "SchedulerInterruptTests.cpp",
                "DriverWDMInterruptTests.cpp", "NEVERD_WDM_INTERRUPT_FIXTURE",
                "NEVERD_WDM_INTERRUPT_CFG_FIXTURE", "driver-interrupt-scenario.json",
                "KernelMMIOFailureTests.cpp",
                "KernelResourceBridgeTests.cpp", "UnicornMMIOTests.cpp",
                "DriverWDMResourceTests.cpp", "NEVERD_WDM_RESOURCE_FIXTURE",
                "NEVERD_WDM_RESOURCE_CFG_FIXTURE", "driver-register-bank-scenario.json",
                "KernelRemoveLocksTests.cpp", "KernelRemoveLockBridgeTests.cpp",
                "DriverWDMRemoveLockTests.cpp", "NEVERD_WDM_REMOVE_LOCK_FIXTURE",
                "NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE", "NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE",
                "NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE",
                "DriverPowerScenarioTests.cpp", "KernelPowerRequestTests.cpp",
                "KernelPowerCompletionTests.cpp", "DriverWDMPowerTests.cpp",
                "NEVERD_WDM_POWER_FIXTURE", "NEVERD_WDM_POWER_CFG_FIXTURE",
                "DriverPnpScenarioTests.cpp",
                "KernelPnpDeviceTests.cpp",
                "KernelPnpRequestTests.cpp",
                "KernelPnpCompletionTests.cpp",
                "DriverWDMPnpTests.cpp",
                "NEVERD_WDM_PNP_FIXTURE",
                "NEVERD_WDM_PNP_CFG_FIXTURE",
            ),
            errors,
            view,
        )


README_SECTION_MARKER = re.compile(r'^<!-- i18n-section: ([a-z0-9-]+) -->$', re.MULTILINE)
README_SOURCE_REVISION = re.compile(r'^<!-- i18n-source: ([0-9a-f]{64}) -->$', re.MULTILINE)
SYNCHRONIZED_GUIDES = (
    "process-emulation", "memory-safety", "solver", "interpreter-recovery",
)
GUIDE_SECTION_CONTRACTS = {
    ("interpreter-recovery", "machine-state"): (
        "modelInterpreterMachineStateX64", "InterpreterMachineStateModel",
        "MaxOperations", "checkLowIRLoopRefinement",
        "modelLLVMInterpreterMachineStateX64",
        "llvmInterpreterMachineStateContract", "LLVMInterpreterDefinednessOffset",
        "MaxInputItems", "MaxBlocks", "MaxWork", "initializes",
        "prepareInterpreterLLVMRefinement", "checkBinaryLLVMRefinement",
        "MaxIRBytes", "MaxMachineStateOperations", "MaxPreparationItems",
    ),
    ("interpreter-recovery", "loop-proposals"): (
        "pairLowIRLoopRefinementPlans", "LowIRLoopCutpointPair", "SharedInputs",
        "CandidatePrefix", "UseEntryPrefix", "GeneralizeEntryPrefix",
        "MaxMetadata", "checkLowIRLoopRefinement",
        "inferAndCheckLowIRLoopRefinement", "LowIRLoopAlignmentLimits",
        "MaxSolverQueries", "MaxSearchWork", "MaxCandidateAttempts",
        "MaxPairingAttempts", "MaxCuts",
        "MaxCutpointAttempts", "MaxRankCandidates",
    ),
    ("process-emulation", "linux-semantics"): (
        "mmap", "mprotect", "munmap", "brk", "PROT_NONE", "ENOMEM",
        "MAP_PRIVATE | MAP_ANONYMOUS", "PT_DYNAMIC", "PT_TLS", "arch_prctl",
    ),
    ("memory-safety", "formatted-output"): (
        "snprintf", "vsnprintf", "_chk", "%%", "format_string",
    ),
    ("memory-safety", "stack-initialization"): ("uninitialized_read",),
    ("memory-safety", "strict-publication"): (
        "binary-sanitizer-v1", "neverd_session_sanitize",
        "neverd patch --sanitize=strict", "Session.sanitize",
        "neverd_sanitize_publication_abi_version()", "UNSUPPORTED_TARGET",
        "CREATE_EXCLUSIVE", "NO_CHANGE", "NOT_PUBLISHED",
    ),
    ("memory-safety", "native-replay"): (
        "process-replay-v1", "NativeProcessReplayAdapter", "Available",
    ),
}


def readme_sections(
    text: str, path: Path, errors: list[str], label: str = "README"
) -> list[tuple[str, str]]:
    markers = list(README_SECTION_MARKER.finditer(text))
    sections = [('banner', text[:markers[0].start()] if markers else text)]
    seen: set[str] = set()
    for index, marker in enumerate(markers):
        key = marker.group(1)
        end = markers[index + 1].start() if index + 1 < len(markers) else len(text)
        body = text[marker.end():end]
        if key in seen or not re.match(r'\s*#{2,3} [^\n]+\n', body):
            report(errors, f'{path}: duplicate or detached {label} section {key}')
        seen.add(key)
        sections.append((key, body))
    headings = re.findall(r'^#{2,3} ', without_markdown_fences(text), re.MULTILINE)
    if len(headings) != len(markers):
        report(errors, f'{path}: {label} headings require stable section markers')
    return sections


def readme_visible_text(text: str) -> str:
    return re.sub(r'<!--.*?-->', '', without_markdown_fences(text), flags=re.DOTALL)


def readme_example_signature(text: str) -> list[tuple[str, str]]:
    result = []
    for match in re.finditer(r'^```([^\n]*)\n(.*?)^```\s*$', text, re.MULTILINE | re.DOTALL):
        language, body = match.group(1).strip(), match.group(2)
        if language in ('bash', 'sh'):
            body = '\n'.join(line.rstrip() for line in body.splitlines()
                             if line.strip() and not line.lstrip().startswith('#'))
            language = 'shell'
        elif language == 'text' and not body.lstrip().startswith('neverd '):
            # Diagram descriptions are translated; executable examples are not.
            body = '\n'.join(re.findall(r'[→├└│─]+', body))
        result.append((language, body.strip()))
    return result


def readme_table_signature(text: str) -> list[list[tuple[int, tuple[str, ...]]]]:
    tables: list[list[tuple[int, tuple[str, ...]]]] = []
    table: list[tuple[int, tuple[str, ...]]] = []
    for line in readme_visible_text(text).splitlines() + ['']:
        if line.startswith('|'):
            cells = re.split(r'(?<!\\)\|', line.strip().strip('|'))
            first = cells[0].strip()
            keys = re.findall(r'`([^`]+)`', first)
            if not keys:
                keys = re.findall(r'\*\*([^*]+)\*\*', first)
            table.append((len(cells), tuple(keys)))
        elif table:
            tables.append(table)
            table = []
    return tables


def readme_urls(text: str) -> list[str]:
    text = readme_visible_text(text)
    text = re.sub(r'`[^`\n]*`', '', text)
    urls = re.findall(r'\]\(([^\s)]+)(?:\s+["\'][^\n]*?["\'])?\)', text)
    # A reference link must be defined, and its destination participates in parity.
    definitions = {label.casefold(): url for label, url in re.findall(
        r'^\s*\[([^\]]+)\]:\s*(\S+)', text, re.MULTILINE)}
    for label, reference in re.findall(r'\[([^\]\n]+)\]\[([^\]\n]*)\]', text):
        urls.append(definitions.get((reference or label).casefold(), 'missing-reference:' + (reference or label)))
    return urls


def canonical_readme_url(url: str, path: PurePath, section_slugs: dict[str, str]) -> str:
    if re.match(r'[a-zA-Z][a-zA-Z0-9+.-]*:', url) or url.startswith('//'):
        return url
    target, sep, fragment = url.partition('#')
    resolved = Path(posixpath.normpath((path.parent / unquote(target)).as_posix())) if target else path
    parts = resolved.parts
    if len(parts) >= 3 and parts[0] == 'docs' and parts[1] in LOCALES:
        name = '/'.join(parts[2:])
        resolved = Path({'project.md': 'README.md', 'CONTRIBUTING.md': 'CONTRIBUTING.md',
                         'ATTRIBUTION.md': 'ATTRIBUTION.md'}.get(name, 'docs/' + name))
    if not target:
        fragment = section_slugs.get(unquote(fragment), fragment)
    return resolved.as_posix() + (sep + fragment if sep else '')


def validate_readme_parity(errors: list[str], view: RepositoryView) -> None:
    """Check source-relative structure and examples, not translation quality."""
    for source, filename in ((Path('README.md'), 'project.md'), (Path('docs/README.md'), 'README.md')):
        original = view.read_text(source)
        source_revision = hashlib.sha256(original.encode("utf-8")).hexdigest()
        selector_locales = ('zh-CN', 'zh-TW', 'ja', 'ko', 'fr', 'de', 'es', 'it', 'ru', 'ar')
        source_selector = ['README.md'] + [
            (f'docs/{locale}/project.md' if filename == 'project.md'
             else f'{locale}/README.md') for locale in selector_locales]
        if readme_urls(original.split('\n', 1)[0]) != source_selector:
            report(errors, f'{source}: README language selector differs')
        original_sections = readme_sections(original, source, errors) if filename == 'project.md' else [('index', original)]
        for locale in LOCALES:
            path = Path(f'docs/{locale}/{filename}')
            text = view.read_text(path)
            if README_SOURCE_REVISION.findall(text) != [source_revision]:
                report(errors, f'{path}: README source revision differs from {source}; '
                       'review the translation before updating its i18n-source SHA-256')
            translated_sections = readme_sections(text, path, errors) if filename == 'project.md' else [('index', text)]
            if [key for key, _ in translated_sections] != [key for key, _ in original_sections]:
                report(errors, f'{path}: README section order differs from {source}')
                continue
            slugs: dict[str, str] = {}
            for key, body in translated_sections:
                heading = re.search(r'^#{2,3} (.+)$', body, re.MULTILINE)
                if heading:
                    slugs[github_slug_base(heading.group(1))] = key
            for (key, expected), (_, actual) in zip(original_sections, translated_sections):
                if readme_example_signature(expected) != readme_example_signature(actual):
                    report(errors, f'{path}: README examples differ in {key}')
                if readme_table_signature(expected) != readme_table_signature(actual):
                    report(errors, f'{path}: README table rows differ in {key}')
                expected_visible, actual_visible = readme_visible_text(expected), readme_visible_text(actual)
                expected_codes = Counter(re.findall(r'`([^`\n]+)`', expected_visible))
                actual_codes = Counter(re.findall(r'`([^`\n]+)`', actual_visible))
                missing = expected_codes - actual_codes
                if missing:
                    report(errors, f'{path}: README API/options missing in {key}: {", ".join(sorted(missing))}')
                # Navigation selectors intentionally link all languages. Check them
                # separately, then compare only the translated body destinations.
                if key in ('banner', 'index'):
                    expected_visible = expected_visible.split('\n', 1)[1]
                    actual_visible = actual_visible.split('\n', 1)[1]
                expected_links = [canonical_readme_url(url, source, {}) for url in readme_urls(expected_visible)]
                actual_links = [canonical_readme_url(url, path, slugs) for url in readme_urls(actual_visible)]
                if expected_links != actual_links:
                    report(errors, f'{path}: README link order or destinations differ in {key}')
                for target in readme_urls(actual_visible):
                    if re.match(r'[a-zA-Z][a-zA-Z0-9+.-]*:', target) or target.startswith(('#', '//')):
                        continue
                    raw = target.partition('#')[0]
                    resolved = Path(posixpath.normpath((path.parent / unquote(raw)).as_posix()))
                    if resolved.parts and resolved.parts[0] == '..':
                        report(errors, f'{path}: README link escapes the repository: {target}')
                    elif not view.exists(resolved):
                        report(errors, f'{path}: missing README link target: {target}')
                    if len(resolved.parts) > 2 and resolved.parts[0] == 'docs' and resolved.parts[1] in LOCALES and resolved.parts[1] != locale:
                        report(errors, f'{path}: body link targets another locale: {target}')
                html_assets = lambda s: re.findall(r'\b(?:src|srcset)="([^"\n]+)"', s)
                if [canonical_readme_url(u, source, {}) for u in html_assets(expected_visible)] != [canonical_readme_url(u, path, {}) for u in html_assets(actual_visible)]:
                    report(errors, f'{path}: README HTML assets differ in {key}')
                for asset in html_assets(actual_visible):
                    if not re.match(r'https?://', asset):
                        asset_path = Path(posixpath.normpath((path.parent / asset).as_posix()))
                        if not view.exists(asset_path):
                            report(errors, f'{path}: missing README HTML asset: {asset}')
            expected_selector = [('../../README.md' if filename == 'project.md' else '../README.md')]
            selector_locales = ('zh-CN','zh-TW','ja','ko','fr','de','es','it','ru','ar')
            expected_selector += [filename if item == locale else f'../{item}/{filename}' for item in selector_locales]
            if readme_urls(text.split('\n', 1)[0]) != expected_selector:
                report(errors, f'{path}: README language selector differs')


def validate_synced_guide_examples(errors: list[str], view: RepositoryView) -> None:
    """Keep executable examples in their matching translated sections."""
    for stem in SYNCHRONIZED_GUIDES:
        source = Path(f"docs/{stem}.md")
        original = readme_sections(view.read_text(source), source, errors, "guide")
        for locale in LOCALES:
            path = Path(f"docs/{locale}/{stem}.md")
            translated = readme_sections(view.read_text(path), path, errors, "guide")
            if [key for key, _ in original] != [key for key, _ in translated]:
                report(errors, f"{path}: guide section order differs from {source}")
                continue
            for (key, expected), (_, actual) in zip(original, translated):
                if readme_example_signature(expected) != readme_example_signature(actual):
                    report(errors, f"{path}: guide examples differ in {key}")
                visible = readme_visible_text(actual)
                codes = set(re.findall(r"`([^`\n]+)`", visible))
                missing = set(GUIDE_SECTION_CONTRACTS.get((stem, key), ())) - codes
                if missing:
                    report(errors, f"{path}: guide contract missing in {key}: "
                           + ", ".join(sorted(missing)))


def validate_macos_guides(errors: list[str], view: RepositoryView) -> None:
    """Keep every Mac guide discoverable, reviewed and executable in each locale."""
    for stem in MACOS_GUIDE_STEMS:
        source = Path(f"docs/{stem}.md")
        original = view.read_text(source)
        revision = hashlib.sha256(original.encode("utf-8")).hexdigest()
        validate_language_selector(source, stem, None, errors, view)
        for locale in LOCALES:
            path = Path(f"docs/{locale}/{stem}.md")
            text = view.read_text(path)
            validate_language_selector(path, stem, locale, errors, view)
            if README_SOURCE_REVISION.findall(text) != [revision]:
                report(errors, f"{path}: Mac guide source revision differs from {source}; "
                       "review the translation before updating its i18n-source SHA-256")
            if readme_example_signature(original) != readme_example_signature(text):
                report(errors, f"{path}: Mac guide executable examples differ from {source}")
            index = Path(f"docs/{locale}/README.md")
            if f"{stem}.md" not in LINK_RE.findall(view.read_text(index)):
                report(errors, f"{index}: missing local Mac guide link: {stem}.md")


def validate_emulation_overview(errors: list[str], view: RepositoryView) -> None:
    """Keep relocated contracts reviewed and reachable in every language."""
    stem = "emulation"
    source = Path(f"docs/{stem}.md")
    original = view.read_text(source)
    revision = hashlib.sha256(original.encode("utf-8")).hexdigest()
    sections = readme_sections(original, source, errors, "emulation guide")
    validate_language_selector(source, stem, None, errors, view)
    for locale in LOCALES:
        path = Path(f"docs/{locale}/{stem}.md")
        text = view.read_text(path)
        validate_language_selector(path, stem, locale, errors, view)
        if README_SOURCE_REVISION.findall(text) != [revision]:
            report(errors, f"{path}: emulation guide source revision differs from {source}; "
                   "review the translation before updating its i18n-source SHA-256")
        translated = readme_sections(text, path, errors, "emulation guide")
        if [key for key, _ in translated] != [key for key, _ in sections]:
            report(errors, f"{path}: emulation guide section order differs from {source}")
            continue
        for (key, expected), (_, actual) in zip(sections, translated):
            if readme_example_signature(expected) != readme_example_signature(actual):
                report(errors, f"{path}: emulation guide examples differ in {key}")
            expected, actual = readme_visible_text(expected), readme_visible_text(actual)
            missing = (Counter(re.findall(r"`([^`\n]+)`", expected))
                       - Counter(re.findall(r"`([^`\n]+)`", actual)))
            if missing:
                report(errors, f"{path}: emulation guide API/options missing in {key}: "
                       + ", ".join(sorted(missing)))
            if key == "banner":
                expected, actual = expected.split("\n", 1)[1], actual.split("\n", 1)[1]
            expected_links = [canonical_readme_url(url, source, {}) for url in readme_urls(expected)]
            actual_links = [canonical_readme_url(url, path, {}) for url in readme_urls(actual)]
            if actual_links != expected_links:
                report(errors, f"{path}: emulation guide link destinations differ in {key}")
            for url in readme_urls(actual):
                target = url.partition("#")[0]
                if not target or re.match(r"[a-zA-Z][a-zA-Z0-9+.-]*:", target):
                    continue
                resolved = Path(posixpath.normpath((path.parent / unquote(target)).as_posix()))
                if (resolved.parts[:1] == ("docs",) and resolved.suffix == ".md"
                        and resolved.parent != path.parent
                        and view.exists(path.parent / resolved.name)):
                    report(errors, f"{path}: emulation guide body link must use the current locale: {url}")
        for filename in ("README.md", "project.md"):
            overview = path.parent / filename
            if f"{stem}.md" not in readme_urls(view.read_text(overview)):
                report(errors, f"{overview}: missing local emulation guide link")


def emulation_document_tokens(inventory: str) -> dict[str, list[str]]:
    tokens: dict[str, list[str]] = defaultdict(list)
    for group, literals in re.findall(
        r'NEVERD_EMULATION_DOC_TOKEN\(\s*(\w+),\s*((?:"[^"\\]+"\s*)+)\)',
        inventory,
    ):
        tokens[group].append("".join(re.findall(r'"([^"]*)"', literals)))
    return tokens


def validate_matrix(errors: list[str], view: RepositoryView) -> None:
    for path in MARKDOWN_DOCS:
        if not view.exists(path):
            report(
                errors,
                f"missing localized documentation file: {display_path(path)}",
            )
    if errors:
        return

    # Execution contracts and document paths stay in the .def inventory, so
    # every locale is checked against one set of semantic entry points.
    inventory = view.read_text(EMULATION_DOC_INVENTORY)
    doc_tokens = emulation_document_tokens(inventory)
    for group, pattern in re.findall(
        r'NEVERD_EMULATION_DOC_PATH\(\s*(\w+),\s*"([^\"]+)"\s*\)', inventory
    ):
        paths = [pattern.format(locale=locale) for locale in LOCALES]
        for path in dict.fromkeys(paths):
            require_tokens(Path(path), tuple(doc_tokens[group]), errors, view)

    if errors:
        return

    validate_readme_parity(errors, view)
    validate_emulation_overview(errors, view)
    validate_synced_guide_examples(errors, view)
    validate_macos_guides(errors, view)
    validate_driver_documents(errors, view)
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
    for stem in ("android", "ios"):
        validate_mobile_readme_entries(errors, view, stem)
    for stem in ("android", "ios", "mobile"):
        validate_mobile_examples(errors, view, stem)
    validate_mobile_overview_links(errors, view)
    validate_mobile_native_runtime(errors, view)
    registered_evm_tests = evm_test_targets(errors, view)

    require_tokens(Path("README.md"), ("docs/android.md", "docs/ios.md", "neverd mobile"), errors, view)
    require_tokens(
        Path("docs/README.md"),
        ("android.md", "ios.md", "cpu-execution.md", "process-emulation.md",
         "unpack.md", "solver.md"),
        errors,
        view,
    )

    selector_tokens = {
        stem: (f"{stem}.md", *(f"{locale}/{stem}.md" for locale in LOCALES))
        for stem in (*GUIDE_STEMS, "cpu-execution", "process-emulation", "solver", "unpack")
    }
    for stem in GUIDE_STEMS:
        guide = Path(f"docs/{stem}.md")
        require_tokens(
            guide, (*selector_tokens[stem], *GUIDE_REQUIRED_TOKENS[stem]), errors, view
        )
        validate_language_selector(guide, stem, None, errors, view)

    for stem, tokens in (
        (
            "cpu-execution",
            (
                "ExecutionConfiguration",
                "neverd_cpu_capabilities_json",
                "ExecutionExitKind::ServiceRequest",
                "MXCSR",
                "TPIDR_EL0",
            ),
        ),
        (
            "process-emulation",
            (
                "linux-elf64-v1",
                "neverd emulate guest.elf",
                "NeverDLinuxProcessTests",
                "ExecutionSession",
                "ET_DYN",
                "PT_TLS",
                "arch_prctl",
                "NeverDThreadPointerTests",
            ),
        ),
        (
            "solver",
            (
                "NEVERD_ENABLE_Z3",
                "NEVERD_Z3_PROVIDER",
                "NeverDSolverTests",
                "neverd-solver-bench",
                "--synthesize",
            ),
        ),
        ("unpack", UNPACK_REQUIRED_TOKENS),
    ):
        guide = Path(f"docs/{stem}.md")
        require_tokens(guide, (*selector_tokens[stem], *tokens), errors, view)
        validate_language_selector(guide, stem, None, errors, view)

    require_tokens(
        Path("docs/testing.md"),
        (*registered_evm_tests, *TESTING_REQUIRED_TOKENS),
        errors,
        view,
    )

    for locale in LOCALES:
        (
            project_readme,
            _contributing,
            _attribution,
            index,
            _architecture,
            memory_safety,
            _plugins,
            _python_plugins,
            roadmap,
            testing,
            _windows_exception,
            evm_guide,
            sbf_guide,
            android_guide,
            ios_guide,
            _driver_guide,
            _interpreter_guide,
            _emulation_overview,
            cpu_guide,
            process_guide,
            solver_guide,
            _mobile_overview,
            _hvf_guide,
            _darwin_guide,
            unpack_guide,
        ) = localized_paths(locale)
        require_tokens(
            _interpreter_guide,
            ("--devirtualize", "--vm-control", "--vm-control-stack=-16:8",
             "neverd_devirtualize_source_v1", "neverd_free_string", "testing.md"),
            errors,
            view,
        )
        require_tokens(
            index,
            ("interpreter-recovery.md", "cpu-execution.md", "process-emulation.md",
             "unpack.md", "solver.md"),
            errors,
            view,
        )
        for stem, guide, tokens in (
            (
                "cpu-execution",
                cpu_guide,
                (
                    "ExecutionConfiguration",
                    "neverd_cpu_capabilities_json",
                    "ExecutionExitKind::ServiceRequest",
                    "MXCSR",
                    "TPIDR_EL0",
                ),
            ),
            (
                "process-emulation",
                process_guide,
                (
                    "linux-elf64-v1",
                    "neverd emulate guest.elf",
                    "NeverDLinuxProcessTests",
                    "ExecutionSession",
                    "ET_DYN",
                    "PT_TLS",
                    "arch_prctl",
                    "NeverDThreadPointerTests",
                ),
            ),
            (
                "solver",
                solver_guide,
                (
                    "NEVERD_ENABLE_Z3",
                    "NEVERD_Z3_PROVIDER",
                    "NeverDSolverTests",
                    "neverd-solver-bench",
                    "--synthesize",
                ),
            ),
            ("unpack", unpack_guide, UNPACK_REQUIRED_TOKENS),
        ):
            localized_selector_tokens = (
                f"../{stem}.md",
                f"{stem}.md",
                *(f"../{other}/{stem}.md" for other in LOCALES if other != locale),
            )
            require_tokens(guide, (*localized_selector_tokens, *tokens), errors, view)
            validate_language_selector(guide, stem, locale, errors, view)
        require_tokens(project_readme, ("--devirtualize", "interpreter-recovery.md"), errors, view)
        require_tokens(
            project_readme,
            (
                "EVM256",
                "Solana SBF",
                "v0-v4",
                "--language=solidity",
                "--language=rust",
                "evm.md",
                "sbf.md",
                "android.md",
                "ios.md",
                "neverd mobile",
            ),
            errors,
            view,
        )
        require_tokens(
            index,
            ("evm.md", "sbf.md", "android.md", "ios.md"),
            errors,
            view,
        )
        require_tokens(
            roadmap,
            (
                "v0-v4",
                "Solidity",
                "Rust",
                "evm.md",
                "sbf.md",
            ),
            errors,
            view,
        )
        require_tokens(
            testing,
            (
                *registered_evm_tests,
                "NeverDSBFMetadataTests",
                "NeverDSBFSemanticTests",
                "NeverDSBFIntegrationTests",
                "NeverDThreadPointerTests",
                "NeverDKvmCancellationTests",
                "DriverBackendParityTests.cpp",
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
        for stem, guide in zip(
            GUIDE_STEMS, (evm_guide, sbf_guide, android_guide, ios_guide)
        ):
            targets = tuple(
                posixpath.relpath(f"docs/{target}", guide.parent.as_posix())
                for target in selector_tokens[stem]
            )
            require_tokens(
                guide, (*targets, *GUIDE_REQUIRED_TOKENS[stem]), errors, view
            )


def validate_staged(errors: list[str]) -> None:
    result = subprocess.run(
        ("git", "diff", "--cached", "--name-only"),
        cwd=REPO_ROOT,
        check=True,
        capture_output=True,
        text=True,
        encoding="utf-8",
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
