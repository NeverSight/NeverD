#!/usr/bin/env python3
"""Fail the build when a tracked file would tell a reader where NeverD's ideas
came from, or where its author's home directory is.

Two different things are being kept out, and they are worth telling apart.

The first is a *private path*: an absolute path from a developer's machine, or a
reference to a working directory that is deliberately not in the repository.
Those leak the shape of someone's disk into a published artifact and are simply
a mistake.

The second is a *provenance marker*: the name of another project alongside
wording that presents it as a source, or terminology that belongs to one
particular tool rather than to the field.  NeverD's own vocabulary is its own --
`NdVar`, `NdOp`, `SymRef` -- and a stray borrowed term reads as a fragment of
somebody else's design left in place, whatever the truth is.  The point is not
that referring to other work is wrong; published research and public tools are
cited properly where NeverD builds on them.  The point is that a term appearing
with no citation and no explanation is worse than either citing it or not using
it, so this asks for one or the other.

Every exception is listed below with a reason, so an allowed hit is a decision
somebody made rather than a pattern nobody tightened.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from fnmatch import fnmatch
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]

# This file necessarily spells every pattern it looks for.
SELF = Path(__file__).resolve().relative_to(REPO_ROOT).as_posix()

TEXT_SUFFIXES = frozenset(
    {
        "",
        ".c",
        ".cc",
        ".cfg",
        ".cmake",
        ".cpp",
        ".def",
        ".h",
        ".hpp",
        ".in",
        ".inc",
        ".ini",
        ".json",
        ".md",
        ".py",
        ".rs",
        ".sh",
        ".toml",
        ".txt",
        ".yaml",
        ".yml",
    }
)


@dataclass(frozen=True, slots=True)
class Rule:
    name: str
    pattern: re.Pattern[str]
    explanation: str


def _words(*terms: str) -> str:
    return r"\b(?:" + "|".join(terms) + r")\b"


RULES: tuple[Rule, ...] = (
    Rule(
        name="private-path",
        pattern=re.compile(
            r"(?:/Users/[A-Za-z0-9._-]+|/home/[A-Za-z0-9._-]+"
            r"|[Cc]:\\\\?Users\\\\?[A-Za-z0-9._-]+|\blocal_docs\b)"
        ),
        explanation=(
            "an absolute path from a developer's machine, or a directory kept "
            "out of the repository on purpose"
        ),
    ),
    Rule(
        name="foreign-project",
        pattern=re.compile(
            _words(
                "angr",
                "bitwuzla",
                "claripy",
                "cobra",
                "gamba",
                "libvex",
                "mcsema",
                "msynth",
                "neureduce",
                "pyvex",
                "qsynth",
                "remill",
                "retdec",
                "rumba",
                "souper",
                "syntia",
                "triton",
                "vexa",
                "xyntia",
            ),
            re.IGNORECASE,
        ),
        explanation=(
            "the name of another project; cite it deliberately or do not name "
            "it at all"
        ),
    ),
    Rule(
        name="foreign-terminology",
        pattern=re.compile(
            # Every alternative is anchored: "setup-codex" contains "p-code"
            # and is nobody's terminology.
            r"(?:"
            + _words("varnode", "pcode", "sleigh", "ghidra", "hexrays", "binaryninja")
            + r"|\bp-code\b|\bhex-rays\b|\bbinary ninja\b"
            + r"|\bMLIL\b|\bHLIL\b|\bLLIL\b)",
            re.IGNORECASE,
        ),
        explanation=(
            "vocabulary belonging to one particular tool rather than to the "
            "field; NeverD names its own things"
        ),
    ),
    Rule(
        name="provenance-phrase",
        pattern=re.compile(
            # Anchored on a word boundary, because "reported from" and
            # "exported from" are ordinary English and "ported from" inside
            # them is not a claim about where anything came from.  Phrases
            # general enough to be ordinary English on their own -- "derived
            # from", say -- are deliberately absent: a check that cries wolf
            # gets switched off.
            r"(?:\binspired by\b|\badapted from\b|\bported from\b"
            r"|\bfrom the paper\b|\bin the literature\b|\bas published in\b"
            r"|\ba port of\b|\breimplementation of\b)",
            re.IGNORECASE,
        ),
        explanation=(
            "wording that presents something as taken from elsewhere without "
            "saying from where"
        ),
    ),
)


@dataclass(frozen=True, slots=True)
class Allowance:
    path: str
    rule: str
    reason: str


ALLOWED: tuple[Allowance, ...] = (
    Allowance(
        path="include/neverd/sbf/SBFAnchorNames.def",
        rule="foreign-project",
        reason=(
            "`gamba_*` are instruction names of a deployed Solana program, "
            "recovered by deriving their discriminators; the collision with a "
            "tool of the same name is accidental"
        ),
    ),
    Allowance(
        path=".gitignore",
        rule="private-path",
        reason="naming the directory is how it is kept out of the repository",
    ),
    Allowance(
        path="docs/sbf*.md",
        rule="foreign-terminology",
        reason=(
            "a survey of the SBF tooling that was considered as a semantic "
            "oracle and why each was not used, pinned to the commit reviewed; "
            "naming them is the point of the section"
        ),
    ),
)


def is_allowed(path: str, rule: str) -> bool:
    return any(a.rule == rule and fnmatch(path, a.path) for a in ALLOWED)


def tracked_files() -> list[str]:
    """Every file git tracks.

    Submodules appear as a single entry rather than as their contents, so
    vendored trees are out of scope without having to be named here.
    """

    output = subprocess.run(
        ["git", "ls-files", "-z"],
        cwd=REPO_ROOT,
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    return [entry for entry in output.split("\0") if entry]


def scan(paths: list[str]) -> list[str]:
    findings: list[str] = []
    for name in paths:
        if name == SELF:
            continue
        path = REPO_ROOT / name
        if not path.is_file() or path.suffix.lower() not in TEXT_SUFFIXES:
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue

        for rule in RULES:
            if is_allowed(name, rule.name):
                continue
            for number, line in enumerate(text.splitlines(), start=1):
                match = rule.pattern.search(line)
                if match is None:
                    continue
                findings.append(
                    f"{name}:{number}: {rule.name}: {match.group(0)!r} — "
                    f"{rule.explanation}"
                )
    return findings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "paths",
        nargs="*",
        help="files to scan; defaults to everything git tracks",
    )
    arguments = parser.parse_args()

    paths = arguments.paths or tracked_files()
    findings = scan(paths)
    if findings:
        for finding in findings:
            print(f"error: {finding}", file=sys.stderr)
        print(
            f"\n{len(findings)} provenance findings. Either remove the text, or "
            f"add a reviewed entry to ALLOWED in {SELF} saying why it stays.",
            file=sys.stderr,
        )
        return 1
    print(f"source provenance check passed: {len(paths)} tracked files scanned")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
