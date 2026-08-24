#!/usr/bin/env python3
"""Audit NeverD's EVM opcode database against current go-ethereum sources."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import secrets
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator, Mapping, Sequence


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_NEVERD_OPCODES = REPO_ROOT / "include/neverd/evm/bytecode/EVMOpcodes.def"
DEFAULT_NEVERD_HARDFORKS = REPO_ROOT / "include/neverd/evm/bytecode/EVMHardforks.def"
DEFAULT_NEVERD_CONSTANTS = REPO_ROOT / "include/neverd/evm/EVMConstants.h"
DEFAULT_POLICY = REPO_ROOT / "include/neverd/evm/bytecode/EVMUpstreamOpcodePolicy.def"
DEFAULT_SEMANTICS_POLICY = (
    REPO_ROOT / "include/neverd/evm/bytecode/EVMUpstreamSemanticsPolicy.def"
)
DEFAULT_GETH_FORK_ALIASES = (
    REPO_ROOT / "include/neverd/evm/bytecode/EVMUpstreamForkAliases.def"
)
DEFAULT_EIP8024_POLICY = (
    REPO_ROOT / "include/neverd/evm/bytecode/EVMEIP8024Immediates.def"
)
DEFAULT_GETH_REMOTE = "https://github.com/ethereum/go-ethereum.git"
DEFAULT_GETH_REF = "HEAD"
DEFAULT_GETH_PROBE = REPO_ROOT / "scripts/evm_geth_opcode_probe.go"
DEFAULT_GETH_EIP8024_OVERLAY = REPO_ROOT / "scripts/evm_geth_eip8024_overlay.go"
DEFAULT_GIT_EXECUTABLE = "git"
DEFAULT_GO_EXECUTABLE = "go"
DEFAULT_GO_TOOLCHAIN = "local"
GO_TOOLCHAIN_CHOICES = (DEFAULT_GO_TOOLCHAIN,)
GO_MODULE_PROXY = "https://proxy.golang.org"
GO_CHECKSUM_DATABASE = "sum.golang.org"
DARWIN_SANDBOX_EXECUTABLE = "sandbox-exec"
DARWIN_SANDBOX_DYLD_PROFILE = "dyld-support.sb"
LINUX_SANDBOX_EXECUTABLE = "bwrap"
DARWIN_SANDBOX_SYSTEM_READ_PATHS = (
    Path("/System/Library"),
    Path("/usr/lib"),
    Path("/Library/Apple/usr/lib"),
    Path("/private/var/db/timezone"),
    Path("/private/etc/resolv.conf"),
    Path("/private/etc/hosts"),
    Path("/private/etc/ssl/cert.pem"),
    Path("/dev/null"),
    Path("/dev/random"),
    Path("/dev/urandom"),
)
DARWIN_SANDBOX_SYSTEM_WRITE_PATHS = (Path("/dev/null"),)
DARWIN_SANDBOX_SYSTEM_METADATA_PATHS = (
    Path("/etc"),
    Path("/var"),
    Path("/tmp"),
    Path("/private/etc/localtime"),
    Path("/private/etc/resolv.conf"),
    Path("/usr/share/zoneinfo"),
)
LINUX_SANDBOX_SYSTEM_READ_PATHS = (
    Path("/lib"),
    Path("/lib64"),
    Path("/usr/lib"),
    Path("/usr/lib64"),
    Path("/etc/ld.so.cache"),
    Path("/etc/localtime"),
    Path("/etc/hosts"),
    Path("/etc/nsswitch.conf"),
    Path("/etc/passwd"),
    Path("/etc/group"),
    Path("/etc/resolv.conf"),
    Path("/etc/ssl/certs"),
    Path("/etc/ssl/cert.pem"),
    Path("/usr/share/zoneinfo"),
)
GO_TOOLCHAIN_METADATA_FIELDS = frozenset({"GOROOT", "GOVERSION"})
GETH_OPCODE_PATH = Path("core/vm/opcodes.go")
GETH_CACHE_REF_PREFIX = "refs/neverd/go-ethereum-audit"
GETH_CACHE_REF_TOKEN_BYTES = 16
GETH_CACHE_REQUIRED_CONFIG = {
    "core.bare": "true",
    "core.repositoryformatversion": "0",
}
GETH_CACHE_BOOLEAN_CONFIG = frozenset(
    {
        "core.filemode",
        "core.ignorecase",
        "core.logallrefupdates",
        "core.precomposeunicode",
        "core.symlinks",
    }
)
GETH_CACHE_FORBIDDEN_PATHS = (
    Path("info/attributes"),
    Path("info/grafts"),
    Path("objects/info/alternates"),
    Path("objects/info/http-alternates"),
    Path("refs/replace"),
)
GIT_FETCH_TIMEOUT_SECONDS = 300
GO_PROBE_TOTAL_TIMEOUT_SECONDS = 900
GO_PROBE_TIMEOUT_DIAGNOSTIC = (
    "go opcode probe exceeded the shared "
    f"{GO_PROBE_TOTAL_TIMEOUT_SECONDS}-second timeout"
)
GETH_PROBE_SCHEMA_VERSION = 3
GETH_AUDIT_AUTHORITY = "official-fresh-fetch"
GETH_MANIFEST_ROOT_FIELDS = frozenset(
    {
        "schema_version",
        "authority",
        "geth_remote",
        "geth_ref",
        "geth_revision",
        "audit_unix_time",
        "go_version",
        "stack_limit",
        "forks",
        "rule_probes",
        "mainnet",
        "eip8024",
    }
)
GETH_MANIFEST_FORK_FIELDS = frozenset({"name", "rules", "opcodes"})
GETH_MANIFEST_OPCODE_FIELDS = frozenset(
    {"byte", "name", "base_min_stack", "net_stack_delta"}
)
GETH_MANIFEST_RULE_PROBE_FIELDS = frozenset(
    {"name", "category", "expected_fork", "lookup_error", "opcodes"}
)
GETH_MANIFEST_MAINNET_FIELDS = frozenset({"active", "scheduled"})
GETH_MANIFEST_MAINNET_FORK_FIELDS = frozenset({"upstream_fork", "rules", "opcodes"})
GETH_MANIFEST_EIP8024_FIELDS = frozenset({"tables"})
GETH_MANIFEST_EIP8024_TABLE_FIELDS = frozenset(
    {"target", "active_opcodes", "handlers", "observations", "missing_operand"}
)
GETH_MANIFEST_EIP8024_HANDLER_FIELDS = frozenset({"opcode", "symbol"})
GETH_MANIFEST_EIP8024_OBSERVATION_FIELDS = frozenset(
    {
        "opcode",
        "encoded",
        "accepted",
        "operands",
        "pc_delta",
        "error_class",
        "stack_delta",
        "marker_transition_verified",
        "underflow_error_class",
        "underflow_pc_delta",
        "underflow_stack_unchanged",
    }
)
GETH_MANIFEST_EIP8024_MISSING_FIELDS = frozenset(
    {"opcode", "matches_zero_immediate", "marker_transition_verified"}
)
AUDIT_MANIFEST_FIELDS = GETH_MANIFEST_ROOT_FIELDS | frozenset({"diagnostics"})
OPCODE_BITS = 8
OPCODE_MAX = (1 << OPCODE_BITS) - 1
OPCODE_HEX_DIGITS = OPCODE_BITS // 4
PROCESS_STDOUT_LIMIT_BYTES = 8 * 1024 * 1024
PROCESS_STDERR_LIMIT_BYTES = 1024 * 1024
PROCESS_DIAGNOSTIC_TAIL_BYTES = 4096
PROCESS_IO_CHUNK_BYTES = 64 * 1024
PROCESS_PIPE_DRAIN_TIMEOUT_SECONDS = 0.5
PROBE_REQUEST_LIMIT_BYTES = 1 << 20
FIXED_INPUT_FILE_LIMIT_BYTES = 4 * 1024 * 1024
FIXED_INPUT_AGGREGATE_LIMIT_BYTES = 16 * 1024 * 1024
PARSER_STRING_LIMIT_BYTES = 256
MAX_OPCODE_RECORDS = OPCODE_MAX + 1
MAX_HARDFORK_RECORDS = 128
MAX_RULE_FIELDS = 128
MAX_RULE_PROBES = 128
MAX_RULES_PER_FORK = 128
MAX_POLICY_RECORDS = 512
MAX_SEMANTICS_POLICY_RECORDS = 1024
MAX_FORK_ALIAS_RECORDS = 128
MAX_EIP8024_POLICY_RECORDS = 4 * (OPCODE_MAX + 1)
MAX_EIP8024_SPECS = OPCODE_MAX + 1
PROBE_REQUEST_FIELDS = frozenset(
    {"rule_fields", "rule_probes", "forks", "opcodes", "eip8024_specs"}
)
PROBE_TRUSTED_FIELDS = frozenset(
    {
        "schema_version",
        "authority",
        "geth_remote",
        "geth_ref",
        "geth_revision",
        "audit_unix_time",
    }
)
MAINNET_ACTIVE_EIP8024_TARGET = "mainnet.active"
MAINNET_SCHEDULED_EIP8024_TARGET = "mainnet.scheduled"
SUBPROCESS_ENVIRONMENT_ALLOWLIST = frozenset(
    {"PATH", "SYSTEMROOT", "WINDIR", "COMSPEC", "PATHEXT"}
)

NEVERD_OPCODE_METADATA_RE = re.compile(
    r"^[ \t]*EVM_OPCODE\([ \t\r\n]*([A-Z][A-Z0-9_]*)[ \t\r\n]*,"
    r"[ \t\r\n]*(0x[0-9a-fA-F]+)[ \t\r\n]*,"
    r"[ \t\r\n]*([0-9]+)[ \t\r\n]*,[ \t\r\n]*([0-9]+)"
    r"[ \t\r\n]*,[ \t\r\n]*[0-9]+[ \t\r\n]*,"
    r"[ \t\r\n]*[A-Za-z][A-Za-z0-9_]*[ \t\r\n]*,"
    r"[ \t\r\n]*[A-Za-z][A-Za-z0-9_]*[ \t\r\n]*,"
    r"[ \t\r\n]*([A-Za-z][A-Za-z0-9_]*)[ \t\r\n]*,"
    r"[ \t\r\n]*[A-Za-z][A-Za-z0-9_]*[ \t\r\n]*,"
    r"[ \t\r\n]*[A-Za-z][A-Za-z0-9_]*[ \t\r\n]*,"
    r"[ \t\r\n]*[A-Za-z][A-Za-z0-9_]*[ \t\r\n]*,"
    r"[ \t\r\n]*[A-Za-z][A-Za-z0-9_]*[ \t\r\n]*,"
    r"[ \t\r\n]*(?:true|false)[ \t\r\n]*\)[ \t]*(?://[^\n]*)?$",
    re.MULTILINE,
)
NEVERD_OPCODE_DECLARATION_RE = re.compile(r"^[ \t]*EVM_OPCODE\s*\(", re.MULTILINE)
NEVERD_OPCODE_NAME_ALIAS_RE = re.compile(
    r"^[ \t]*EVM_OPCODE_NAME_ALIAS\([ \t]*([A-Z][A-Z0-9_]*)[ \t]*,"
    r"[ \t]*([A-Z][A-Z0-9_]*)[ \t]*,"
    r"[ \t]*([A-Za-z][A-Za-z0-9_]*)[ \t]*,"
    r"[ \t]*([A-Za-z][A-Za-z0-9_]*)[ \t]*\)"
    r"[ \t]*(?://[^\n]*)?$",
    re.MULTILINE,
)
NEVERD_OPCODE_NAME_ALIAS_DECLARATION_RE = re.compile(
    r"^[ \t]*EVM_OPCODE_NAME_ALIAS\s*\(", re.MULTILINE
)
POLICY_ALIAS_RE = re.compile(
    r"^\s*EVM_UPSTREAM_OPCODE_ALIAS\(\s*([A-Z][A-Z0-9_]*)\s*,\s*"
    r"([A-Z][A-Z0-9_]*)\s*\)[ \t]*(?://[^\n]*)?$",
    re.MULTILINE,
)
POLICY_ALIAS_DECLARATION_RE = re.compile(
    r"^\s*EVM_UPSTREAM_OPCODE_ALIAS\s*\(", re.MULTILINE
)
POLICY_IGNORE_RE = re.compile(
    r"^\s*EVM_UPSTREAM_OPCODE_IGNORE\(\s*([A-Z][A-Z0-9_]*)\s*,\s*"
    r"(0x[0-9a-fA-F]+)\s*,\s*([A-Za-z][A-Za-z0-9_]*)\s*\)"
    r"[ \t]*(?://[^\n]*)?$",
    re.MULTILINE,
)
POLICY_IGNORE_DECLARATION_RE = re.compile(
    r"^\s*EVM_UPSTREAM_OPCODE_IGNORE\s*\(", re.MULTILINE
)
POLICY_EXCLUSION_KIND_RE = re.compile(
    r"^\s*EVM_UPSTREAM_OPCODE_EXCLUSION_KIND\(\s*"
    r"([A-Za-z][A-Za-z0-9_]*)\s*,\s*(true|false)\s*,\s*"
    r'(true|false)\s*,\s*"(?:[^"\\]|\\.)*"\s*\)'
    r"[ \t]*(?://[^\n]*)?$",
    re.MULTILINE,
)
POLICY_EXCLUSION_KIND_DECLARATION_RE = re.compile(
    r"^\s*EVM_UPSTREAM_OPCODE_EXCLUSION_KIND\s*\(", re.MULTILINE
)
SEMANTICS_FORK_RE = re.compile(
    r"^\s*EVM_GETH_FORK_RULE\(\s*([A-Za-z][A-Za-z0-9_]*)\s*,\s*"
    r"([A-Za-z][A-Za-z0-9_]*)\s*\)[ \t]*(?://[^\n]*)?$",
    re.MULTILINE,
)
SEMANTICS_FORK_DECLARATION_RE = re.compile(r"^\s*EVM_GETH_FORK_RULE\s*\(", re.MULTILINE)
SEMANTICS_RULE_FIELD_RE = re.compile(
    r"^\s*EVM_GETH_RULE_FIELD\(\s*([A-Za-z][A-Za-z0-9_]*)\s*,\s*"
    r"([A-Za-z][A-Za-z0-9_]*)\s*,\s*"
    r"([A-Za-z][A-Za-z0-9_]*)\s*\)[ \t]*(?://[^\n]*)?$",
    re.MULTILINE,
)
SEMANTICS_RULE_FIELD_DECLARATION_RE = re.compile(
    r"^\s*EVM_GETH_RULE_FIELD\s*\(", re.MULTILINE
)
SEMANTICS_BASE_MIN_STACK_RE = re.compile(
    r"^\s*EVM_GETH_BASE_MIN_STACK\(\s*([A-Z][A-Z0-9_]*)\s*,\s*"
    r"([0-9]+)\s*\)[ \t]*(?://[^\n]*)?$",
    re.MULTILINE,
)
SEMANTICS_BASE_MIN_STACK_DECLARATION_RE = re.compile(
    r"^\s*EVM_GETH_BASE_MIN_STACK\s*\(", re.MULTILINE
)
SEMANTICS_ACTIVE_WITHOUT_COST_RE = re.compile(
    r"^\s*EVM_GETH_ACTIVE_WITHOUT_COST\(\s*([A-Z][A-Z0-9_]*)\s*,\s*"
    r"([A-Za-z][A-Za-z0-9_]*)\s*\)[ \t]*(?://[^\n]*)?$",
    re.MULTILINE,
)
SEMANTICS_ACTIVE_WITHOUT_COST_DECLARATION_RE = re.compile(
    r"^\s*EVM_GETH_ACTIVE_WITHOUT_COST\s*\(", re.MULTILINE
)
SEMANTICS_DYNAMIC_STACK_RE = re.compile(
    r"^\s*EVM_GETH_DYNAMIC_STACK_IMMEDIATE\(\s*"
    r"([A-Z][A-Z0-9_]*)\s*,\s*([A-Za-z][A-Za-z0-9_]*)\s*,\s*"
    r"([A-Za-z][A-Za-z0-9_]*)\s*,\s*(-?[0-9]+)\s*\)"
    r"[ \t]*(?://[^\n]*)?$",
    re.MULTILINE,
)
SEMANTICS_DYNAMIC_STACK_DECLARATION_RE = re.compile(
    r"^\s*EVM_GETH_DYNAMIC_STACK_IMMEDIATE\s*\(", re.MULTILINE
)
NEVERD_HARDFORK_RE = re.compile(
    r"^[ \t]*EVM_HARDFORK\([ \t]*([A-Za-z][A-Za-z0-9_]*)[ \t]*,"
    r'[ \t]*"(?:[^"\\]|\\.)+"[ \t]*\)[ \t]*(?://[^\n]*)?$',
    re.MULTILINE,
)
NEVERD_HARDFORK_DECLARATION_RE = re.compile(r"^[ \t]*EVM_HARDFORK\s*\(", re.MULTILINE)
NEVERD_LATEST_HARDFORK_RE = re.compile(
    r"^[ \t]*EVM_HARDFORK_LATEST\([ \t]*([A-Za-z][A-Za-z0-9_]*)[ \t]*,"
    r'[ \t]*"(?:[^"\\]|\\.)+"[ \t]*\)[ \t]*(?://[^\n]*)?$',
    re.MULTILINE,
)
NEVERD_LATEST_HARDFORK_DECLARATION_RE = re.compile(
    r"^\s*EVM_HARDFORK_LATEST\s*\(", re.MULTILINE
)
NEVERD_HARDFORK_ALIAS_RE = re.compile(
    r'^[ \t]*EVM_HARDFORK_ALIAS\([ \t]*"((?:[^"\\]|\\.)+)"[ \t]*,'
    r"[ \t]*([A-Za-z][A-Za-z0-9_]*)[ \t]*\)[ \t]*(?://[^\n]*)?$",
    re.MULTILINE,
)
NEVERD_HARDFORK_ALIAS_DECLARATION_RE = re.compile(
    r"^[ \t]*EVM_HARDFORK_ALIAS\s*\(", re.MULTILINE
)
NEVERD_NEWEST_HARDFORK_RE = re.compile(
    r"^[ \t]*EVM_HARDFORK_NEWEST\([ \t]*([A-Za-z][A-Za-z0-9_]*)"
    r"[ \t]*\)[ \t]*(?://[^\n]*)?$",
    re.MULTILINE,
)
NEVERD_NEWEST_HARDFORK_DECLARATION_RE = re.compile(
    r"^[ \t]*EVM_HARDFORK_NEWEST\s*\(", re.MULTILINE
)
GETH_FORK_ALIAS_RE = re.compile(
    r"^[ \t]*EVM_GETH_FORK_ALIAS\([ \t]*([A-Za-z][A-Za-z0-9_]*)[ \t]*,"
    r"[ \t]*([A-Za-z][A-Za-z0-9_]*)[ \t]*\)[ \t]*(?://[^\n]*)?$",
    re.MULTILINE,
)
GETH_FORK_ALIAS_DECLARATION_RE = re.compile(
    r"^\s*EVM_GETH_FORK_ALIAS\s*\(", re.MULTILINE
)
EIP8024_SINGLE_VALID_RE = re.compile(
    r"^[ \t]*EVM_EIP8024_SINGLE_VALID\([ \t]*(0x[0-9a-fA-F]+)[ \t]*,"
    r"[ \t]*([0-9]+)[ \t]*\)[ \t]*(?://[^\n]*)?$",
    re.MULTILINE,
)
EIP8024_SINGLE_INVALID_RE = re.compile(
    r"^[ \t]*EVM_EIP8024_SINGLE_INVALID\([ \t]*(0x[0-9a-fA-F]+)"
    r"[ \t]*\)[ \t]*(?://[^\n]*)?$",
    re.MULTILINE,
)
EIP8024_PAIR_VALID_RE = re.compile(
    r"^[ \t]*EVM_EIP8024_PAIR_VALID\([ \t]*(0x[0-9a-fA-F]+)[ \t]*,"
    r"[ \t]*([0-9]+)[ \t]*,[ \t]*([0-9]+)[ \t]*\)"
    r"[ \t]*(?://[^\n]*)?$",
    re.MULTILINE,
)
EIP8024_PAIR_INVALID_RE = re.compile(
    r"^[ \t]*EVM_EIP8024_PAIR_INVALID\([ \t]*(0x[0-9a-fA-F]+)"
    r"[ \t]*\)[ \t]*(?://[^\n]*)?$",
    re.MULTILINE,
)
EIP8024_SINGLE_DECLARATION_RE = re.compile(
    r"^\s*EVM_EIP8024_SINGLE_(?:VALID|INVALID)\s*\(", re.MULTILINE
)
EIP8024_PAIR_DECLARATION_RE = re.compile(
    r"^\s*EVM_EIP8024_PAIR_(?:VALID|INVALID)\s*\(", re.MULTILINE
)
NEVERD_STACK_LIMIT_RE = re.compile(
    r"^\s*inline\s+constexpr\s+std::size_t\s+kStackLimit\s*=\s*"
    r"([0-9][0-9']*)\s*;\s*$",
    re.MULTILINE,
)
CONST_BLOCK_RE = re.compile(r"\bconst\s*\((.*?)\n\s*\)", re.DOTALL)
CONST_SPEC_RE = re.compile(r"^([A-Z][A-Z0-9_]*)\s*(?:(OpCode)\s*)?(?:=\s*(.*?))?\s*$")
OPCODE_EXPRESSION_RE = re.compile(r"^(0x[0-9a-fA-F]+|[0-9]+)(?:\s*\+\s*iota)?$")
OPCODE_DECLARATION_RE = re.compile(
    r"^\s*(?:const\s+)?([A-Z][A-Z0-9_]*)\s+OpCode\b", re.MULTILINE
)
GIT_OBJECT_ID_RE = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
GIT_REF_RE = re.compile(r"^(?:HEAD|refs/(?:heads|tags)/[A-Za-z0-9][A-Za-z0-9._/-]*)$")
GETH_CACHE_AUTHORITY_REF_RE = re.compile(
    rf"^{re.escape(GETH_CACHE_REF_PREFIX)}/"
    rf"[0-9a-f]{{{2 * GETH_CACHE_REF_TOKEN_BYTES}}}$"
)
GO_VERSION_RE = re.compile(r"^go\s+([0-9]+(?:\.[0-9]+){1,2})\s*$", re.MULTILINE)
NO_GETH_RULE = "None"
MAPPED_FORK_SELECTOR = "MappedForkSelector"
NO_OPCODE_ALLOCATION = "NoOpcodeAllocation"
EXCLUDED_SELECTOR_EXPECTED_ERROR = "ExcludedSelectorExpectedError"
GETH_RULE_CATEGORIES = frozenset(
    {
        MAPPED_FORK_SELECTOR,
        NO_OPCODE_ALLOCATION,
        EXCLUDED_SELECTOR_EXPECTED_ERROR,
    }
)
EIP8024_OPERAND_FAMILIES = {"Single": "single", "Pair": "pair"}
EIP8024_OPERATION_KINDS = {"Dup": "dup", "Swap": "swap", "Exchange": "exchange"}
EIP8024_ERROR_NONE = "none"
EIP8024_ERROR_INVALID_OPCODE = "invalid_opcode"
EIP8024_ERROR_STACK_UNDERFLOW = "stack_underflow"
EIP8024_ERROR_NOT_RUN = "not_run"
EIP8024_ACCEPTED_PC_DELTA = 1
EIP8024_REJECTED_PC_DELTA = 0


class OpcodeAuditError(ValueError):
    """Raised when a metadata source is malformed or has drifted."""


@dataclass(frozen=True)
class BoundedProcessResult:
    returncode: int
    stdout: str
    stderr: str
    stdout_sha256: str
    stderr_sha256: str


@dataclass
class _BoundedStreamState:
    limit: int
    size: int = 0
    sha256: str = ""
    exceeded: bool = False


@dataclass
class _FixedInputBudget:
    limit: int = FIXED_INPUT_AGGREGATE_LIMIT_BYTES
    used: int = 0

    def charge(self, size: int, context: str) -> None:
        if size < 0 or size > self.limit - self.used:
            raise OpcodeAuditError(
                f"{context}: aggregate fixed-input bytes exceed {self.limit}"
            )
        self.used += size


def _read_bounded_utf8(
    path: Path,
    context: str,
    *,
    budget: _FixedInputBudget | None = None,
    per_file_limit: int = FIXED_INPUT_FILE_LIMIT_BYTES,
) -> str:
    if per_file_limit < 0:
        raise ValueError("fixed-input file limit must not be negative")
    with path.open("rb") as source:
        encoded = source.read(per_file_limit + 1)
    if len(encoded) > per_file_limit:
        raise OpcodeAuditError(
            f"{context}: input exceeds the {per_file_limit}-byte file limit"
        )
    if budget is not None:
        budget.charge(len(encoded), context)
    try:
        return encoded.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise OpcodeAuditError(f"{context}: input is not valid UTF-8") from error


def _require_string_budget(value: str, context: str) -> None:
    if len(value) > PARSER_STRING_LIMIT_BYTES:
        raise OpcodeAuditError(
            f"{context}: string exceeds {PARSER_STRING_LIMIT_BYTES} bytes"
        )
    if len(value.encode("utf-8")) > PARSER_STRING_LIMIT_BYTES:
        raise OpcodeAuditError(
            f"{context}: string exceeds {PARSER_STRING_LIMIT_BYTES} UTF-8 bytes"
        )


def _require_cardinality(size: int, limit: int, context: str) -> None:
    if size > limit:
        raise OpcodeAuditError(f"{context} count exceeds {limit}")


def _require_probe_request_bytes(
    encoded_request: str, limit: int = PROBE_REQUEST_LIMIT_BYTES
) -> None:
    if len(encoded_request) > limit or len(encoded_request.encode("utf-8")) > limit:
        raise OpcodeAuditError(
            f"geth opcode probe request exceeds the {limit}-byte limit"
        )


def _kill_process_group(process: subprocess.Popen[bytes]) -> None:
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        return


def _join_process_output_threads(
    process: subprocess.Popen[bytes], threads: Sequence[threading.Thread]
) -> None:
    deadline = time.monotonic() + PROCESS_PIPE_DRAIN_TIMEOUT_SECONDS
    for thread in threads:
        thread.join(max(0.0, deadline - time.monotonic()))
    if not any(thread.is_alive() for thread in threads):
        return

    _kill_process_group(process)
    deadline = time.monotonic() + PROCESS_PIPE_DRAIN_TIMEOUT_SECONDS
    for thread in threads:
        thread.join(max(0.0, deadline - time.monotonic()))
    if not any(thread.is_alive() for thread in threads):
        return

    for stream in (process.stdout, process.stderr):
        if stream is not None:
            stream.close()
    deadline = time.monotonic() + PROCESS_PIPE_DRAIN_TIMEOUT_SECONDS
    for thread in threads:
        thread.join(max(0.0, deadline - time.monotonic()))
    if any(thread.is_alive() for thread in threads):
        raise OpcodeAuditError(
            "process output readers did not drain after process-group shutdown"
        )


def _stream_process_output(
    source: object,
    destination: object,
    state: _BoundedStreamState,
    process: subprocess.Popen[bytes],
) -> None:
    digest = hashlib.sha256()
    try:
        while True:
            chunk = source.read(PROCESS_IO_CHUNK_BYTES)  # type: ignore[attr-defined]
            if not chunk:
                break
            digest.update(chunk)
            remaining = state.limit - state.size
            if remaining > 0:
                captured = chunk[:remaining]
                destination.write(captured)  # type: ignore[attr-defined]
                state.size += len(captured)
            if len(chunk) > remaining:
                state.exceeded = True
                try:
                    _kill_process_group(process)
                except OSError:
                    pass
                break
    finally:
        state.sha256 = digest.hexdigest()
        source.close()  # type: ignore[attr-defined]


def _process_stream_diagnostic(text: str, digest: str) -> str:
    tail = text[-PROCESS_DIAGNOSTIC_TAIL_BYTES:]
    return f"sha256={digest}, bounded tail={tail!r}"


def _run_bounded_process(
    command: Sequence[str],
    *,
    cwd: Path | None,
    environment: Mapping[str, str],
    timeout_seconds: float,
    input_text: str | None = None,
    stdout_limit: int = PROCESS_STDOUT_LIMIT_BYTES,
    stderr_limit: int = PROCESS_STDERR_LIMIT_BYTES,
) -> BoundedProcessResult:
    if timeout_seconds <= 0:
        raise ValueError("process timeout must be greater than zero")
    if stdout_limit < 0 or stderr_limit < 0:
        raise ValueError("process output limits must not be negative")
    if os.name != "posix" or not (
        sys.platform == "darwin" or sys.platform.startswith("linux")
    ):
        raise OpcodeAuditError(
            f"unsupported platform for bounded process execution: {sys.platform}"
        )
    with tempfile.TemporaryDirectory(prefix="neverd-bounded-process-") as temporary:
        root = Path(temporary)
        root.chmod(0o700)
        stdout_path = root / "stdout"
        stderr_path = root / "stderr"
        input_path = root / "stdin"
        if input_text is not None:
            input_path.write_text(input_text, encoding="utf-8")
            input_path.chmod(0o600)
        with (
            (
                input_path.open("rb")
                if input_text is not None
                else open(os.devnull, "rb")
            ) as process_input,
            stdout_path.open("w+b") as stdout_file,
            stderr_path.open("w+b") as stderr_file,
        ):
            process = subprocess.Popen(
                tuple(command),
                cwd=cwd,
                env=dict(environment),
                stdin=process_input,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                start_new_session=True,
            )
            assert process.stdout is not None
            assert process.stderr is not None
            stdout_state = _BoundedStreamState(stdout_limit)
            stderr_state = _BoundedStreamState(stderr_limit)
            stdout_thread = threading.Thread(
                target=_stream_process_output,
                args=(process.stdout, stdout_file, stdout_state, process),
                daemon=True,
            )
            stderr_thread = threading.Thread(
                target=_stream_process_output,
                args=(process.stderr, stderr_file, stderr_state, process),
                daemon=True,
            )
            stdout_thread.start()
            stderr_thread.start()
            try:
                returncode = process.wait(timeout=timeout_seconds)
            except subprocess.TimeoutExpired:
                _kill_process_group(process)
                process.wait()
                _join_process_output_threads(process, (stdout_thread, stderr_thread))
                raise
            try:
                _join_process_output_threads(process, (stdout_thread, stderr_thread))
            except OpcodeAuditError:
                _kill_process_group(process)
                raise
            stdout_file.flush()
            stderr_file.flush()
            stdout_file.seek(0)
            stderr_file.seek(0)
            stdout_bytes = stdout_file.read()
            stderr_bytes = stderr_file.read()
        stdout = stdout_bytes.decode("utf-8", errors="replace")
        stderr = stderr_bytes.decode("utf-8", errors="replace")
        if stdout_state.exceeded:
            raise OpcodeAuditError(
                f"process stdout exceeded {stdout_limit}-byte limit; "
                + _process_stream_diagnostic(stdout, stdout_state.sha256)
            )
        if stderr_state.exceeded:
            raise OpcodeAuditError(
                f"process stderr exceeded {stderr_limit}-byte limit; "
                + _process_stream_diagnostic(stderr, stderr_state.sha256)
            )
        return BoundedProcessResult(
            returncode=returncode,
            stdout=stdout,
            stderr=stderr,
            stdout_sha256=stdout_state.sha256,
            stderr_sha256=stderr_state.sha256,
        )


def _require_exact_fields(
    value: Mapping[str, object], expected: frozenset[str], context: str
) -> None:
    missing = sorted(expected - set(value))
    unexpected = sorted(set(value) - expected)
    if not missing and not unexpected:
        return
    details: list[str] = []
    if missing:
        details.append("missing field " + ", ".join(missing))
    if unexpected:
        details.append("unexpected field " + ", ".join(unexpected))
    raise OpcodeAuditError(f"{context}: " + "; ".join(details))


def _require_required_known_fields(
    value: Mapping[str, object],
    required: frozenset[str],
    allowed: frozenset[str],
    context: str,
) -> None:
    missing = sorted(required - set(value))
    unexpected = sorted(set(value) - allowed)
    if not missing and not unexpected:
        return
    details: list[str] = []
    if missing:
        details.append("missing field " + ", ".join(missing))
    if unexpected:
        details.append("unexpected field " + ", ".join(unexpected))
    raise OpcodeAuditError(f"{context}: " + "; ".join(details))


def _require_integer_field(
    value: Mapping[str, object], field: str, context: str
) -> int:
    result = value.get(field)
    if type(result) is not int:
        raise OpcodeAuditError(f"{context}: {field} must be an integer")
    return result


def _require_string_field(value: Mapping[str, object], field: str, context: str) -> str:
    result = value.get(field)
    if not isinstance(result, str):
        raise OpcodeAuditError(f"{context}: {field} must be a string")
    _require_string_budget(result, f"{context}: {field}")
    return result


def _require_string_array_field(
    value: Mapping[str, object],
    field: str,
    context: str,
    *,
    limit: int = MAX_RULES_PER_FORK,
) -> list[str]:
    result = value.get(field)
    if not isinstance(result, list):
        raise OpcodeAuditError(f"{context}: {field} must be an array of strings")
    _require_cardinality(len(result), limit, f"{context}: {field}")
    for item in result:
        if not isinstance(item, str):
            raise OpcodeAuditError(f"{context}: {field} must be an array of strings")
        _require_string_budget(item, f"{context}: {field} item")
    return result


def _require_boolean_field(
    value: Mapping[str, object], field: str, context: str
) -> bool:
    result = value.get(field)
    if type(result) is not bool:
        raise OpcodeAuditError(f"{context}: {field} must be a boolean")
    return result


def _require_integer_array_field(
    value: Mapping[str, object], field: str, context: str
) -> list[int]:
    result = value.get(field)
    if (
        not isinstance(result, list)
        or len(result) > 2
        or not all(type(item) is int for item in result)
    ):
        raise OpcodeAuditError(f"{context}: {field} must be an array of integers")
    return result


@dataclass(frozen=True)
class IgnoredOpcode:
    byte: int
    reason: str


@dataclass(frozen=True)
class OpcodeExclusionKind:
    allow_canonical_byte: bool
    require_inactive: bool


@dataclass(frozen=True)
class UpstreamPolicy:
    aliases: Mapping[str, str]
    ignored: Mapping[str, IgnoredOpcode]
    exclusion_kinds: Mapping[str, OpcodeExclusionKind]


@dataclass(frozen=True)
class AuditResult:
    neverd_count: int
    upstream_count: int
    ignored_count: int


@dataclass(frozen=True)
class GethOpcodeSource:
    text: str
    revision: str
    authority_ref: str | None = None


@dataclass(frozen=True)
class NeverDOpcodeMetadata:
    name: str
    byte: int
    stack_pops: int
    stack_pushes: int
    introduced: str


@dataclass(frozen=True)
class GethRuleClassification:
    category: str
    expected_fork: str


@dataclass(frozen=True)
class DynamicStackImmediateSpec:
    family: str
    operation_kind: str
    valid_stack_delta: int


@dataclass(frozen=True)
class EIP8024ImmediatePolicy:
    single: Mapping[int, tuple[int, ...]]
    pair: Mapping[int, tuple[int, ...]]


@dataclass(frozen=True)
class UpstreamSemanticsPolicy:
    rule_fields: tuple[str, ...]
    rule_classifications: Mapping[str, GethRuleClassification]
    fork_rules: tuple[tuple[str, str | None], ...]
    base_min_stack: Mapping[str, int]
    active_without_cost: Mapping[str, str]
    dynamic_stack_immediates: Mapping[str, DynamicStackImmediateSpec]


@dataclass(frozen=True)
class SemanticsAuditResult:
    opcode_count: int
    base_min_stack_override_count: int
    dynamic_stack_immediate_count: int


@dataclass(frozen=True)
class EIP8024AuditResult:
    table_count: int
    active_targets: tuple[str, ...]
    observation_count: int
    missing_operand_count: int


@dataclass(frozen=True)
class AuditInputPaths:
    neverd_opcodes: Path = DEFAULT_NEVERD_OPCODES
    neverd_hardforks: Path = DEFAULT_NEVERD_HARDFORKS
    neverd_constants: Path = DEFAULT_NEVERD_CONSTANTS
    opcode_policy: Path = DEFAULT_POLICY
    semantics_policy: Path = DEFAULT_SEMANTICS_POLICY
    geth_fork_aliases: Path = DEFAULT_GETH_FORK_ALIASES
    eip8024_policy: Path = DEFAULT_EIP8024_POLICY


def _isolate_git_environment(environment: dict[str, str]) -> None:
    for name in tuple(environment):
        # Git gains new environment switches over time. Start from no inherited
        # Git policy instead of maintaining a deny-list that can silently age.
        if name == "GIT_CONFIG" or name.startswith("GIT_"):
            environment.pop(name)
    environment.pop("SSH_ASKPASS", None)
    environment.pop("SSH_ASKPASS_REQUIRE", None)
    environment["GIT_CONFIG_NOSYSTEM"] = "1"
    environment["GIT_CONFIG_GLOBAL"] = os.devnull
    environment["GIT_ATTR_NOSYSTEM"] = "1"
    environment["GIT_CONFIG_COUNT"] = "3"
    environment["GIT_CONFIG_KEY_0"] = "core.hooksPath"
    environment["GIT_CONFIG_VALUE_0"] = os.devnull
    environment["GIT_CONFIG_KEY_1"] = "core.attributesFile"
    environment["GIT_CONFIG_VALUE_1"] = os.devnull
    environment["GIT_CONFIG_KEY_2"] = "protocol.ext.allow"
    environment["GIT_CONFIG_VALUE_2"] = "never"
    environment["GIT_NO_REPLACE_OBJECTS"] = "1"
    environment["GIT_TERMINAL_PROMPT"] = "0"


def _minimal_subprocess_environment() -> dict[str, str]:
    return {
        name: os.environ[name]
        for name in SUBPROCESS_ENVIRONMENT_ALLOWLIST
        if name in os.environ
    }


def _run_git(
    arguments: Sequence[str],
    *,
    git_executable: str = DEFAULT_GIT_EXECUTABLE,
) -> str:
    environment = _minimal_subprocess_environment()
    _isolate_git_environment(environment)
    operation = next(
        (argument for argument in arguments if not argument.startswith("-")),
        "command",
    )
    try:
        result = _run_bounded_process(
            (git_executable, *arguments),
            cwd=None,
            environment=environment,
            timeout_seconds=GIT_FETCH_TIMEOUT_SECONDS,
        )
    except FileNotFoundError as error:
        raise OpcodeAuditError(f"Git executable not found: {git_executable}") from error
    except subprocess.TimeoutExpired as error:
        raise OpcodeAuditError(
            f"git {operation} exceeded {GIT_FETCH_TIMEOUT_SECONDS} seconds"
        ) from error
    except OSError as error:
        raise OpcodeAuditError(f"could not execute Git: {error}") from error

    if result.returncode != 0:
        if result.stderr:
            detail = _process_stream_diagnostic(result.stderr, result.stderr_sha256)
        elif result.stdout:
            detail = _process_stream_diagnostic(result.stdout, result.stdout_sha256)
        else:
            detail = f"exit status {result.returncode}"
        raise OpcodeAuditError(f"git {operation} failed: {detail}")
    return result.stdout


def _validate_geth_fetch(remote: str, ref: str) -> None:
    if not remote or remote.startswith("-"):
        raise OpcodeAuditError("go-ethereum remote must be a non-option value")
    if (
        not GIT_REF_RE.fullmatch(ref)
        or ".." in ref
        or "@{" in ref
        or ref.endswith(("/", ".", ".lock"))
    ):
        raise OpcodeAuditError(
            "go-ethereum ref must be HEAD or a full refs/heads/... or "
            "refs/tags/... name"
        )


def _parse_cache_config(text: str, cache: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for record in text.split("\0"):
        if not record:
            continue
        if "\n" not in record:
            raise OpcodeAuditError(
                f"go-ethereum cache has malformed local Git config: {cache}"
            )
        name, value = record.split("\n", maxsplit=1)
        if name in result:
            raise OpcodeAuditError(
                f"go-ethereum cache repeats local Git config {name}: {cache}"
            )
        result[name] = value
    return result


def _validate_bare_cache(cache: Path, git_executable: str) -> None:
    if cache.is_symlink() or not cache.is_dir():
        raise OpcodeAuditError(
            f"go-ethereum cache is not a private bare Git repository: {cache}"
        )
    for relative in (Path("config"), Path("HEAD"), Path("objects"), Path("refs")):
        entry = cache / relative
        if entry.is_symlink() or not entry.exists():
            raise OpcodeAuditError(
                "go-ethereum cache is not a safe bare Git repository: "
                f"missing or linked {entry}"
            )
    config_path = cache / "config"
    if not config_path.is_file():
        raise OpcodeAuditError(
            f"go-ethereum cache has no regular local Git config: {config_path}"
        )
    config = _parse_cache_config(
        _run_git(
            (
                "config",
                "--file",
                str(config_path),
                "--null",
                "--list",
                "--no-includes",
            ),
            git_executable=git_executable,
        ),
        cache,
    )
    unexpected = sorted(
        set(config) - set(GETH_CACHE_REQUIRED_CONFIG) - GETH_CACHE_BOOLEAN_CONFIG
    )
    if unexpected:
        raise OpcodeAuditError(
            "go-ethereum cache has unexpected local Git config: "
            + ", ".join(unexpected)
        )
    for name, expected in GETH_CACHE_REQUIRED_CONFIG.items():
        if config.get(name) != expected:
            raise OpcodeAuditError(
                f"go-ethereum cache local Git config {name} must be {expected}"
            )
    for name in GETH_CACHE_BOOLEAN_CONFIG & set(config):
        if config[name] not in {"true", "false"}:
            raise OpcodeAuditError(
                f"go-ethereum cache local Git config {name} must be boolean"
            )
    for relative in GETH_CACHE_FORBIDDEN_PATHS:
        entry = cache / relative
        if entry.exists() or entry.is_symlink():
            raise OpcodeAuditError(
                f"go-ethereum cache contains forbidden object indirection: {entry}"
            )
    is_bare = _run_git(
        (f"--git-dir={cache}", "rev-parse", "--is-bare-repository"),
        git_executable=git_executable,
    ).strip()
    if is_bare != "true":
        raise OpcodeAuditError(
            f"go-ethereum cache is not a bare Git repository: {cache}"
        )


def _prepare_bare_cache(cache: Path, git_executable: str) -> None:
    if cache.exists():
        _validate_bare_cache(cache, git_executable)
        return

    cache.parent.mkdir(parents=True, exist_ok=True)
    _run_git(
        ("init", "--bare", "--quiet", "--template=", str(cache)),
        git_executable=git_executable,
    )
    _validate_bare_cache(cache, git_executable)


def fetch_geth_opcode_source(
    *,
    remote: str,
    ref: str,
    cache: Path,
    git_executable: str = DEFAULT_GIT_EXECUTABLE,
) -> GethOpcodeSource:
    """Fetch one upstream revision and read its opcode source from Git."""

    _validate_geth_fetch(remote, ref)
    _prepare_bare_cache(cache, git_executable)
    git_directory = f"--git-dir={cache}"
    authority_ref = (
        f"{GETH_CACHE_REF_PREFIX}/{secrets.token_hex(GETH_CACHE_REF_TOKEN_BYTES)}"
    )
    cache_refspec = f"+{ref}:{authority_ref}"
    try:
        _run_git(
            (
                git_directory,
                "fetch",
                "--quiet",
                "--no-tags",
                "--depth=1",
                "--force",
                remote,
                cache_refspec,
            ),
            git_executable=git_executable,
        )
        revision = _run_git(
            (
                git_directory,
                "rev-parse",
                "--verify",
                f"{authority_ref}^{{commit}}",
            ),
            git_executable=git_executable,
        ).strip()
        if not GIT_OBJECT_ID_RE.fullmatch(revision):
            raise OpcodeAuditError(
                f"go-ethereum fetch produced an invalid Git object ID: {revision!r}"
            )
        text = _run_git(
            (git_directory, "show", f"{revision}:{GETH_OPCODE_PATH.as_posix()}"),
            git_executable=git_executable,
        )
    except BaseException:
        try:
            _run_git(
                (git_directory, "update-ref", "-d", authority_ref),
                git_executable=git_executable,
            )
        except OpcodeAuditError:
            pass
        raise
    return GethOpcodeSource(text=text, revision=revision, authority_ref=authority_ref)


@contextmanager
def checkout_geth_revision(
    *,
    cache: Path,
    revision: str,
    authority_ref: str | None = None,
    git_executable: str = DEFAULT_GIT_EXECUTABLE,
) -> Iterator[Path]:
    """Materialize one fetched revision as a temporary detached worktree."""

    if not GIT_OBJECT_ID_RE.fullmatch(revision):
        raise OpcodeAuditError(
            f"go-ethereum checkout received an invalid Git object ID: {revision!r}"
        )
    _prepare_bare_cache(cache, git_executable)
    git_directory = f"--git-dir={cache}"
    owned_authority_ref: str | None = None
    if authority_ref is not None:
        if not GETH_CACHE_AUTHORITY_REF_RE.fullmatch(authority_ref):
            raise OpcodeAuditError(
                f"go-ethereum checkout received an invalid authority ref: "
                f"{authority_ref!r}"
            )
        owned_authority_ref = authority_ref
    try:
        if owned_authority_ref is not None:
            authoritative_revision = _run_git(
                (
                    git_directory,
                    "rev-parse",
                    "--verify",
                    f"{owned_authority_ref}^{{commit}}",
                ),
                git_executable=git_executable,
            ).strip()
            if authoritative_revision != revision:
                raise OpcodeAuditError(
                    "go-ethereum authority ref no longer names the fetched SHA"
                )
        with tempfile.TemporaryDirectory(prefix="neverd-geth-worktree-") as temporary:
            checkout = Path(temporary) / "go-ethereum"
            worktree_added = False
            try:
                _run_git(
                    (
                        git_directory,
                        "worktree",
                        "add",
                        "--quiet",
                        "--detach",
                        str(checkout),
                        revision,
                    ),
                    git_executable=git_executable,
                )
                worktree_added = True
                yield checkout
            finally:
                if worktree_added:
                    _run_git(
                        (
                            git_directory,
                            "worktree",
                            "remove",
                            "--force",
                            str(checkout),
                        ),
                        git_executable=git_executable,
                    )
    finally:
        if owned_authority_ref is not None:
            _run_git(
                (git_directory, "update-ref", "-d", owned_authority_ref),
                git_executable=git_executable,
            )


def _path_is_within(path: Path, root: Path) -> bool:
    return path == root or root in path.parents


def _resolve_executable_path(executable: str, context: str) -> Path:
    if not executable:
        raise OpcodeAuditError(f"{context} executable must not be empty")
    candidate: str | None
    if Path(executable).is_absolute() or os.sep in executable:
        candidate = executable
    else:
        candidate = shutil.which(executable)
    if candidate is None:
        raise OpcodeAuditError(f"{context} executable not found: {executable}")
    try:
        resolved = Path(candidate).resolve(strict=True)
    except (OSError, RuntimeError) as error:
        raise OpcodeAuditError(
            f"could not resolve {context} executable {executable}: {error}"
        ) from error
    if not resolved.is_file() or not os.access(resolved, os.X_OK):
        raise OpcodeAuditError(
            f"{context} executable is not an executable regular file: {resolved}"
        )
    return resolved


def _reject_broad_capability_root(path: Path, context: str) -> None:
    protected_roots = (
        Path.home().resolve(),
        REPO_ROOT.resolve(),
        Path(tempfile.gettempdir()).resolve(),
    )
    if path == Path(path.anchor) or any(
        _path_is_within(protected, path) for protected in protected_roots
    ):
        raise OpcodeAuditError(f"{context} must not expose broad host root {path}")


def _canonical_capability_directory(path: Path, context: str) -> Path:
    try:
        resolved = path.resolve(strict=True)
    except (OSError, RuntimeError) as error:
        raise OpcodeAuditError(
            f"could not resolve {context} {path}: {error}"
        ) from error
    if not resolved.is_dir():
        raise OpcodeAuditError(f"{context} is not a directory: {resolved}")
    _reject_broad_capability_root(resolved, context)
    return resolved


def _validate_go_toolchain_paths(
    go_executable: Path, reported_go_root: str
) -> tuple[Path, Path]:
    resolved_executable = _resolve_executable_path(str(go_executable), "Go")
    if not reported_go_root or not Path(reported_go_root).is_absolute():
        raise OpcodeAuditError("Go reported a non-absolute GOROOT")
    resolved_go_root = _canonical_capability_directory(
        Path(reported_go_root), "Go GOROOT"
    )
    try:
        resolved_bin = (resolved_go_root / "bin").resolve(strict=True)
    except (OSError, RuntimeError) as error:
        raise OpcodeAuditError(
            f"could not resolve GOROOT/bin for {resolved_go_root}: {error}"
        ) from error
    if not resolved_bin.is_dir() or resolved_executable.parent != resolved_bin:
        raise OpcodeAuditError(
            f"Go executable {resolved_executable} resolves outside GOROOT/bin "
            f"for {resolved_go_root}"
        )
    inferred_go_root = resolved_executable.parent.parent.resolve(strict=True)
    if inferred_go_root != resolved_go_root:
        raise OpcodeAuditError(
            "Go executable distribution root does not equal the reported GOROOT"
        )
    return resolved_executable, resolved_go_root


def _infer_go_distribution_root(go_executable: Path) -> Path:
    resolved_executable = _resolve_executable_path(str(go_executable), "Go")
    if resolved_executable.parent.name != "bin":
        raise OpcodeAuditError(
            f"Go executable is not in a canonical bin directory: {resolved_executable}"
        )
    inferred_root = _canonical_capability_directory(
        resolved_executable.parent.parent, "inferred Go distribution root"
    )
    if resolved_executable.parent != (inferred_root / "bin").resolve(strict=True):
        raise OpcodeAuditError(
            f"Go executable resolves outside inferred GOROOT/bin: {resolved_executable}"
        )
    return inferred_root


def _require_unlinked_regular_file(
    path: Path, context: str, *, containing_root: Path | None = None
) -> Path:
    if path.is_symlink():
        raise OpcodeAuditError(f"{context} must not be a symbolic link: {path}")
    try:
        resolved = path.resolve(strict=True)
    except (OSError, RuntimeError) as error:
        raise OpcodeAuditError(
            f"could not resolve {context} {path}: {error}"
        ) from error
    if not resolved.is_file():
        raise OpcodeAuditError(f"{context} is not a regular file: {resolved}")
    if containing_root is not None and not _path_is_within(resolved, containing_root):
        raise OpcodeAuditError(
            f"{context} resolves outside its capability root: {resolved}"
        )
    return resolved


def _canonical_sandbox_directories(
    paths: Sequence[Path], context: str
) -> tuple[Path, ...]:
    result: list[Path] = []
    seen: set[Path] = set()
    for index, path in enumerate(paths):
        resolved = _canonical_capability_directory(path, f"{context} #{index}")
        if resolved not in seen:
            seen.add(resolved)
            result.append(resolved)
    if not result:
        raise OpcodeAuditError(f"{context} must not be empty")
    return tuple(result)


def _existing_canonical_paths(paths: Sequence[Path]) -> tuple[Path, ...]:
    result: list[Path] = []
    seen: set[Path] = set()
    for path in paths:
        try:
            resolved = path.resolve(strict=True)
        except (OSError, RuntimeError):
            continue
        if resolved in seen:
            continue
        seen.add(resolved)
        result.append(resolved)
    return tuple(result)


def _sandbox_profile_string(value: Path) -> str:
    raw = str(value)
    if not raw.isprintable():
        raise OpcodeAuditError("Darwin sandbox path contains a control character")
    return raw.replace("\\", "\\\\").replace('"', '\\"')


def _write_darwin_sandbox_profile(
    *,
    executable: Path,
    readable_roots: Sequence[Path],
    writable_roots: Sequence[Path],
    profile_root: Path,
    network_allowed: bool,
) -> Path:
    readable_paths = set(_existing_canonical_paths(DARWIN_SANDBOX_SYSTEM_READ_PATHS))
    readable_paths.update(readable_roots)
    readable_paths.add(executable)
    writable_paths = set(writable_roots)
    system_write_paths = _existing_canonical_paths(DARWIN_SANDBOX_SYSTEM_WRITE_PATHS)

    lines = [
        "(version 1)",
        f'(import "{DARWIN_SANDBOX_DYLD_PROFILE}")',
        "(deny default)",
        "(allow process*)",
        "(allow sysctl-read)",
        "(allow mach-lookup)",
    ]
    if network_allowed:
        lines.append("(allow network*)")
    for path in sorted(readable_paths, key=str):
        filter_name = "subpath" if path.is_dir() else "literal"
        lines.append(
            f'(allow file-read* ({filter_name} "{_sandbox_profile_string(path)}"))'
        )
    for path in DARWIN_SANDBOX_SYSTEM_METADATA_PATHS:
        lines.append(
            "(allow file-read-metadata file-test-existence "
            f'(literal "{_sandbox_profile_string(path)}"))'
        )
    for path in sorted(writable_paths, key=str):
        lines.append(f'(allow file-write* (subpath "{_sandbox_profile_string(path)}"))')
    for path in sorted(system_write_paths, key=str):
        lines.append(f'(allow file-write* (literal "{_sandbox_profile_string(path)}"))')

    with tempfile.NamedTemporaryFile(
        mode="w",
        encoding="utf-8",
        dir=profile_root,
        prefix=".neverd-evm-sandbox-",
        suffix=".sb",
        delete=False,
    ) as output:
        output.write("\n".join(lines) + "\n")
        profile = Path(output.name)
    profile.chmod(0o600)
    return profile


def _linux_system_mounts() -> tuple[tuple[Path, Path], ...]:
    result: list[tuple[Path, Path]] = []
    seen_destinations: set[Path] = set()
    for declared_path in LINUX_SANDBOX_SYSTEM_READ_PATHS:
        try:
            source = declared_path.resolve(strict=True)
        except (OSError, RuntimeError):
            continue
        destination = Path(os.path.abspath(declared_path))
        if destination in seen_destinations:
            continue
        seen_destinations.add(destination)
        result.append((source, destination))
    return tuple(result)


def _bubblewrap_directories(
    mounts: Sequence[tuple[Path, Path]], writable_roots: Sequence[Path]
) -> tuple[Path, ...]:
    result: set[Path] = {Path("/proc"), Path("/dev")}
    for source, destination in (*mounts, *((path, path) for path in writable_roots)):
        current = destination if source.is_dir() else destination.parent
        while current != Path(current.anchor):
            result.add(current)
            current = current.parent
    return tuple(sorted(result, key=lambda path: (len(path.parts), str(path))))


def _run_go(
    arguments: Sequence[str],
    *,
    cwd: Path,
    go_executable: str,
    environment_root: Path,
    go_toolchain: str,
    timeout_seconds: float,
    input_text: str | None = None,
    network_allowed: bool = True,
    sandbox_required: bool = False,
    sandbox_readable_roots: Sequence[Path] = (),
    sandbox_writable_roots: Sequence[Path] = (),
) -> str:
    if timeout_seconds <= 0:
        raise OpcodeAuditError("Go command timeout must be greater than zero")
    if go_toolchain not in GO_TOOLCHAIN_CHOICES:
        raise OpcodeAuditError(
            f"Go toolchain mode must be one of {', '.join(GO_TOOLCHAIN_CHOICES)}"
        )
    environment = _minimal_subprocess_environment()
    _isolate_git_environment(environment)
    environment_root.mkdir(parents=True, exist_ok=True)
    environment_root = environment_root.resolve(strict=True)
    go_path = environment_root / "path"
    module_cache = environment_root / "module-cache"
    build_cache = environment_root / "build-cache"
    temporary = environment_root / "temporary"
    home = environment_root / "home"
    xdg_config = environment_root / "xdg-config"
    xdg_cache = environment_root / "xdg-cache"
    xdg_data = environment_root / "xdg-data"
    xdg_state = environment_root / "xdg-state"
    for directory in (
        go_path,
        module_cache,
        build_cache,
        temporary,
        home,
        xdg_config,
        xdg_cache,
        xdg_data,
        xdg_state,
    ):
        directory.mkdir(parents=True, exist_ok=True)
    # The probe only reads jump-table metadata; native crypto acceleration is
    # irrelevant and would make this audit depend on the runner's C toolchain.
    environment["CGO_ENABLED"] = "0"
    environment["GO111MODULE"] = "on"
    environment["GOCACHE"] = str(build_cache)
    environment["GOCACHEPROG"] = ""
    environment["GOENV"] = "off"
    environment["GOFLAGS"] = ""
    environment["GOINSECURE"] = ""
    environment["GOMODCACHE"] = str(module_cache)
    environment["GONOPROXY"] = ""
    environment["GONOSUMDB"] = ""
    environment["GOPATH"] = str(go_path)
    environment["GOPRIVATE"] = ""
    environment["GOPROXY"] = GO_MODULE_PROXY
    environment["GOSUMDB"] = GO_CHECKSUM_DATABASE
    environment["GOTMPDIR"] = str(temporary)
    environment["GOTOOLCHAIN"] = go_toolchain
    environment["GOVCS"] = "public:git,private:off"
    environment["GOWORK"] = "off"
    environment["HOME"] = str(home)
    environment["TMPDIR"] = str(temporary)
    environment["XDG_CONFIG_HOME"] = str(xdg_config)
    environment["XDG_CACHE_HOME"] = str(xdg_cache)
    environment["XDG_DATA_HOME"] = str(xdg_data)
    environment["XDG_STATE_HOME"] = str(xdg_state)
    command: tuple[str, ...] = (go_executable, *arguments)
    if not network_allowed:
        environment["GOPROXY"] = "off"
        environment["GOSUMDB"] = "off"
        environment["GOVCS"] = "*:off"
    if sandbox_required:
        command = _sandboxed_command(
            command,
            cwd=cwd,
            readable_roots=sandbox_readable_roots,
            writable_roots=(environment_root, *sandbox_writable_roots),
            profile_root=environment_root.parent,
            network_allowed=network_allowed,
        )
    try:
        result = _run_bounded_process(
            command,
            cwd=cwd,
            environment=environment,
            input_text=input_text,
            timeout_seconds=timeout_seconds,
        )
    except FileNotFoundError as error:
        raise OpcodeAuditError(f"Go executable not found: {go_executable}") from error
    except subprocess.TimeoutExpired as error:
        raise OpcodeAuditError(GO_PROBE_TIMEOUT_DIAGNOSTIC) from error
    except OSError as error:
        raise OpcodeAuditError(f"could not execute Go: {error}") from error
    if result.returncode != 0:
        if result.stderr:
            detail = _process_stream_diagnostic(result.stderr, result.stderr_sha256)
        elif result.stdout:
            detail = _process_stream_diagnostic(result.stdout, result.stdout_sha256)
        else:
            detail = f"exit status {result.returncode}"
        raise OpcodeAuditError(f"go {arguments[0]} failed: {detail}")
    return result.stdout


def _sandboxed_command(
    command: Sequence[str],
    *,
    cwd: Path,
    readable_roots: Sequence[Path],
    writable_roots: Sequence[Path],
    profile_root: Path,
    network_allowed: bool,
) -> tuple[str, ...]:
    if not command:
        raise OpcodeAuditError("sandbox command must not be empty")
    executable = _resolve_executable_path(command[0], "sandboxed command")
    resolved_command = (str(executable), *command[1:])
    resolved_readable_roots = _canonical_sandbox_directories(
        readable_roots, "sandbox readable roots"
    )
    resolved_writable_roots = _canonical_sandbox_directories(
        writable_roots, "sandbox writable roots"
    )
    resolved_cwd = _canonical_capability_directory(cwd, "sandbox working directory")
    resolved_profile_root = _canonical_capability_directory(
        profile_root, "sandbox profile root"
    )
    for path, context in (
        (resolved_cwd, "sandbox working directory"),
        (resolved_profile_root, "sandbox profile root"),
        *((path, "sandbox writable root") for path in resolved_writable_roots),
    ):
        if not any(_path_is_within(path, root) for root in resolved_readable_roots):
            raise OpcodeAuditError(
                f"{context} {path} is outside every sandbox readable root"
            )

    if sys.platform == "darwin":
        sandbox_executable = shutil.which(DARWIN_SANDBOX_EXECUTABLE)
        if sandbox_executable is None:
            raise OpcodeAuditError(
                "Darwin EVM upstream execution requires sandbox-exec"
            )
        profile = _write_darwin_sandbox_profile(
            executable=executable,
            readable_roots=resolved_readable_roots,
            writable_roots=resolved_writable_roots,
            profile_root=resolved_profile_root,
            network_allowed=network_allowed,
        )
        return (sandbox_executable, "-f", str(profile), *resolved_command)
    if sys.platform.startswith("linux"):
        bubblewrap = shutil.which(LINUX_SANDBOX_EXECUTABLE)
        if bubblewrap is None:
            raise OpcodeAuditError(
                "Linux EVM upstream execution requires bubblewrap (bwrap)"
            )
        mounts: list[tuple[Path, Path]] = [
            (path, path) for path in resolved_readable_roots
        ]
        mounts.extend(_linux_system_mounts())
        if not any(
            _path_is_within(executable, source)
            for source, _destination in mounts
            if source.is_dir()
        ):
            mounts.append((executable, executable))
        unique_mounts: dict[Path, Path] = {}
        for source, destination in mounts:
            if source == Path(source.anchor) or destination == Path(destination.anchor):
                raise OpcodeAuditError("Linux sandbox refuses a host root bind")
            previous = unique_mounts.setdefault(destination, source)
            if previous != source:
                raise OpcodeAuditError(
                    f"Linux sandbox destination {destination} has conflicting sources"
                )

        arguments: list[str] = [
            bubblewrap,
            "--die-with-parent",
            "--new-session",
            "--unshare-pid",
            "--unshare-uts",
            "--unshare-ipc",
            "--cap-drop",
            "ALL",
        ]
        if not network_allowed:
            arguments.append("--unshare-net")
        for directory in _bubblewrap_directories(
            tuple(
                (source, destination) for destination, source in unique_mounts.items()
            ),
            resolved_writable_roots,
        ):
            arguments.extend(("--dir", str(directory)))
        for destination, source in sorted(
            unique_mounts.items(), key=lambda item: (len(item[0].parts), str(item[0]))
        ):
            arguments.extend(("--ro-bind", str(source), str(destination)))
        for path in sorted(
            resolved_writable_roots, key=lambda value: (len(value.parts), str(value))
        ):
            arguments.extend(("--bind", str(path), str(path)))
        arguments.extend(
            (
                "--proc",
                "/proc",
                "--dev",
                "/dev",
                "--chdir",
                str(resolved_cwd),
                "--",
                *resolved_command,
            )
        )
        return tuple(arguments)
    raise OpcodeAuditError(
        f"unsupported platform for fail-closed EVM upstream sandbox: {sys.platform}"
    )


def run_geth_opcode_probe(
    *,
    geth_root: Path,
    geth_revision: str,
    request: Mapping[str, object],
    helper: Path = DEFAULT_GETH_PROBE,
    eip8024_overlay: Path = DEFAULT_GETH_EIP8024_OVERLAY,
    go_executable: str = DEFAULT_GO_EXECUTABLE,
    go_toolchain: str = DEFAULT_GO_TOOLCHAIN,
    audit_unix_time: int | None = None,
    sandbox_required: bool = True,
) -> dict[str, object]:
    """Run the exported geth jump-table probe from an external Go module."""

    deadline = time.monotonic() + GO_PROBE_TOTAL_TIMEOUT_SECONDS

    if not isinstance(request, Mapping):
        raise OpcodeAuditError("geth opcode probe request is not an object")
    reserved = sorted(set(request) & PROBE_TRUSTED_FIELDS)
    if reserved:
        raise OpcodeAuditError(
            "geth opcode probe request attempts to override trusted fields: "
            + ", ".join(reserved)
        )
    _require_exact_fields(request, PROBE_REQUEST_FIELDS, "geth opcode probe request")
    if not GIT_OBJECT_ID_RE.fullmatch(geth_revision):
        raise OpcodeAuditError(
            f"geth opcode probe received an invalid Git object ID: {geth_revision!r}"
        )
    if audit_unix_time is None:
        audit_unix_time = int(time.time())
    if audit_unix_time <= 0:
        raise OpcodeAuditError("geth opcode probe audit Unix time must be positive")
    trusted_identity = {
        "schema_version": GETH_PROBE_SCHEMA_VERSION,
        "authority": GETH_AUDIT_AUTHORITY,
        "geth_remote": DEFAULT_GETH_REMOTE,
        "geth_ref": DEFAULT_GETH_REF,
        "geth_revision": geth_revision,
        "audit_unix_time": audit_unix_time,
    }
    probe_request = {**request, **trusted_identity}
    encoded_probe_request = json.dumps(
        probe_request, ensure_ascii=False, separators=(",", ":")
    )
    _require_probe_request_bytes(encoded_probe_request)

    geth_root = _canonical_capability_directory(geth_root, "go-ethereum worktree")
    geth_module = _require_unlinked_regular_file(
        geth_root / "go.mod",
        "go-ethereum go.mod",
        containing_root=geth_root,
    )
    geth_sums = _require_unlinked_regular_file(
        geth_root / "go.sum",
        "go-ethereum go.sum",
        containing_root=geth_root,
    )
    helper = _require_unlinked_regular_file(helper, "geth opcode probe helper")
    eip8024_overlay = _require_unlinked_regular_file(
        eip8024_overlay, "geth EIP-8024 overlay helper"
    )
    resolved_go_executable = _resolve_executable_path(go_executable, "Go")
    inferred_go_root = _infer_go_distribution_root(resolved_go_executable)
    version_match = GO_VERSION_RE.search(
        _read_bounded_utf8(geth_module, "go-ethereum go.mod")
    )
    if version_match is None:
        raise OpcodeAuditError(
            f"go-ethereum worktree has no valid Go version: {geth_module}"
        )
    geth_go_version = version_match.group(1)

    with tempfile.TemporaryDirectory(prefix="neverd-geth-probe-") as temporary:
        root = Path(temporary).resolve(strict=True)
        module = root / "module"
        module.mkdir()
        environment_root = root / "go-environment"
        overlay_source = root / "evm_geth_eip8024_overlay.go"
        shutil.copyfile(eip8024_overlay, overlay_source)
        vm_root = _canonical_capability_directory(
            geth_root / "core/vm", "go-ethereum core/vm"
        )
        if not _path_is_within(vm_root, geth_root):
            raise OpcodeAuditError(
                f"go-ethereum core/vm resolves outside its worktree: {vm_root}"
            )
        virtual_overlay = vm_root / "neverd_eip8024_audit_overlay.go"
        if virtual_overlay.exists() or virtual_overlay.is_symlink():
            raise OpcodeAuditError(
                "go-ethereum unexpectedly owns the reserved EIP-8024 audit "
                f"overlay path: {virtual_overlay}"
            )
        overlay_configuration = root / "overlay.json"
        overlay_configuration.write_text(
            json.dumps(
                {"Replace": {str(virtual_overlay): str(overlay_source.resolve())}}
            ),
            encoding="utf-8",
        )

        sandbox_readable_roots: tuple[Path, ...] = (
            root,
            geth_root,
            inferred_go_root,
        )

        def run_go(
            arguments: Sequence[str],
            *,
            cwd: Path,
            input_text: str | None = None,
            network_allowed: bool = True,
            module_writable: bool = False,
        ) -> str:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise OpcodeAuditError(GO_PROBE_TIMEOUT_DIAGNOSTIC)
            return _run_go(
                arguments,
                cwd=cwd,
                go_executable=str(resolved_go_executable),
                environment_root=environment_root,
                go_toolchain=go_toolchain,
                timeout_seconds=remaining,
                input_text=input_text,
                network_allowed=network_allowed,
                sandbox_required=sandbox_required,
                sandbox_readable_roots=sandbox_readable_roots,
                sandbox_writable_roots=(module,) if module_writable else (),
            )

        shutil.copyfile(helper, module / "main.go")
        toolchain_output = run_go(
            ("env", "-json", "GOROOT", "GOVERSION"),
            cwd=module,
            network_allowed=False,
        )
        try:
            toolchain_metadata = json.loads(toolchain_output)
        except json.JSONDecodeError as error:
            raise OpcodeAuditError(
                f"Go toolchain metadata is not valid JSON: {error}"
            ) from error
        if not isinstance(toolchain_metadata, dict):
            raise OpcodeAuditError("Go toolchain metadata root is not an object")
        _require_exact_fields(
            toolchain_metadata,
            GO_TOOLCHAIN_METADATA_FIELDS,
            "Go toolchain metadata",
        )
        reported_go_root = _require_string_field(
            toolchain_metadata, "GOROOT", "Go toolchain metadata"
        )
        go_version = _require_string_field(
            toolchain_metadata, "GOVERSION", "Go toolchain metadata"
        )
        resolved_go_executable, resolved_go_root = _validate_go_toolchain_paths(
            resolved_go_executable, reported_go_root
        )
        if resolved_go_root != inferred_go_root:
            raise OpcodeAuditError(
                "Go reported GOROOT differs from the executable distribution root"
            )
        sandbox_readable_roots = (root, geth_root, resolved_go_root)

        run_go(
            ("mod", "init", "neverd.dev/evm-upstream-audit"),
            cwd=module,
            network_allowed=False,
            module_writable=True,
        )
        shutil.copyfile(geth_sums, module / "go.sum")
        run_go(
            ("mod", "edit", f"-go={geth_go_version}"),
            cwd=module,
            network_allowed=False,
            module_writable=True,
        )
        run_go(
            (
                "mod",
                "edit",
                "-require=github.com/ethereum/go-ethereum@v0.0.0",
            ),
            cwd=module,
            network_allowed=False,
            module_writable=True,
        )
        run_go(
            (
                "mod",
                "edit",
                f"-replace=github.com/ethereum/go-ethereum={geth_root.resolve()}",
            ),
            cwd=module,
            network_allowed=False,
            module_writable=True,
        )
        run_go(
            ("mod", "tidy"),
            cwd=module,
            module_writable=True,
        )
        run_go(
            ("mod", "download", "all"),
            cwd=module,
            module_writable=True,
        )
        output = run_go(
            (
                "run",
                "-mod=readonly",
                f"-overlay={overlay_configuration}",
                "./main.go",
            ),
            cwd=module,
            input_text=encoded_probe_request,
            network_allowed=False,
        )
    try:
        decoded = json.loads(output)
    except json.JSONDecodeError as error:
        raise OpcodeAuditError(
            f"geth opcode probe returned invalid JSON: {error}"
        ) from error
    if not isinstance(decoded, dict):
        raise OpcodeAuditError("geth opcode probe JSON root is not an object")
    _require_exact_fields(
        decoded, GETH_MANIFEST_ROOT_FIELDS, "geth opcode probe manifest"
    )
    returned_revision = _require_string_field(
        decoded, "geth_revision", "geth opcode probe manifest"
    )
    returned_go_version = _require_string_field(
        decoded, "go_version", "geth opcode probe manifest"
    )
    if returned_revision != geth_revision:
        raise OpcodeAuditError("geth opcode probe revision does not match fetched SHA")
    if returned_go_version != go_version:
        raise OpcodeAuditError("geth opcode probe Go version does not match toolchain")
    if decoded.get("authority") != GETH_AUDIT_AUTHORITY:
        raise OpcodeAuditError("geth opcode probe authority identity drift")
    if decoded.get("geth_remote") != DEFAULT_GETH_REMOTE:
        raise OpcodeAuditError("geth opcode probe official remote identity drift")
    if decoded.get("geth_ref") != DEFAULT_GETH_REF:
        raise OpcodeAuditError("geth opcode probe requested ref identity drift")
    if decoded.get("audit_unix_time") != audit_unix_time:
        raise OpcodeAuditError("geth opcode probe audit Unix time drift")
    return decoded


def _atomic_write_json(path: Path, value: Mapping[str, object]) -> None:
    """Atomically replace *path* with a complete, fsynced JSON document."""

    path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            dir=path.parent,
            prefix=f".{path.name}.",
            suffix=".tmp",
            delete=False,
        ) as output:
            temporary_path = Path(output.name)
            json.dump(value, output, indent=2, sort_keys=True)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary_path, path)
        temporary_path = None
    finally:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)


def _format_opcode(value: int) -> str:
    return f"0x{value:0{OPCODE_HEX_DIGITS}x}"


def _insert_unique(result: dict[str, int], name: str, value: int, source: str) -> None:
    if name in result:
        previous = result[name]
        if previous != value:
            raise OpcodeAuditError(
                f"{source}: opcode {name} has conflicting values "
                f"{_format_opcode(previous)} and {_format_opcode(value)}"
            )
        raise OpcodeAuditError(f"{source}: duplicate opcode declaration {name}")
    result[name] = value


def parse_neverd_opcode_metadata(
    text: str,
) -> dict[str, NeverDOpcodeMetadata]:
    _require_macro_cardinality(
        text,
        NEVERD_OPCODE_DECLARATION_RE,
        MAX_OPCODE_RECORDS,
        "NeverD opcode metadata",
    )
    _require_macro_cardinality(
        text,
        NEVERD_OPCODE_NAME_ALIAS_DECLARATION_RE,
        MAX_OPCODE_RECORDS,
        "NeverD opcode alias metadata",
    )
    result: dict[str, NeverDOpcodeMetadata] = {}
    used_bytes: dict[int, str] = {}
    for match in NEVERD_OPCODE_METADATA_RE.finditer(text):
        name, byte_text, pops_text, pushes_text, introduced = match.groups()
        _require_string_budget(name, "NeverD opcode name")
        _require_string_budget(introduced, f"NeverD opcode {name} hardfork")
        value = int(byte_text, 16)
        if value > OPCODE_MAX:
            raise OpcodeAuditError(f"NeverD: {name} is outside the opcode byte")
        if name in result:
            raise OpcodeAuditError(f"NeverD: duplicate opcode name {name}")
        if value in used_bytes:
            raise OpcodeAuditError(
                f"NeverD: {name} and {used_bytes[value]} both use "
                f"{_format_opcode(value)}"
            )
        result[name] = NeverDOpcodeMetadata(
            name=name,
            byte=value,
            stack_pops=int(pops_text),
            stack_pushes=int(pushes_text),
            introduced=introduced,
        )
        used_bytes[value] = name
    _require_all_macro_records_parsed(
        text,
        NEVERD_OPCODE_DECLARATION_RE,
        len(result),
        "EVM_OPCODE",
        "NeverD opcode metadata",
    )
    raw_aliases = NEVERD_OPCODE_NAME_ALIAS_RE.findall(text)
    _require_all_macro_records_parsed(
        text,
        NEVERD_OPCODE_NAME_ALIAS_DECLARATION_RE,
        len(raw_aliases),
        "EVM_OPCODE_NAME_ALIAS",
        "NeverD opcode metadata",
    )
    seen_aliases: set[str] = set()
    for opcode, alias, first_fork, last_fork in raw_aliases:
        for value, context in (
            (opcode, "NeverD opcode alias owner"),
            (alias, "NeverD opcode alias"),
            (first_fork, f"NeverD opcode alias {alias} first fork"),
            (last_fork, f"NeverD opcode alias {alias} last fork"),
        ):
            _require_string_budget(value, context)
        if opcode not in result:
            raise OpcodeAuditError(
                f"NeverD opcode alias {alias} names unknown opcode {opcode}"
            )
        if alias in seen_aliases:
            raise OpcodeAuditError(f"NeverD: duplicate opcode alias {alias}")
        seen_aliases.add(alias)
    if not result:
        raise OpcodeAuditError("NeverD: no EVM_OPCODE records found")
    return result


def parse_neverd_opcodes(text: str) -> dict[str, int]:
    return {
        name: metadata.byte
        for name, metadata in parse_neverd_opcode_metadata(text).items()
    }


def parse_neverd_hardforks(text: str) -> tuple[str, ...]:
    _require_macro_cardinality(
        text,
        NEVERD_HARDFORK_DECLARATION_RE,
        MAX_HARDFORK_RECORDS,
        "NeverD hardfork metadata",
    )
    _require_macro_cardinality(
        text,
        NEVERD_HARDFORK_ALIAS_DECLARATION_RE,
        MAX_HARDFORK_RECORDS,
        "NeverD hardfork alias metadata",
    )
    _require_macro_cardinality(
        text,
        NEVERD_NEWEST_HARDFORK_DECLARATION_RE,
        1,
        "NeverD newest hardfork metadata",
    )
    hardforks = NEVERD_HARDFORK_RE.findall(text)
    _require_all_macro_records_parsed(
        text,
        NEVERD_HARDFORK_DECLARATION_RE,
        len(hardforks),
        "EVM_HARDFORK",
        "NeverD hardfork metadata",
    )
    if not hardforks:
        raise OpcodeAuditError("NeverD: no EVM_HARDFORK records found")
    if len(hardforks) != len(set(hardforks)):
        raise OpcodeAuditError("NeverD: duplicate EVM_HARDFORK record")
    for hardfork in hardforks:
        _require_string_budget(hardfork, "NeverD hardfork name")
    canonical = set(hardforks)
    raw_aliases = NEVERD_HARDFORK_ALIAS_RE.findall(text)
    _require_all_macro_records_parsed(
        text,
        NEVERD_HARDFORK_ALIAS_DECLARATION_RE,
        len(raw_aliases),
        "EVM_HARDFORK_ALIAS",
        "NeverD hardfork metadata",
    )
    _require_cardinality(
        len(raw_aliases), MAX_HARDFORK_RECORDS, "NeverD hardfork alias"
    )
    seen_spellings: set[str] = set()
    for spelling, hardfork in raw_aliases:
        _require_string_budget(spelling, "NeverD hardfork alias spelling")
        _require_string_budget(hardfork, f"NeverD hardfork alias {spelling}")
        if spelling in seen_spellings:
            raise OpcodeAuditError(
                f"NeverD: duplicate hardfork alias spelling {spelling}"
            )
        if hardfork not in canonical:
            raise OpcodeAuditError(
                f"NeverD: hardfork alias {spelling} names unknown {hardfork}"
            )
        seen_spellings.add(spelling)
    newest = NEVERD_NEWEST_HARDFORK_RE.findall(text)
    _require_all_macro_records_parsed(
        text,
        NEVERD_NEWEST_HARDFORK_DECLARATION_RE,
        len(newest),
        "EVM_HARDFORK_NEWEST",
        "NeverD hardfork metadata",
    )
    if len(newest) > 1:
        raise OpcodeAuditError("NeverD: multiple EVM_HARDFORK_NEWEST records")
    if newest and newest[0] not in canonical:
        raise OpcodeAuditError(
            f"NeverD: newest marker names unknown canonical hardfork {newest[0]}"
        )
    return tuple(hardforks)


def parse_neverd_latest_hardfork(text: str, hardforks: Sequence[str]) -> str:
    _require_macro_cardinality(
        text,
        NEVERD_LATEST_HARDFORK_DECLARATION_RE,
        MAX_HARDFORK_RECORDS,
        "NeverD latest hardfork metadata",
    )
    latest = NEVERD_LATEST_HARDFORK_RE.findall(text)
    if len(NEVERD_LATEST_HARDFORK_DECLARATION_RE.findall(text)) != len(latest):
        raise OpcodeAuditError("NeverD: unparsed EVM_HARDFORK_LATEST record")
    if len(latest) != 1:
        raise OpcodeAuditError(
            "NeverD: expected exactly one EVM_HARDFORK_LATEST record"
        )
    if latest[0] not in set(hardforks):
        raise OpcodeAuditError(
            f"NeverD: latest marker names unknown canonical hardfork {latest[0]}"
        )
    return latest[0]


def parse_geth_fork_aliases(text: str, hardforks: Sequence[str]) -> dict[str, str]:
    _require_macro_cardinality(
        text,
        GETH_FORK_ALIAS_DECLARATION_RE,
        MAX_FORK_ALIAS_RECORDS,
        "geth fork aliases",
    )
    raw_aliases = GETH_FORK_ALIAS_RE.findall(text)
    if len(GETH_FORK_ALIAS_DECLARATION_RE.findall(text)) != len(raw_aliases):
        raise OpcodeAuditError("geth fork aliases: unparsed EVM_GETH_FORK_ALIAS record")
    aliases: dict[str, str] = {}
    canonical = set(hardforks)
    for upstream, local in raw_aliases:
        _require_string_budget(upstream, "geth upstream fork alias")
        _require_string_budget(local, f"geth fork alias {upstream}")
        if upstream in aliases:
            raise OpcodeAuditError(
                f"geth fork aliases: duplicate upstream fork {upstream}"
            )
        if local not in canonical:
            raise OpcodeAuditError(
                f"geth fork aliases: {upstream} maps to unknown canonical "
                f"hardfork {local}"
            )
        aliases[upstream] = local
    if not aliases:
        raise OpcodeAuditError("geth fork aliases: no aliases found")
    return aliases


def parse_eip8024_immediate_policy(text: str) -> EIP8024ImmediatePolicy:
    declaration_count = _require_macro_cardinality(
        text,
        EIP8024_SINGLE_DECLARATION_RE,
        MAX_EIP8024_POLICY_RECORDS,
        "EIP-8024 single immediate policy",
    )
    declaration_count += _require_macro_cardinality(
        text,
        EIP8024_PAIR_DECLARATION_RE,
        MAX_EIP8024_POLICY_RECORDS,
        "EIP-8024 pair immediate policy",
    )
    _require_cardinality(
        declaration_count, MAX_EIP8024_POLICY_RECORDS, "EIP-8024 policy record"
    )
    single: dict[int, tuple[int, ...]] = {}
    pair: dict[int, tuple[int, ...]] = {}
    single_valid = EIP8024_SINGLE_VALID_RE.findall(text)
    single_invalid = EIP8024_SINGLE_INVALID_RE.findall(text)
    pair_valid = EIP8024_PAIR_VALID_RE.findall(text)
    pair_invalid = EIP8024_PAIR_INVALID_RE.findall(text)
    if len(EIP8024_SINGLE_DECLARATION_RE.findall(text)) != (
        len(single_valid) + len(single_invalid)
    ):
        raise OpcodeAuditError("EIP-8024 policy: unparsed single record")
    if len(EIP8024_PAIR_DECLARATION_RE.findall(text)) != (
        len(pair_valid) + len(pair_invalid)
    ):
        raise OpcodeAuditError("EIP-8024 policy: unparsed pair record")

    def insert(
        table: dict[int, tuple[int, ...]],
        family: str,
        encoded_text: str,
        operands: tuple[int, ...],
    ) -> None:
        encoded = int(encoded_text, 0)
        if not 0 <= encoded <= OPCODE_MAX:
            raise OpcodeAuditError(
                f"EIP-8024 policy: {family} byte is outside one byte"
            )
        if encoded in table:
            raise OpcodeAuditError(
                f"EIP-8024 policy: duplicate {family} byte {_format_opcode(encoded)}"
            )
        table[encoded] = operands

    for encoded, depth in single_valid:
        insert(single, "single", encoded, (int(depth),))
    for encoded in single_invalid:
        insert(single, "single", encoded, ())
    for encoded, first, second in pair_valid:
        insert(pair, "pair", encoded, (int(first), int(second)))
    for encoded in pair_invalid:
        insert(pair, "pair", encoded, ())

    expected_bytes = set(range(OPCODE_MAX + 1))
    for family, table in (("single", single), ("pair", pair)):
        if set(table) != expected_bytes:
            missing = sorted(expected_bytes - set(table))
            extra = sorted(set(table) - expected_bytes)
            detail: list[str] = []
            if missing:
                detail.append(
                    "missing " + ", ".join(_format_opcode(value) for value in missing)
                )
            if extra:
                detail.append(
                    "unexpected " + ", ".join(_format_opcode(value) for value in extra)
                )
            raise OpcodeAuditError(
                f"EIP-8024 policy: {family} byte inventory drift: " + "; ".join(detail)
            )
    return EIP8024ImmediatePolicy(single=single, pair=pair)


def parse_neverd_stack_limit(text: str) -> int:
    matches = NEVERD_STACK_LIMIT_RE.findall(text)
    if len(matches) != 1:
        raise OpcodeAuditError(
            "NeverD: expected exactly one literal kStackLimit definition"
        )
    value = int(matches[0].replace("'", ""))
    if value <= 0:
        raise OpcodeAuditError("NeverD: kStackLimit must be greater than zero")
    return value


def _evaluate_opcode_expression(expression: str, iota: int) -> int | None:
    compact = expression.strip()
    if not OPCODE_EXPRESSION_RE.fullmatch(compact):
        return None
    base_text = compact.split("+", maxsplit=1)[0].strip()
    value = int(base_text, 0)
    return value + iota if "iota" in compact else value


def parse_geth_opcodes(text: str) -> dict[str, int]:
    declaration_count = sum(1 for _ in OPCODE_DECLARATION_RE.finditer(text))
    _require_cardinality(
        declaration_count, MAX_OPCODE_RECORDS, "go-ethereum opcode declaration"
    )
    result: dict[str, int] = {}
    for block in CONST_BLOCK_RE.findall(text):
        lines = [
            line
            for raw_line in block.splitlines()
            if (line := raw_line.split("//", maxsplit=1)[0].strip())
        ]
        if not any(re.search(r"\bOpCode\b", line) for line in lines):
            continue

        previous_expression: str | None = None
        for iota, line in enumerate(lines):
            match = CONST_SPEC_RE.fullmatch(line)
            if not match:
                raise OpcodeAuditError(
                    f"go-ethereum: unsupported OpCode declaration {line!r}"
                )
            name, type_name, expression = match.groups()
            _require_string_budget(name, "go-ethereum opcode name")
            if iota == 0 and type_name != "OpCode":
                raise OpcodeAuditError(
                    "go-ethereum: an OpCode const block must begin with an "
                    "explicit OpCode declaration"
                )
            if expression is not None:
                previous_expression = expression
            if previous_expression is None:
                raise OpcodeAuditError(
                    f"go-ethereum: {name} has no inherited OpCode expression"
                )
            value = _evaluate_opcode_expression(previous_expression, iota)
            if value is None:
                raise OpcodeAuditError(
                    f"go-ethereum: unsupported OpCode expression "
                    f"{previous_expression!r} for {name}"
                )
            if not 0 <= value <= OPCODE_MAX:
                raise OpcodeAuditError(
                    f"go-ethereum: {name} is outside the opcode byte"
                )
            _insert_unique(result, name, value, "go-ethereum")
    unparsed = sorted(set(OPCODE_DECLARATION_RE.findall(text)) - set(result))
    if unparsed:
        raise OpcodeAuditError(
            "go-ethereum: unparsed OpCode declarations: " + ", ".join(unparsed)
        )
    if not result:
        raise OpcodeAuditError("go-ethereum: no OpCode constants found")
    return result


def _require_all_macro_records_parsed(
    text: str,
    declaration_pattern: re.Pattern[str],
    parsed_count: int,
    macro: str,
    context: str,
) -> None:
    if sum(1 for _ in declaration_pattern.finditer(text)) != parsed_count:
        raise OpcodeAuditError(f"{context}: unparsed {macro} record")


def _require_macro_cardinality(
    text: str,
    declaration_pattern: re.Pattern[str],
    limit: int,
    context: str,
) -> int:
    count = 0
    for _ in declaration_pattern.finditer(text):
        count += 1
        if count > limit:
            raise OpcodeAuditError(f"{context} record count exceeds {limit}")
    return count


def parse_policy(text: str) -> UpstreamPolicy:
    policy_record_count = sum(
        _require_macro_cardinality(text, pattern, MAX_POLICY_RECORDS, "policy")
        for pattern in (
            POLICY_ALIAS_DECLARATION_RE,
            POLICY_EXCLUSION_KIND_DECLARATION_RE,
            POLICY_IGNORE_DECLARATION_RE,
        )
    )
    _require_cardinality(policy_record_count, MAX_POLICY_RECORDS, "policy record")
    raw_aliases = POLICY_ALIAS_RE.findall(text)
    _require_all_macro_records_parsed(
        text,
        POLICY_ALIAS_DECLARATION_RE,
        len(raw_aliases),
        "EVM_UPSTREAM_OPCODE_ALIAS",
        "policy",
    )
    aliases: dict[str, str] = {}
    for local_name, upstream_name in raw_aliases:
        _require_string_budget(local_name, "policy local opcode alias")
        _require_string_budget(upstream_name, f"policy alias for {local_name}")
        if local_name in aliases:
            raise OpcodeAuditError(f"policy: duplicate alias for {local_name}")
        aliases[local_name] = upstream_name
    raw_exclusion_kinds = POLICY_EXCLUSION_KIND_RE.findall(text)
    _require_all_macro_records_parsed(
        text,
        POLICY_EXCLUSION_KIND_DECLARATION_RE,
        len(raw_exclusion_kinds),
        "EVM_UPSTREAM_OPCODE_EXCLUSION_KIND",
        "policy",
    )
    exclusion_kinds: dict[str, OpcodeExclusionKind] = {}
    for (
        name,
        allow_canonical_byte,
        require_inactive,
    ) in raw_exclusion_kinds:
        _require_string_budget(name, "policy opcode exclusion kind")
        if name in exclusion_kinds:
            raise OpcodeAuditError("policy: duplicate opcode exclusion kind")
        kind = OpcodeExclusionKind(
            allow_canonical_byte=allow_canonical_byte == "true",
            require_inactive=require_inactive == "true",
        )
        if kind.allow_canonical_byte == kind.require_inactive:
            raise OpcodeAuditError(
                f"policy: exclusion kind {name} must select exactly one of "
                "ALLOW_CANONICAL_BYTE and REQUIRE_INACTIVE"
            )
        exclusion_kinds[name] = kind
    raw_ignored = POLICY_IGNORE_RE.findall(text)
    _require_all_macro_records_parsed(
        text,
        POLICY_IGNORE_DECLARATION_RE,
        len(raw_ignored),
        "EVM_UPSTREAM_OPCODE_IGNORE",
        "policy",
    )
    ignored: dict[str, IgnoredOpcode] = {}
    for name, byte_text, reason in raw_ignored:
        _require_string_budget(name, "policy ignored opcode")
        _require_string_budget(reason, f"policy ignored opcode {name} reason")
        if name in ignored:
            raise OpcodeAuditError("policy: duplicate ignored upstream opcode")
        if reason not in exclusion_kinds:
            raise OpcodeAuditError(
                f"policy: ignored opcode {name} has unknown reason {reason}"
            )
        byte = int(byte_text, 0)
        if not 0 <= byte <= OPCODE_MAX:
            raise OpcodeAuditError(
                f"policy: ignored opcode {name} is outside the opcode byte"
            )
        ignored[name] = IgnoredOpcode(byte=byte, reason=reason)
    overlap = set(ignored) & set(aliases.values())
    if overlap:
        raise OpcodeAuditError(
            "policy: aliased upstream opcodes are also ignored: "
            + ", ".join(sorted(overlap))
        )
    return UpstreamPolicy(
        aliases=aliases,
        ignored=ignored,
        exclusion_kinds=exclusion_kinds,
    )


def parse_semantics_policy(text: str) -> UpstreamSemanticsPolicy:
    semantics_record_count = sum(
        _require_macro_cardinality(
            text, pattern, MAX_SEMANTICS_POLICY_RECORDS, "semantics policy"
        )
        for pattern in (
            SEMANTICS_RULE_FIELD_DECLARATION_RE,
            SEMANTICS_FORK_DECLARATION_RE,
            SEMANTICS_BASE_MIN_STACK_DECLARATION_RE,
            SEMANTICS_ACTIVE_WITHOUT_COST_DECLARATION_RE,
            SEMANTICS_DYNAMIC_STACK_DECLARATION_RE,
        )
    )
    _require_cardinality(
        semantics_record_count,
        MAX_SEMANTICS_POLICY_RECORDS,
        "semantics policy record",
    )
    raw_rule_fields = SEMANTICS_RULE_FIELD_RE.findall(text)
    _require_cardinality(len(raw_rule_fields), MAX_RULE_FIELDS, "semantics rule field")
    if len(SEMANTICS_RULE_FIELD_DECLARATION_RE.findall(text)) != len(raw_rule_fields):
        raise OpcodeAuditError("semantics policy: unparsed EVM_GETH_RULE_FIELD record")
    rule_fields = [name for name, _, _ in raw_rule_fields]
    if len(rule_fields) != len(set(rule_fields)):
        raise OpcodeAuditError("semantics policy: duplicate geth rule field")
    rule_classifications: dict[str, GethRuleClassification] = {}
    for name, category, expected_fork in raw_rule_fields:
        for value, context in (
            (name, "semantics rule field"),
            (category, f"semantics rule {name} category"),
            (expected_fork, f"semantics rule {name} expected fork"),
        ):
            _require_string_budget(value, context)
        if category not in GETH_RULE_CATEGORIES:
            raise OpcodeAuditError(
                f"semantics policy: unknown rule category {category} for {name}"
            )
        rule_classifications[name] = GethRuleClassification(
            category=category, expected_fork=expected_fork
        )

    raw_fork_rules = SEMANTICS_FORK_RE.findall(text)
    _require_cardinality(
        len(raw_fork_rules), MAX_HARDFORK_RECORDS, "semantics hardfork rule"
    )
    _require_all_macro_records_parsed(
        text,
        SEMANTICS_FORK_DECLARATION_RE,
        len(raw_fork_rules),
        "EVM_GETH_FORK_RULE",
        "semantics policy",
    )
    fork_rules: list[tuple[str, str | None]] = []
    seen_forks: set[str] = set()
    for fork, rule in raw_fork_rules:
        _require_string_budget(fork, "semantics hardfork")
        _require_string_budget(rule, f"semantics hardfork {fork} rule")
        if fork in seen_forks:
            raise OpcodeAuditError(f"semantics policy: duplicate hardfork {fork}")
        seen_forks.add(fork)
        fork_rules.append((fork, None if rule == NO_GETH_RULE else rule))
    if not fork_rules:
        raise OpcodeAuditError("semantics policy: no hardfork rules found")
    undeclared_rules = sorted(
        {rule for _, rule in fork_rules if rule is not None} - set(rule_fields)
    )
    if undeclared_rules:
        raise OpcodeAuditError(
            "semantics policy uses undeclared geth rule fields: "
            + ", ".join(undeclared_rules)
        )

    raw_base_min_stack = SEMANTICS_BASE_MIN_STACK_RE.findall(text)
    _require_all_macro_records_parsed(
        text,
        SEMANTICS_BASE_MIN_STACK_DECLARATION_RE,
        len(raw_base_min_stack),
        "EVM_GETH_BASE_MIN_STACK",
        "semantics policy",
    )
    base_min_stack: dict[str, int] = {}
    for opcode, minimum_text in raw_base_min_stack:
        _require_string_budget(opcode, "semantics base-minimum opcode")
        if opcode in base_min_stack:
            raise OpcodeAuditError(
                f"semantics policy: duplicate base minimum for {opcode}"
            )
        base_min_stack[opcode] = int(minimum_text)

    raw_active_without_cost = SEMANTICS_ACTIVE_WITHOUT_COST_RE.findall(text)
    _require_all_macro_records_parsed(
        text,
        SEMANTICS_ACTIVE_WITHOUT_COST_DECLARATION_RE,
        len(raw_active_without_cost),
        "EVM_GETH_ACTIVE_WITHOUT_COST",
        "semantics policy",
    )
    active_without_cost: dict[str, str] = {}
    for opcode, fork in raw_active_without_cost:
        _require_string_budget(opcode, "semantics zero-cost opcode")
        _require_string_budget(fork, f"semantics zero-cost opcode {opcode} fork")
        if opcode in active_without_cost:
            raise OpcodeAuditError(
                f"semantics policy: duplicate zero-cost activation for {opcode}"
            )
        active_without_cost[opcode] = fork

    raw_dynamic_specs = SEMANTICS_DYNAMIC_STACK_RE.findall(text)
    _require_cardinality(
        len(raw_dynamic_specs), MAX_EIP8024_SPECS, "semantics dynamic opcode"
    )
    if len(SEMANTICS_DYNAMIC_STACK_DECLARATION_RE.findall(text)) != len(
        raw_dynamic_specs
    ):
        raise OpcodeAuditError(
            "semantics policy: unparsed EVM_GETH_DYNAMIC_STACK_IMMEDIATE record"
        )
    dynamic_stack_immediates: dict[str, DynamicStackImmediateSpec] = {}
    for opcode, family, operation_kind, valid_stack_delta in raw_dynamic_specs:
        _require_string_budget(opcode, "semantics dynamic opcode")
        _require_string_budget(family, f"semantics dynamic opcode {opcode} family")
        _require_string_budget(
            operation_kind, f"semantics dynamic opcode {opcode} operation kind"
        )
        if opcode in dynamic_stack_immediates:
            raise OpcodeAuditError(
                "semantics policy: duplicate dynamic stack immediate opcode"
            )
        normalized_family = EIP8024_OPERAND_FAMILIES.get(family)
        if normalized_family is None:
            raise OpcodeAuditError(
                f"semantics policy: unknown EIP-8024 operand family {family}"
            )
        normalized_operation = EIP8024_OPERATION_KINDS.get(operation_kind)
        if normalized_operation is None:
            raise OpcodeAuditError(
                f"semantics policy: unknown EIP-8024 operation kind {operation_kind}"
            )
        dynamic_stack_immediates[opcode] = DynamicStackImmediateSpec(
            family=normalized_family,
            operation_kind=normalized_operation,
            valid_stack_delta=int(valid_stack_delta),
        )

    return UpstreamSemanticsPolicy(
        rule_fields=tuple(rule_fields),
        rule_classifications=rule_classifications,
        fork_rules=tuple(fork_rules),
        base_min_stack=base_min_stack,
        active_without_cost=active_without_cost,
        dynamic_stack_immediates=dynamic_stack_immediates,
    )


def build_geth_probe_request(
    opcodes: Mapping[str, NeverDOpcodeMetadata],
    hardforks: Sequence[str],
    policy: UpstreamSemanticsPolicy,
) -> dict[str, object]:
    _require_cardinality(len(opcodes), MAX_OPCODE_RECORDS, "probe opcode")
    _require_cardinality(len(hardforks), MAX_HARDFORK_RECORDS, "probe hardfork")
    _require_cardinality(len(policy.rule_fields), MAX_RULE_FIELDS, "probe rule field")
    _require_cardinality(
        len(policy.rule_classifications), MAX_RULE_PROBES, "probe rule"
    )
    _require_cardinality(
        len(policy.dynamic_stack_immediates),
        MAX_EIP8024_SPECS,
        "probe EIP-8024 spec",
    )
    canonical_forks = tuple(hardforks)
    policy_forks = tuple(fork for fork, _ in policy.fork_rules)
    if policy_forks != canonical_forks:
        raise OpcodeAuditError(
            "semantics policy hardfork order does not match EVMHardforks.def"
        )

    known_forks = set(canonical_forks)
    mapped_rules = {rule: fork for fork, rule in policy.fork_rules if rule is not None}
    for name, classification in policy.rule_classifications.items():
        if classification.expected_fork not in known_forks:
            raise OpcodeAuditError(
                f"semantics policy rule {name} expects unknown hardfork "
                f"{classification.expected_fork}"
            )
        mapped_fork = mapped_rules.get(name)
        if classification.category == MAPPED_FORK_SELECTOR:
            if mapped_fork != classification.expected_fork:
                raise OpcodeAuditError(
                    f"semantics policy mapped selector {name} expects "
                    f"{classification.expected_fork}, fork rules map it to "
                    f"{mapped_fork or NO_GETH_RULE}"
                )
        elif mapped_fork is not None:
            raise OpcodeAuditError(
                f"semantics policy excluded rule {name} is also mapped to {mapped_fork}"
            )
    unclassified_mapped = sorted(set(mapped_rules) - set(policy.rule_classifications))
    if unclassified_mapped:
        raise OpcodeAuditError(
            "semantics policy mapped selectors lack a rule classification: "
            + ", ".join(unclassified_mapped)
        )

    known_opcodes = set(opcodes)
    unknown_overrides = sorted(set(policy.base_min_stack) - known_opcodes)
    if unknown_overrides:
        raise OpcodeAuditError(
            "semantics policy overrides unknown opcodes: "
            + ", ".join(unknown_overrides)
        )
    unknown_zero_cost = sorted(set(policy.active_without_cost) - known_opcodes)
    if unknown_zero_cost:
        raise OpcodeAuditError(
            "semantics policy activates unknown zero-cost opcodes: "
            + ", ".join(unknown_zero_cost)
        )
    unknown_dynamic = sorted(set(policy.dynamic_stack_immediates) - known_opcodes)
    if unknown_dynamic:
        raise OpcodeAuditError(
            "semantics policy names unknown dynamic stack opcodes: "
            + ", ".join(unknown_dynamic)
        )

    fork_indices = {fork: index for index, fork in enumerate(canonical_forks)}
    for opcode in opcodes.values():
        if opcode.introduced not in fork_indices:
            raise OpcodeAuditError(
                f"NeverD opcode {opcode.name} names unknown hardfork "
                f"{opcode.introduced}"
            )

    enabled_rules: list[str] = []
    fork_requests: list[dict[str, object]] = []
    for fork, rule in policy.fork_rules:
        if rule is not None:
            if rule in enabled_rules:
                raise OpcodeAuditError(
                    f"semantics policy enables geth rule {rule} more than once"
                )
            enabled_rules.append(rule)
        fork_requests.append({"name": fork, "rules": list(enabled_rules)})

    opcode_requests: list[dict[str, object]] = []
    for opcode in sorted(opcodes.values(), key=lambda item: item.byte):
        request: dict[str, object] = {"name": opcode.name, "byte": opcode.byte}
        if opcode.name in policy.active_without_cost:
            activation = policy.active_without_cost[opcode.name]
            if activation not in fork_indices:
                raise OpcodeAuditError(
                    f"semantics policy gives {opcode.name} unknown hardfork "
                    f"{activation}"
                )
            if activation != opcode.introduced:
                raise OpcodeAuditError(
                    f"semantics policy zero-cost activation for {opcode.name} "
                    f"is {activation}, NeverD says {opcode.introduced}"
                )
            request["active_without_cost_from"] = fork_indices[activation]
        opcode_requests.append(request)

    eip8024_specs = []
    for name, spec in sorted(
        policy.dynamic_stack_immediates.items(),
        key=lambda item: opcodes[item[0]].byte,
    ):
        eip8024_specs.append(
            {
                "name": name,
                "byte": opcodes[name].byte,
                "family": spec.family,
                "operation_kind": spec.operation_kind,
                "valid_stack_delta": spec.valid_stack_delta,
            }
        )

    return {
        "rule_fields": sorted(policy.rule_fields),
        "rule_probes": [
            {
                "name": name,
                "category": classification.category,
                "expected_fork": classification.expected_fork,
            }
            for name, classification in sorted(policy.rule_classifications.items())
        ],
        "forks": fork_requests,
        "opcodes": opcode_requests,
        "eip8024_specs": eip8024_specs,
    }


def audit_geth_rule_probes(
    policy: UpstreamSemanticsPolicy,
    fork_opcode_records: Mapping[str, object],
    raw_rule_probes: object,
) -> None:
    if not isinstance(raw_rule_probes, list):
        raise OpcodeAuditError("geth opcode probe has no rule probe list")
    expected = sorted(policy.rule_classifications.items())
    if len(raw_rule_probes) != len(expected):
        raise OpcodeAuditError("geth rule probe inventory count drift")
    for raw, (name, classification) in zip(raw_rule_probes, expected):
        if not isinstance(raw, dict):
            raise OpcodeAuditError("geth rule probe contains a non-object record")
        context = f"geth rule probe {name}"
        _require_exact_fields(raw, GETH_MANIFEST_RULE_PROBE_FIELDS, context)
        actual_name = _require_string_field(raw, "name", context)
        category = _require_string_field(raw, "category", context)
        expected_fork = _require_string_field(raw, "expected_fork", context)
        lookup_error = _require_boolean_field(raw, "lookup_error", context)
        if actual_name != name:
            raise OpcodeAuditError("geth rule probe order or name drift")
        if category != classification.category or (
            expected_fork != classification.expected_fork
        ):
            raise OpcodeAuditError(f"geth rule probe {name} policy echo drift")
        wants_error = category == EXCLUDED_SELECTOR_EXPECTED_ERROR
        if lookup_error != wants_error:
            raise OpcodeAuditError(
                f"geth rule probe {name} lookup error contract drift"
            )
        expected_opcodes = fork_opcode_records.get(expected_fork)
        if expected_opcodes is None:
            raise OpcodeAuditError(
                f"geth rule probe {name} expects unavailable fork {expected_fork}"
            )
        if raw.get("opcodes") != expected_opcodes:
            raise OpcodeAuditError(
                f"geth rule probe {name} instruction table fingerprint drift"
            )


def audit_geth_mainnet_forks(
    raw_mainnet: object,
    *,
    latest_hardfork: str,
    geth_fork_aliases: Mapping[str, str],
    fork_opcode_records: Mapping[str, object],
    rule_fields: Sequence[str],
) -> None:
    if not isinstance(raw_mainnet, dict):
        raise OpcodeAuditError("geth opcode probe has no mainnet fork record")
    _require_exact_fields(
        raw_mainnet, GETH_MANIFEST_MAINNET_FIELDS, "geth mainnet fork record"
    )
    known_rules = set(rule_fields)
    for role in ("active", "scheduled"):
        raw = raw_mainnet.get(role)
        context = f"geth mainnet {role} fork"
        if not isinstance(raw, dict):
            raise OpcodeAuditError(f"{context} is not an object")
        _require_exact_fields(raw, GETH_MANIFEST_MAINNET_FORK_FIELDS, context)
        upstream_fork = _require_string_field(raw, "upstream_fork", context)
        rules = _require_string_array_field(raw, "rules", context)
        if len(rules) != len(set(rules)):
            raise OpcodeAuditError(f"{context} repeats an enabled rule")
        unknown_rules = sorted(set(rules) - known_rules)
        if unknown_rules:
            raise OpcodeAuditError(
                f"{context} names unknown rules: " + ", ".join(unknown_rules)
            )
        local_fork = geth_fork_aliases.get(upstream_fork)
        if local_fork is None:
            raise OpcodeAuditError(
                f"{context} reports unknown upstream fork {upstream_fork}"
            )
        if role == "active" and local_fork != latest_hardfork:
            raise OpcodeAuditError(
                "geth active mainnet fork maps to "
                f"{local_fork}, NeverD latest marker is {latest_hardfork}"
            )
        expected_opcodes = fork_opcode_records.get(local_fork)
        if expected_opcodes is None:
            raise OpcodeAuditError(
                f"{context} maps to unprobed canonical hardfork {local_fork}"
            )
        if raw.get("opcodes") != expected_opcodes:
            raise OpcodeAuditError(
                f"{context} instruction table fingerprint drift from {local_fork}"
            )


def _opcode_record_names(raw_records: object, context: str) -> set[str]:
    if not isinstance(raw_records, list):
        raise OpcodeAuditError(f"{context} opcode fingerprint is not an array")
    _require_cardinality(
        len(raw_records), MAX_OPCODE_RECORDS, f"{context} opcode fingerprint"
    )
    result: set[str] = set()
    for raw in raw_records:
        if not isinstance(raw, dict):
            raise OpcodeAuditError(f"{context} opcode fingerprint has a non-object")
        name = _require_string_field(raw, "name", context)
        if name in result:
            raise OpcodeAuditError(f"{context} repeats opcode {name}")
        result.add(name)
    return result


def audit_geth_eip8024_immediates(
    policy: EIP8024ImmediatePolicy,
    semantics_policy: UpstreamSemanticsPolicy,
    opcodes: Mapping[str, NeverDOpcodeMetadata],
    hardforks: Sequence[str],
    fork_opcode_records: Mapping[str, object],
    raw_mainnet: object,
    raw_eip8024: object,
) -> EIP8024AuditResult:
    if not isinstance(raw_eip8024, dict):
        raise OpcodeAuditError("geth opcode probe has no EIP-8024 record")
    _require_exact_fields(
        raw_eip8024, GETH_MANIFEST_EIP8024_FIELDS, "geth EIP-8024 record"
    )
    unknown_dynamic = sorted(
        set(semantics_policy.dynamic_stack_immediates) - set(opcodes)
    )
    if unknown_dynamic:
        raise OpcodeAuditError(
            "EIP-8024 policy names unknown opcodes: " + ", ".join(unknown_dynamic)
        )
    dynamic_specs = []
    for name, spec in sorted(
        semantics_policy.dynamic_stack_immediates.items(),
        key=lambda item: opcodes[item[0]].byte,
    ):
        if spec.family not in {"single", "pair"}:
            raise OpcodeAuditError(
                f"EIP-8024 opcode {name} has unknown immediate family {spec.family}"
            )
        dynamic_specs.append((name, spec))
    dynamic_names = [name for name, _ in dynamic_specs]
    dynamic_name_set = set(dynamic_names)

    expected_targets: list[tuple[str, object]] = []
    for fork in hardforks:
        if fork not in fork_opcode_records:
            raise OpcodeAuditError(f"EIP-8024 audit lacks canonical target {fork}")
        expected_targets.append((fork, fork_opcode_records[fork]))
    if not isinstance(raw_mainnet, dict):
        raise OpcodeAuditError("geth opcode probe has no mainnet fork record")
    for role, target in (
        ("active", MAINNET_ACTIVE_EIP8024_TARGET),
        ("scheduled", MAINNET_SCHEDULED_EIP8024_TARGET),
    ):
        raw_fork = raw_mainnet.get(role)
        if not isinstance(raw_fork, dict):
            raise OpcodeAuditError(f"EIP-8024 audit lacks {target} target")
        expected_targets.append((target, raw_fork.get("opcodes")))

    tables = raw_eip8024.get("tables")
    if not isinstance(tables, list) or len(tables) != len(expected_targets):
        raise OpcodeAuditError(
            "geth EIP-8024 table inventory must contain every canonical and "
            "mainnet target"
        )

    baseline_handlers: dict[str, str] | None = None
    active_targets: list[str] = []
    observation_count = 0
    missing_operand_count = 0
    for raw_table, (expected_target, raw_opcodes) in zip(tables, expected_targets):
        context = f"geth EIP-8024 table {expected_target}"
        if not isinstance(raw_table, dict):
            raise OpcodeAuditError(f"{context} is not an object")
        _require_exact_fields(raw_table, GETH_MANIFEST_EIP8024_TABLE_FIELDS, context)
        if _require_string_field(raw_table, "target", context) != expected_target:
            raise OpcodeAuditError("geth EIP-8024 table target order drift")

        fingerprint_names = _opcode_record_names(raw_opcodes, context)
        activated = fingerprint_names & dynamic_name_set
        if activated and activated != dynamic_name_set:
            raise OpcodeAuditError(
                f"{context} partially activates the dynamic immediate opcodes"
            )
        expected_active = dynamic_names if activated else []
        active = _require_string_array_field(
            raw_table,
            "active_opcodes",
            context,
            limit=MAX_EIP8024_SPECS,
        )
        if active != expected_active:
            raise OpcodeAuditError(f"{context} active opcode inventory drift")

        handlers = raw_table.get("handlers")
        observations = raw_table.get("observations")
        missing = raw_table.get("missing_operand")
        if not expected_active:
            if handlers != [] or observations != [] or missing != []:
                raise OpcodeAuditError(
                    f"{context} inactive table contains behavioral observations"
                )
            continue

        if not isinstance(handlers, list) or len(handlers) != len(dynamic_specs):
            raise OpcodeAuditError(f"{context} handler inventory drift")
        actual_handlers: dict[str, str] = {}
        for raw_handler, (name, _) in zip(handlers, dynamic_specs):
            handler_context = f"{context} handler {name}"
            if not isinstance(raw_handler, dict):
                raise OpcodeAuditError(f"{handler_context} is not an object")
            _require_exact_fields(
                raw_handler, GETH_MANIFEST_EIP8024_HANDLER_FIELDS, handler_context
            )
            actual_name = _require_string_field(raw_handler, "opcode", handler_context)
            symbol = _require_string_field(raw_handler, "symbol", handler_context)
            if actual_name != name or not symbol:
                raise OpcodeAuditError(f"{handler_context} identity drift")
            actual_handlers[name] = symbol
        if baseline_handlers is None:
            baseline_handlers = actual_handlers
        elif actual_handlers != baseline_handlers:
            raise OpcodeAuditError(f"{context} post-activation handler drift")

        active_targets.append(expected_target)

        expected_count = len(dynamic_specs) * (OPCODE_MAX + 1)
        if not isinstance(observations, list) or len(observations) != expected_count:
            raise OpcodeAuditError(
                f"{context} observation count must be {expected_count}"
            )
        observation_index = 0
        for opcode, spec in dynamic_specs:
            family = getattr(policy, spec.family)
            for encoded in range(OPCODE_MAX + 1):
                raw = observations[observation_index]
                observation_index += 1
                observation_context = f"{context} {opcode} {_format_opcode(encoded)}"
                if not isinstance(raw, dict):
                    raise OpcodeAuditError(
                        f"{observation_context} observation is not an object"
                    )
                _require_exact_fields(
                    raw,
                    GETH_MANIFEST_EIP8024_OBSERVATION_FIELDS,
                    observation_context,
                )
                expected_operands = list(family[encoded])
                valid = bool(expected_operands)
                expected = (
                    opcode,
                    encoded,
                    valid,
                    expected_operands,
                    EIP8024_ACCEPTED_PC_DELTA if valid else EIP8024_REJECTED_PC_DELTA,
                    EIP8024_ERROR_NONE if valid else EIP8024_ERROR_INVALID_OPCODE,
                    spec.valid_stack_delta if valid else 0,
                    True,
                    EIP8024_ERROR_STACK_UNDERFLOW if valid else EIP8024_ERROR_NOT_RUN,
                    EIP8024_REJECTED_PC_DELTA,
                    valid,
                )
                actual = (
                    _require_string_field(raw, "opcode", observation_context),
                    _require_integer_field(raw, "encoded", observation_context),
                    _require_boolean_field(raw, "accepted", observation_context),
                    _require_integer_array_field(raw, "operands", observation_context),
                    _require_integer_field(raw, "pc_delta", observation_context),
                    _require_string_field(raw, "error_class", observation_context),
                    _require_integer_field(raw, "stack_delta", observation_context),
                    _require_boolean_field(
                        raw, "marker_transition_verified", observation_context
                    ),
                    _require_string_field(
                        raw, "underflow_error_class", observation_context
                    ),
                    _require_integer_field(
                        raw, "underflow_pc_delta", observation_context
                    ),
                    _require_boolean_field(
                        raw, "underflow_stack_unchanged", observation_context
                    ),
                )
                if actual != expected:
                    raise OpcodeAuditError(
                        f"{observation_context} execution drift: "
                        f"NeverD={expected!r}, go-ethereum={actual!r}"
                    )

        if not isinstance(missing, list) or len(missing) != len(dynamic_specs):
            raise OpcodeAuditError(f"{context} missing-operand inventory drift")
        for raw, (opcode, _) in zip(missing, dynamic_specs):
            missing_context = f"{context} {opcode} missing operand"
            if not isinstance(raw, dict):
                raise OpcodeAuditError(f"{missing_context} record is not an object")
            _require_exact_fields(
                raw, GETH_MANIFEST_EIP8024_MISSING_FIELDS, missing_context
            )
            if _require_string_field(raw, "opcode", missing_context) != opcode or not (
                _require_boolean_field(raw, "matches_zero_immediate", missing_context)
                and _require_boolean_field(
                    raw, "marker_transition_verified", missing_context
                )
            ):
                raise OpcodeAuditError(
                    f"{missing_context} does not match explicit 0x00 behavior"
                )

        observation_count += len(observations)
        missing_operand_count += len(missing)

    return EIP8024AuditResult(
        table_count=len(tables),
        active_targets=tuple(active_targets),
        observation_count=observation_count,
        missing_operand_count=missing_operand_count,
    )


def audit_geth_opcode_semantics(
    opcodes: Mapping[str, NeverDOpcodeMetadata],
    hardforks: Sequence[str],
    policy: UpstreamSemanticsPolicy,
    upstream: Mapping[str, object],
    *,
    expected_revision: str,
    expected_go_version: str,
    expected_stack_limit: int,
) -> SemanticsAuditResult:
    _require_required_known_fields(
        upstream,
        frozenset(
            {
                "schema_version",
                "geth_revision",
                "go_version",
                "stack_limit",
                "forks",
            }
        ),
        GETH_MANIFEST_ROOT_FIELDS,
        "geth opcode probe manifest",
    )
    schema_version = _require_integer_field(
        upstream, "schema_version", "geth opcode probe manifest"
    )
    if schema_version != GETH_PROBE_SCHEMA_VERSION:
        raise OpcodeAuditError(
            "geth opcode probe returned an unsupported schema version"
        )
    geth_revision = _require_string_field(
        upstream, "geth_revision", "geth opcode probe manifest"
    )
    go_version = _require_string_field(
        upstream, "go_version", "geth opcode probe manifest"
    )
    stack_limit = _require_integer_field(
        upstream, "stack_limit", "geth opcode probe manifest"
    )
    if geth_revision != expected_revision:
        raise OpcodeAuditError("geth opcode probe revision does not match fetched SHA")
    if go_version != expected_go_version:
        raise OpcodeAuditError("geth opcode probe Go version does not match toolchain")
    if stack_limit != expected_stack_limit:
        raise OpcodeAuditError("geth opcode probe stack limit drift")

    raw_forks = upstream.get("forks")
    if not isinstance(raw_forks, list):
        raise OpcodeAuditError("geth opcode probe has no hardfork manifest list")
    _require_cardinality(len(raw_forks), MAX_HARDFORK_RECORDS, "geth hardfork manifest")
    seen_forks: set[str] = set()
    for raw_fork in raw_forks:
        if not isinstance(raw_fork, dict):
            raise OpcodeAuditError("geth opcode probe contains a non-object hardfork")
        name = raw_fork.get("name")
        if not isinstance(name, str):
            raise OpcodeAuditError("geth opcode probe hardfork has no string name")
        if name in seen_forks:
            raise OpcodeAuditError(
                f"geth opcode probe contains duplicate hardfork {name}"
            )
        seen_forks.add(name)
    if len(raw_forks) != len(hardforks):
        raise OpcodeAuditError("geth opcode probe hardfork manifest count drift")

    missing_dynamic_overrides = sorted(
        set(policy.dynamic_stack_immediates) - set(policy.base_min_stack)
    )
    if missing_dynamic_overrides:
        raise OpcodeAuditError(
            "dynamic stack opcodes need explicit base minimum overrides: "
            + ", ".join(missing_dynamic_overrides)
        )

    fork_indices = {name: index for index, name in enumerate(hardforks)}
    problems: list[str] = []
    for fork_index, (fork_name, raw_fork) in enumerate(zip(hardforks, raw_forks)):
        assert isinstance(raw_fork, dict)
        _require_exact_fields(
            raw_fork,
            GETH_MANIFEST_FORK_FIELDS,
            f"geth opcode probe hardfork {fork_name}",
        )
        if raw_fork.get("name") != fork_name:
            raise OpcodeAuditError("geth opcode probe hardfork order drift")
        rules = _require_string_array_field(
            raw_fork, "rules", f"geth opcode probe hardfork {fork_name}"
        )
        expected_rules = [
            rule for _, rule in policy.fork_rules[: fork_index + 1] if rule is not None
        ]
        if rules != expected_rules:
            raise OpcodeAuditError(f"{fork_name} geth Rules manifest drift")
        raw_records = raw_fork.get("opcodes")
        if not isinstance(raw_records, list):
            raise OpcodeAuditError(
                f"geth opcode probe hardfork {fork_name} has no opcode list"
            )
        _require_cardinality(
            len(raw_records),
            MAX_OPCODE_RECORDS,
            f"geth opcode probe hardfork {fork_name} opcode",
        )
        records: dict[str, Mapping[str, object]] = {}
        seen_record_names: set[str] = set()
        seen_record_bytes: set[int] = set()
        for raw_record in raw_records:
            if not isinstance(raw_record, dict):
                raise OpcodeAuditError(
                    f"geth opcode probe hardfork {fork_name} has a non-object record"
                )
            _require_exact_fields(
                raw_record,
                GETH_MANIFEST_OPCODE_FIELDS,
                f"geth opcode probe hardfork {fork_name} opcode record",
            )
            record_context = f"geth opcode probe hardfork {fork_name} opcode record"
            byte = _require_integer_field(raw_record, "byte", record_context)
            _require_integer_field(raw_record, "base_min_stack", record_context)
            _require_integer_field(raw_record, "net_stack_delta", record_context)
            name = _require_string_field(raw_record, "name", record_context)
            if name in seen_record_names:
                raise OpcodeAuditError(
                    f"geth opcode probe hardfork {fork_name} contains duplicate "
                    f"opcode record {name}"
                )
            seen_record_names.add(name)
            if byte in seen_record_bytes:
                raise OpcodeAuditError(
                    f"geth opcode probe hardfork {fork_name} contains "
                    f"duplicate opcode byte {_format_opcode(byte)}"
                )
            seen_record_bytes.add(byte)
            records[name] = raw_record

        expected_names = {
            name
            for name, opcode in opcodes.items()
            if fork_indices[opcode.introduced] <= fork_index
        }
        if set(records) != expected_names:
            missing = sorted(expected_names - set(records))
            extra = sorted(set(records) - expected_names)
            detail = []
            if missing:
                detail.append("missing " + ", ".join(missing))
            if extra:
                detail.append("unexpected " + ", ".join(extra))
            problems.append(f"{fork_name} activation drift: " + "; ".join(detail))
            continue

        for name in sorted(expected_names, key=lambda item: opcodes[item].byte):
            opcode = opcodes[name]
            record = records[name]
            expected = {
                "byte": opcode.byte,
                "base_min_stack": policy.base_min_stack.get(name, opcode.stack_pops),
                "net_stack_delta": opcode.stack_pushes - opcode.stack_pops,
            }
            for field, wanted in expected.items():
                actual = record.get(field)
                if actual != wanted:
                    problems.append(
                        f"{fork_name} {name} {field}: NeverD={wanted!r}, "
                        f"go-ethereum={actual!r}"
                    )
    if problems:
        raise OpcodeAuditError("; ".join(problems))

    return SemanticsAuditResult(
        opcode_count=len(opcodes),
        base_min_stack_override_count=len(policy.base_min_stack),
        dynamic_stack_immediate_count=len(policy.dynamic_stack_immediates),
    )


def audit_opcodes(
    neverd: Mapping[str, int],
    upstream: Mapping[str, int],
    policy: UpstreamPolicy,
) -> AuditResult:
    unknown_aliases = sorted(set(policy.aliases) - set(neverd))
    if unknown_aliases:
        raise OpcodeAuditError(
            "policy aliases unknown NeverD opcodes: " + ", ".join(unknown_aliases)
        )

    consumed: set[str] = set()
    problems: list[str] = []
    for local_name, local_value in sorted(neverd.items(), key=lambda item: item[1]):
        upstream_name = policy.aliases.get(local_name, local_name)
        upstream_value = upstream.get(upstream_name)
        if upstream_value is None:
            problems.append(
                f"NeverD {local_name}={_format_opcode(local_value)} "
                "is absent upstream "
                f"as {upstream_name}"
            )
            continue
        consumed.add(upstream_name)
        if upstream_value != local_value:
            problems.append(
                f"{local_name}: NeverD={_format_opcode(local_value)}, "
                f"go-ethereum {upstream_name}="
                f"{_format_opcode(upstream_value)}"
            )

    stale_ignores = sorted(set(policy.ignored) - set(upstream))
    if stale_ignores:
        problems.append("stale ignored upstream names: " + ", ".join(stale_ignores))

    canonical_bytes = set(neverd.values())
    for name, ignored in sorted(policy.ignored.items()):
        upstream_value = upstream.get(name)
        if upstream_value is not None and upstream_value != ignored.byte:
            problems.append(
                f"ignored {name}: policy={_format_opcode(ignored.byte)}, "
                f"go-ethereum={_format_opcode(upstream_value)}"
            )
        kind = policy.exclusion_kinds[ignored.reason]
        shares_canonical_byte = ignored.byte in canonical_bytes
        if shares_canonical_byte and not kind.allow_canonical_byte:
            problems.append(
                f"ignored {name}={_format_opcode(ignored.byte)} overlaps a "
                f"canonical NeverD opcode, but {ignored.reason} forbids it"
            )
        if not shares_canonical_byte and not kind.require_inactive:
            problems.append(
                f"ignored {name}={_format_opcode(ignored.byte)} has no "
                f"canonical NeverD opcode, but {ignored.reason} does not "
                "require an inactive upstream slot"
            )

    unreviewed = sorted(set(upstream) - consumed - set(policy.ignored))
    if unreviewed:
        problems.append(
            "unreviewed go-ethereum opcode constants: " + ", ".join(unreviewed)
        )
    if problems:
        raise OpcodeAuditError("; ".join(problems))

    return AuditResult(
        neverd_count=len(neverd),
        upstream_count=len(upstream),
        ignored_count=len(policy.ignored),
    )


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--manifest-output",
        type=Path,
        help="atomically write the audited upstream manifest and diagnostics",
    )
    return parser.parse_args(argv)


def main(
    argv: Sequence[str] | None = None,
    *,
    input_paths: AuditInputPaths | None = None,
) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    if input_paths is None:
        input_paths = AuditInputPaths()
    audit_unix_time = int(time.time())
    manifest: dict[str, object] = {
        "schema_version": GETH_PROBE_SCHEMA_VERSION,
        "authority": GETH_AUDIT_AUTHORITY,
        "geth_remote": DEFAULT_GETH_REMOTE,
        "geth_ref": DEFAULT_GETH_REF,
        "geth_revision": None,
        "audit_unix_time": audit_unix_time,
        "go_version": None,
        "stack_limit": None,
        "forks": [],
        "rule_probes": [],
        "mainnet": {},
        "eip8024": {},
        "diagnostics": [],
    }
    authority_temporary = None
    try:
        authority_temporary = tempfile.TemporaryDirectory(
            prefix="neverd-geth-authority-"
        )
        authority_repository = Path(authority_temporary.name) / "go-ethereum.git"
        source = fetch_geth_opcode_source(
            remote=DEFAULT_GETH_REMOTE,
            ref=DEFAULT_GETH_REF,
            cache=authority_repository,
            git_executable=DEFAULT_GIT_EXECUTABLE,
        )
        source_description = f"revision {source.revision}"
        print(f"Fetched go-ethereum revision {source.revision}")
        manifest["geth_revision"] = source.revision
        geth_checkout = checkout_geth_revision(
            cache=authority_repository,
            revision=source.revision,
            authority_ref=source.authority_ref,
            git_executable=DEFAULT_GIT_EXECUTABLE,
        )
        with geth_checkout as geth_root:
            input_budget = _FixedInputBudget()
            neverd_metadata = parse_neverd_opcode_metadata(
                _read_bounded_utf8(
                    input_paths.neverd_opcodes,
                    "NeverD opcode metadata",
                    budget=input_budget,
                )
            )
            neverd = {name: metadata.byte for name, metadata in neverd_metadata.items()}
            hardfork_text = _read_bounded_utf8(
                input_paths.neverd_hardforks,
                "NeverD hardfork metadata",
                budget=input_budget,
            )
            hardforks = parse_neverd_hardforks(hardfork_text)
            latest_hardfork = parse_neverd_latest_hardfork(hardfork_text, hardforks)
            geth_fork_aliases = parse_geth_fork_aliases(
                _read_bounded_utf8(
                    input_paths.geth_fork_aliases,
                    "geth fork alias policy",
                    budget=input_budget,
                ),
                hardforks,
            )
            eip8024_policy = parse_eip8024_immediate_policy(
                _read_bounded_utf8(
                    input_paths.eip8024_policy,
                    "EIP-8024 immediate policy",
                    budget=input_budget,
                )
            )
            policy = parse_policy(
                _read_bounded_utf8(
                    input_paths.opcode_policy,
                    "upstream opcode policy",
                    budget=input_budget,
                )
            )
            semantics_policy = parse_semantics_policy(
                _read_bounded_utf8(
                    input_paths.semantics_policy,
                    "upstream semantics policy",
                    budget=input_budget,
                )
            )
            stack_limit = parse_neverd_stack_limit(
                _read_bounded_utf8(
                    input_paths.neverd_constants,
                    "NeverD EVM constants",
                    budget=input_budget,
                )
            )
            manifest["stack_limit"] = stack_limit
            probe_request = build_geth_probe_request(
                neverd_metadata, hardforks, semantics_policy
            )

            upstream = parse_geth_opcodes(source.text)
            result = audit_opcodes(neverd, upstream, policy)
            probe_result = run_geth_opcode_probe(
                geth_root=geth_root,
                geth_revision=source.revision,
                request=probe_request,
                helper=DEFAULT_GETH_PROBE,
                eip8024_overlay=DEFAULT_GETH_EIP8024_OVERLAY,
                go_executable=DEFAULT_GO_EXECUTABLE,
                go_toolchain=DEFAULT_GO_TOOLCHAIN,
                audit_unix_time=audit_unix_time,
            )
        manifest = {**probe_result, "diagnostics": []}
        _require_exact_fields(
            manifest, AUDIT_MANIFEST_FIELDS, "EVM upstream audit manifest"
        )
        go_version = _require_string_field(
            probe_result, "go_version", "geth opcode probe manifest"
        )
        semantics_result = audit_geth_opcode_semantics(
            neverd_metadata,
            hardforks,
            semantics_policy,
            probe_result,
            expected_revision=source.revision,
            expected_go_version=go_version,
            expected_stack_limit=stack_limit,
        )
        fork_opcode_records = {
            raw_fork["name"]: raw_fork["opcodes"] for raw_fork in probe_result["forks"]
        }
        audit_geth_rule_probes(
            semantics_policy,
            fork_opcode_records,
            probe_result["rule_probes"],
        )
        audit_geth_mainnet_forks(
            probe_result["mainnet"],
            latest_hardfork=latest_hardfork,
            geth_fork_aliases=geth_fork_aliases,
            fork_opcode_records=fork_opcode_records,
            rule_fields=semantics_policy.rule_fields,
        )
        eip8024_result = audit_geth_eip8024_immediates(
            eip8024_policy,
            semantics_policy,
            neverd_metadata,
            hardforks,
            fork_opcode_records,
            probe_result["mainnet"],
            probe_result["eip8024"],
        )
    except (OSError, OpcodeAuditError) as error:
        manifest["diagnostics"] = [str(error)]
        if args.manifest_output is not None:
            try:
                _atomic_write_json(args.manifest_output, manifest)
            except OSError as manifest_error:
                print(
                    f"Could not write EVM opcode audit manifest: {manifest_error}",
                    file=sys.stderr,
                )
        print(f"EVM opcode audit failed: {error}", file=sys.stderr)
        return 1
    finally:
        if authority_temporary is not None:
            authority_temporary.cleanup()

    if args.manifest_output is not None:
        try:
            _atomic_write_json(args.manifest_output, manifest)
        except OSError as error:
            print(
                f"EVM opcode audit failed to write manifest: {error}",
                file=sys.stderr,
            )
            return 1

    active_target_summary = ", ".join(eip8024_result.active_targets)
    print(
        f"EVM opcode metadata and exported instruction sets match "
        f"go-ethereum {source_description}: "
        f"{result.neverd_count} NeverD records, "
        f"{result.upstream_count} upstream constants, "
        f"{result.ignored_count} explicit exclusions; "
        f"{semantics_result.opcode_count} opcodes match activation, "
        "base_min_stack, and net_stack_delta; "
        f"{semantics_result.base_min_stack_override_count} explicit "
        "base-minimum overrides; "
        f"{semantics_result.dynamic_stack_immediate_count} "
        "dynamic-immediate opcodes; "
        f"{eip8024_result.table_count} EIP-8024 table targets audited, "
        f"{len(eip8024_result.active_targets)} active "
        f"({active_target_summary}) with "
        f"{eip8024_result.observation_count} candidate executions and "
        f"{eip8024_result.missing_operand_count} missing-operand cases matched"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
