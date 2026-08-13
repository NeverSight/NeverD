#!/usr/bin/env python3
"""Audit NeverD's EVM opcode database against current go-ethereum sources."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping, Sequence


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_NEVERD_OPCODES = REPO_ROOT / "include/neverd/evm/bytecode/EVMOpcodes.def"
DEFAULT_POLICY = (
    REPO_ROOT / "include/neverd/evm/bytecode/EVMUpstreamOpcodePolicy.def"
)
DEFAULT_GETH_REMOTE = "https://github.com/ethereum/go-ethereum.git"
DEFAULT_GETH_REF = "HEAD"
DEFAULT_GETH_CACHE = REPO_ROOT / "build/evm-opcode-audit/go-ethereum.git"
DEFAULT_GIT_EXECUTABLE = "git"
GETH_OPCODE_PATH = Path("core/vm/opcodes.go")
GETH_CACHE_REF = "refs/neverd/go-ethereum"
GIT_FETCH_TIMEOUT_SECONDS = 300
OPCODE_BITS = 8
OPCODE_MAX = (1 << OPCODE_BITS) - 1
OPCODE_HEX_DIGITS = OPCODE_BITS // 4

NEVERD_OPCODE_RE = re.compile(
    r"^\s*EVM_OPCODE\(\s*([A-Z][A-Z0-9_]*)\s*,\s*(0x[0-9a-fA-F]+)\s*,",
    re.MULTILINE,
)
POLICY_ALIAS_RE = re.compile(
    r"^\s*EVM_UPSTREAM_OPCODE_ALIAS\(\s*([A-Z][A-Z0-9_]*)\s*,\s*"
    r"([A-Z][A-Z0-9_]*)\s*\)",
    re.MULTILINE,
)
POLICY_IGNORE_RE = re.compile(
    r"^\s*EVM_UPSTREAM_OPCODE_IGNORE\(\s*([A-Z][A-Z0-9_]*)\s*,",
    re.MULTILINE,
)
CONST_BLOCK_RE = re.compile(r"\bconst\s*\((.*?)\n\s*\)", re.DOTALL)
CONST_SPEC_RE = re.compile(
    r"^([A-Z][A-Z0-9_]*)\s*(?:(OpCode)\s*)?(?:=\s*(.*?))?\s*$"
)
OPCODE_EXPRESSION_RE = re.compile(
    r"^(0x[0-9a-fA-F]+|[0-9]+)(?:\s*\+\s*iota)?$"
)
OPCODE_DECLARATION_RE = re.compile(
    r"^\s*(?:const\s+)?([A-Z][A-Z0-9_]*)\s+OpCode\b", re.MULTILINE
)
GIT_OBJECT_ID_RE = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
GIT_REF_RE = re.compile(
    r"^(?:HEAD|refs/(?:heads|tags)/[A-Za-z0-9][A-Za-z0-9._/-]*)$"
)


class OpcodeAuditError(ValueError):
    """Raised when a metadata source is malformed or has drifted."""


@dataclass(frozen=True)
class UpstreamPolicy:
    aliases: Mapping[str, str]
    ignored: frozenset[str]


@dataclass(frozen=True)
class AuditResult:
    neverd_count: int
    upstream_count: int
    ignored_count: int


@dataclass(frozen=True)
class GethOpcodeSource:
    text: str
    revision: str


def _run_git(
    arguments: Sequence[str],
    *,
    git_executable: str = DEFAULT_GIT_EXECUTABLE,
) -> str:
    environment = os.environ.copy()
    environment["GIT_TERMINAL_PROMPT"] = "0"
    operation = next(
        (argument for argument in arguments if not argument.startswith("-")),
        "command",
    )
    try:
        result = subprocess.run(
            (git_executable, *arguments),
            check=False,
            capture_output=True,
            encoding="utf-8",
            env=environment,
            timeout=GIT_FETCH_TIMEOUT_SECONDS,
        )
    except FileNotFoundError as error:
        raise OpcodeAuditError(
            f"Git executable not found: {git_executable}"
        ) from error
    except subprocess.TimeoutExpired as error:
        raise OpcodeAuditError(
            f"git {operation} exceeded {GIT_FETCH_TIMEOUT_SECONDS} seconds"
        ) from error
    except OSError as error:
        raise OpcodeAuditError(f"could not execute Git: {error}") from error

    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        if not detail:
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


def _prepare_bare_cache(cache: Path, git_executable: str) -> None:
    if cache.exists():
        if not cache.is_dir():
            raise OpcodeAuditError(
                f"go-ethereum cache is not a bare Git repository: {cache}"
            )
        try:
            is_bare = _run_git(
                (f"--git-dir={cache}", "rev-parse", "--is-bare-repository"),
                git_executable=git_executable,
            ).strip()
        except OpcodeAuditError as error:
            raise OpcodeAuditError(
                f"go-ethereum cache is not a bare Git repository: {cache}"
            ) from error
        if is_bare != "true":
            raise OpcodeAuditError(
                f"go-ethereum cache is not a bare Git repository: {cache}"
            )
        return

    cache.parent.mkdir(parents=True, exist_ok=True)
    _run_git(
        ("init", "--bare", "--quiet", str(cache)),
        git_executable=git_executable,
    )


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
    cache_refspec = f"+{ref}:{GETH_CACHE_REF}"
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
            f"{GETH_CACHE_REF}^{{commit}}",
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
    return GethOpcodeSource(text=text, revision=revision)


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


def parse_neverd_opcodes(text: str) -> dict[str, int]:
    result: dict[str, int] = {}
    used_bytes: dict[int, str] = {}
    for match in NEVERD_OPCODE_RE.finditer(text):
        name = match.group(1)
        value = int(match.group(2), 16)
        if value > OPCODE_MAX:
            raise OpcodeAuditError(f"NeverD: {name} is outside the opcode byte")
        if name in result:
            raise OpcodeAuditError(f"NeverD: duplicate opcode name {name}")
        if value in used_bytes:
            raise OpcodeAuditError(
                f"NeverD: {name} and {used_bytes[value]} both use "
                f"{_format_opcode(value)}"
            )
        result[name] = value
        used_bytes[value] = name
    if not result:
        raise OpcodeAuditError("NeverD: no EVM_OPCODE records found")
    return result


def _evaluate_opcode_expression(expression: str, iota: int) -> int | None:
    compact = expression.strip()
    if not OPCODE_EXPRESSION_RE.fullmatch(compact):
        return None
    base_text = compact.split("+", maxsplit=1)[0].strip()
    value = int(base_text, 0)
    return value + iota if "iota" in compact else value


def parse_geth_opcodes(text: str) -> dict[str, int]:
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


def parse_policy(text: str) -> UpstreamPolicy:
    aliases: dict[str, str] = {}
    for local_name, upstream_name in POLICY_ALIAS_RE.findall(text):
        if local_name in aliases:
            raise OpcodeAuditError(f"policy: duplicate alias for {local_name}")
        aliases[local_name] = upstream_name
    ignored_names = POLICY_IGNORE_RE.findall(text)
    if len(ignored_names) != len(set(ignored_names)):
        raise OpcodeAuditError("policy: duplicate ignored upstream opcode")
    ignored = frozenset(ignored_names)
    overlap = ignored & set(aliases.values())
    if overlap:
        raise OpcodeAuditError(
            "policy: aliased upstream opcodes are also ignored: "
            + ", ".join(sorted(overlap))
        )
    return UpstreamPolicy(aliases=aliases, ignored=ignored)


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

    stale_ignores = sorted(policy.ignored - set(upstream))
    if stale_ignores:
        problems.append("stale ignored upstream names: " + ", ".join(stale_ignores))

    unreviewed = sorted(set(upstream) - consumed - policy.ignored)
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
        "--geth-root",
        type=Path,
        help=(
            "use an existing go-ethereum checkout instead of fetching the "
            "current upstream revision"
        ),
    )
    parser.add_argument(
        "--geth-remote",
        default=DEFAULT_GETH_REMOTE,
        help="Git remote fetched when --geth-root is not supplied",
    )
    parser.add_argument(
        "--geth-ref",
        default=DEFAULT_GETH_REF,
        help=(
            "remote HEAD or full refs/heads/... or refs/tags/... name fetched "
            "for the audit"
        ),
    )
    parser.add_argument(
        "--geth-cache",
        type=Path,
        default=DEFAULT_GETH_CACHE,
        help="bare Git cache refreshed before each remote audit",
    )
    parser.add_argument(
        "--git-executable",
        default=DEFAULT_GIT_EXECUTABLE,
        help="Git executable used to refresh the upstream cache",
    )
    parser.add_argument(
        "--neverd-opcodes", type=Path, default=DEFAULT_NEVERD_OPCODES
    )
    parser.add_argument("--policy", type=Path, default=DEFAULT_POLICY)
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        neverd = parse_neverd_opcodes(args.neverd_opcodes.read_text(encoding="utf-8"))
        if args.geth_root is None:
            source = fetch_geth_opcode_source(
                remote=args.geth_remote,
                ref=args.geth_ref,
                cache=args.geth_cache,
                git_executable=args.git_executable,
            )
            upstream_text = source.text
            source_description = source.revision
        else:
            upstream_path = args.geth_root / GETH_OPCODE_PATH
            upstream_text = upstream_path.read_text(encoding="utf-8")
            source_description = str(upstream_path)
        upstream = parse_geth_opcodes(upstream_text)
        policy = parse_policy(args.policy.read_text(encoding="utf-8"))
        result = audit_opcodes(neverd, upstream, policy)
    except (OSError, OpcodeAuditError) as error:
        print(f"EVM opcode audit failed: {error}", file=sys.stderr)
        return 1

    print(
        f"EVM opcode metadata matches go-ethereum {source_description}: "
        f"{result.neverd_count} NeverD records, "
        f"{result.upstream_count} upstream constants, "
        f"{result.ignored_count} explicit exclusions"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
