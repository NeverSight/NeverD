#!/usr/bin/env python3
"""Audit NeverD's EVM opcode database against a go-ethereum checkout."""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping, Sequence


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_NEVERD_OPCODES = REPO_ROOT / "include/neverd/evm/EVMOpcodes.def"
DEFAULT_POLICY = REPO_ROOT / "include/neverd/evm/EVMUpstreamOpcodePolicy.def"
DEFAULT_GETH_ROOT = REPO_ROOT / "local_docs/go-ethereum"
GETH_OPCODE_PATH = Path("core/vm/opcodes.go")
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
    parser.add_argument("--geth-root", type=Path, default=DEFAULT_GETH_ROOT)
    parser.add_argument(
        "--neverd-opcodes", type=Path, default=DEFAULT_NEVERD_OPCODES
    )
    parser.add_argument("--policy", type=Path, default=DEFAULT_POLICY)
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        neverd = parse_neverd_opcodes(args.neverd_opcodes.read_text(encoding="utf-8"))
        upstream_path = args.geth_root / GETH_OPCODE_PATH
        upstream = parse_geth_opcodes(upstream_path.read_text(encoding="utf-8"))
        policy = parse_policy(args.policy.read_text(encoding="utf-8"))
        result = audit_opcodes(neverd, upstream, policy)
    except (OSError, OpcodeAuditError) as error:
        print(f"EVM opcode audit failed: {error}", file=sys.stderr)
        return 1

    print(
        "EVM opcode metadata matches go-ethereum: "
        f"{result.neverd_count} NeverD records, "
        f"{result.upstream_count} upstream constants, "
        f"{result.ignored_count} explicit exclusions"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
