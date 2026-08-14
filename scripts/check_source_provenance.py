#!/usr/bin/env python3
"""Fail the build on private path leaks and unreviewed provenance markers.

The repository contains only generic, non-sensitive checks.  Project-specific
identifiers may be supplied at invocation time through an external policy file;
that file must live outside the repository.  This keeps the public checker
useful without turning its source into a catalogue of private vocabulary.

Required licence notices and attribution must remain.  Published research and
public tools should be cited deliberately where NeverD builds on them.  A
marker with no citation or explanation instead requires review, so the checker
asks for either a documented allowance or a terminology correction.

Every repository allowance is listed below with a reason, so an allowed hit is
a decision somebody made rather than a pattern nobody tightened.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from fnmatch import fnmatch
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[1]
EXTERNAL_POLICY_ENV = "NEVERD_SOURCE_PROVENANCE_POLICY"
EXTERNAL_POLICY_SCHEMA = 1
MAX_EXTERNAL_POLICY_BYTES = 1024 * 1024

# The checker spells out every generic pattern it looks for, and its tests
# contain matching examples.  Both would otherwise report themselves.
SELF = Path(__file__).resolve().relative_to(REPO_ROOT).as_posix()
EXEMPT = frozenset({SELF, "scripts/tests/test_check_source_provenance.py"})

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
        ".s",
        ".sh",
        ".toml",
        ".ts",
        ".tsx",
        ".txt",
        ".yaml",
        ".yml",
    }
)

_ASCII_TOKEN = r"[A-Za-z0-9_]+(?:-[A-Za-z0-9_]+)*"
ASCII_TERM_RE = re.compile(_ASCII_TOKEN)
POLICY_TERM_RE = re.compile(rf"{_ASCII_TOKEN}(?: {_ASCII_TOKEN})?")


@dataclass(frozen=True, slots=True)
class Rule:
    name: str
    explanation: str
    pattern: re.Pattern[str] | None = None
    terms: frozenset[str] = frozenset()


RULES: tuple[Rule, ...] = (
    Rule(
        name="private-path",
        explanation="an absolute path from a developer's machine",
        pattern=re.compile(
            r"(?:/Users/[^/\r\n]+|/home/[^/\r\n]+"
            r"|[Cc]:\\\\?Users\\\\?[^\\/\r\n]+)"
        ),
    ),
    Rule(
        name="provenance-phrase",
        explanation=(
            "wording that presents something as taken from elsewhere without "
            "saying from where"
        ),
        pattern=re.compile(
            # Anchoring on word boundaries avoids matching ordinary words such
            # as "reported" and "exported" through a shorter suffix.
            r"(?:\binspired by\b|\badapted from\b|\bported from\b"
            r"|\bfrom the paper\b|\bin the literature\b|\bas published in\b"
            r"|\ba port of\b|\breimplementation of\b)",
            re.IGNORECASE,
        ),
    ),
)


@dataclass(frozen=True, slots=True)
class PolicyCategory:
    field: str
    rule: str
    explanation: str


POLICY_CATEGORIES: tuple[PolicyCategory, ...] = (
    PolicyCategory(
        field="private_identifiers",
        rule="private-path",
        explanation="an identifier configured by the private review policy",
    ),
    PolicyCategory(
        field="foreign_projects",
        rule="foreign-project",
        explanation=(
            "a project name configured by the review policy; cite it "
            "deliberately or remove the accidental reference"
        ),
    ),
    PolicyCategory(
        field="foreign_terminology",
        rule="foreign-terminology",
        explanation=(
            "tool-specific vocabulary configured by the review policy; use "
            "the repository's own terminology"
        ),
    ),
)


class ExternalPolicyError(ValueError):
    """An external policy cannot be applied safely."""


class GitScanError(RuntimeError):
    """Git could not provide a patch required by the scan."""


def _external_policy_path(environ: Mapping[str, str]) -> Path | None:
    raw_path = environ.get(EXTERNAL_POLICY_ENV)
    if raw_path is None:
        return None
    if not raw_path:
        raise ExternalPolicyError(
            f"{EXTERNAL_POLICY_ENV} must name an absolute policy file"
        )

    candidate = Path(raw_path)
    if not candidate.is_absolute():
        raise ExternalPolicyError(
            f"{EXTERNAL_POLICY_ENV} must name an absolute policy file"
        )

    try:
        resolved = candidate.resolve(strict=True)
    except OSError:
        raise ExternalPolicyError(
            f"{EXTERNAL_POLICY_ENV} does not identify a readable policy file"
        ) from None

    repository = REPO_ROOT.resolve()
    try:
        resolved.relative_to(repository)
    except ValueError:
        pass
    else:
        raise ExternalPolicyError(
            f"{EXTERNAL_POLICY_ENV} must point outside the repository"
        )

    if not resolved.is_file():
        raise ExternalPolicyError(
            f"{EXTERNAL_POLICY_ENV} does not identify a readable policy file"
        )
    return resolved


def _unique_json_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ExternalPolicyError("external policy has a duplicate field")
        result[key] = value
    return result


def _parse_external_policy(document: Any) -> tuple[Rule, ...]:
    if type(document) is not dict:
        raise ExternalPolicyError("external policy must be a JSON object")

    known_fields = {"schema", *(category.field for category in POLICY_CATEGORIES)}
    if set(document) - known_fields:
        raise ExternalPolicyError("external policy has an unsupported field")
    if type(document.get("schema")) is not int or document["schema"] != 1:
        raise ExternalPolicyError(
            f"external policy schema must be {EXTERNAL_POLICY_SCHEMA}"
        )

    seen_terms: set[str] = set()
    rules: list[Rule] = []
    for category in POLICY_CATEGORIES:
        configured = document.get(category.field, [])
        if type(configured) is not list:
            raise ExternalPolicyError("external policy term groups must be arrays")

        terms: set[str] = set()
        for term in configured:
            if (
                type(term) is not str
                or term != term.strip()
                or POLICY_TERM_RE.fullmatch(term) is None
            ):
                raise ExternalPolicyError(
                    "external policy terms must be one or two ASCII identifiers"
                )
            canonical = term.casefold()
            if canonical in seen_terms:
                raise ExternalPolicyError("external policy terms must be unique")
            seen_terms.add(canonical)
            terms.add(canonical)

        if terms:
            rules.append(
                Rule(
                    name=category.rule,
                    explanation=category.explanation,
                    terms=frozenset(terms),
                )
            )

    if not seen_terms:
        raise ExternalPolicyError("external policy must contain at least one term")
    return tuple(rules)


def load_external_rules(
    environ: Mapping[str, str] | None = None,
) -> tuple[Rule, ...]:
    """Load strict review rules without exposing their path or contents."""

    active_environ = os.environ if environ is None else environ
    path = _external_policy_path(active_environ)
    if path is None:
        return ()

    try:
        with path.open("rb") as stream:
            encoded = stream.read(MAX_EXTERNAL_POLICY_BYTES + 1)
    except OSError:
        raise ExternalPolicyError("external policy could not be read") from None
    if len(encoded) > MAX_EXTERNAL_POLICY_BYTES:
        raise ExternalPolicyError("external policy exceeds the size limit")

    try:
        document = json.loads(
            encoded.decode("utf-8"), object_pairs_hook=_unique_json_object
        )
    except UnicodeDecodeError:
        raise ExternalPolicyError("external policy must be valid UTF-8 JSON") from None
    except json.JSONDecodeError:
        raise ExternalPolicyError("external policy must be valid UTF-8 JSON") from None

    return _parse_external_policy(document)


def line_terms(line: str) -> set[str]:
    """Return ASCII identifiers plus adjacent two-token phrases."""

    terms = [match.group(0).casefold() for match in ASCII_TERM_RE.finditer(line)]
    candidates = set(terms)
    candidates.update(f"{left} {right}" for left, right in zip(terms, terms[1:]))
    return candidates


@dataclass(frozen=True, slots=True)
class Allowance:
    path: str
    rule: str
    reason: str


ALLOWED: tuple[Allowance, ...] = (
    Allowance(
        path="include/neverd/sbf/solana/SBFAnchorNames.def",
        rule="foreign-project",
        reason=(
            "deployed program instruction identifiers; the collision with a "
            "review-policy term is accidental"
        ),
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
    Allowance(
        path=".agents/skills/awesome-game-security-overview/SKILL.md",
        rule="foreign-terminology",
        reason="the skill routes research queries by the external tools they name",
    ),
    Allowance(
        path=".agents/skills/binary-lifting/SKILL.md",
        rule="foreign-project",
        reason="the skill is an attributed survey of binary-lifting frameworks",
    ),
    Allowance(
        path=".agents/skills/game-engine-resources/SKILL.md",
        rule="foreign-terminology",
        reason="the skill documents the external tools used in engine research",
    ),
    Allowance(
        path=".agents/skills/mobile-security/SKILL.md",
        rule="foreign-terminology",
        reason="the skill documents the external tools used in mobile research",
    ),
    Allowance(
        path=".agents/skills/reverse-engineering-tools/SKILL.md",
        rule="foreign-project",
        reason="the skill is an attributed catalog of reverse-engineering tools",
    ),
    Allowance(
        path=".agents/skills/reverse-engineering-tools/SKILL.md",
        rule="foreign-terminology",
        reason="the skill is an attributed catalog of reverse-engineering tools",
    ),
)


def is_allowed(path: str, rule: str) -> bool:
    return any(a.rule == rule and fnmatch(path, a.path) for a in ALLOWED)


def tracked_files() -> list[str]:
    """Return every file Git tracks, leaving submodules opaque."""

    output = subprocess.run(
        ["git", "ls-files", "-z"],
        cwd=REPO_ROOT,
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    return [entry for entry in output.split("\0") if entry]


def worktree_files() -> list[str]:
    """Return tracked plus pending, non-ignored files in the worktree."""

    output = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        cwd=REPO_ROOT,
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    return sorted({entry for entry in output.split("\0") if entry})


def _scan_text(
    name: str,
    text: str,
    rules: Sequence[Rule],
    *,
    display_name: str | None = None,
) -> list[str]:
    findings: list[str] = []
    if name in EXEMPT or Path(name).suffix.lower() not in TEXT_SUFFIXES:
        return findings
    active_rules = tuple(rule for rule in rules if not is_allowed(name, rule.name))
    needs_terms = any(rule.terms for rule in active_rules)
    for number, line in enumerate(text.splitlines(), start=1):
        candidates = line_terms(line) if needs_terms else set()
        for rule in active_rules:
            matched_pattern = (
                rule.pattern is not None and rule.pattern.search(line) is not None
            )
            matched_term = bool(rule.terms & candidates)
            if not matched_pattern and not matched_term:
                continue
            findings.append(
                f"{display_name or name}:{number}: {rule.name} — {rule.explanation}"
            )
    return findings


def scan(paths: Sequence[str], rules: Sequence[Rule] | None = None) -> list[str]:
    findings: list[str] = []
    selected_rules = RULES if rules is None else tuple(rules)
    for name in paths:
        if name in EXEMPT:
            continue
        path = REPO_ROOT / name
        if not path.is_file() or path.suffix.lower() not in TEXT_SUFFIXES:
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        findings.extend(_scan_text(name, text, selected_rules))
    return findings


def _git_output(arguments: Sequence[str]) -> str:
    try:
        return subprocess.run(
            ["git", *arguments],
            cwd=REPO_ROOT,
            check=True,
            capture_output=True,
            text=True,
        ).stdout
    except (OSError, subprocess.CalledProcessError):
        raise GitScanError("Git patch inventory could not be read") from None


def _patch_payload(patch: str, *, deleted_only: bool) -> str:
    payload: list[str] = []
    in_hunk = False
    for line in patch.splitlines():
        if line.startswith("diff --git "):
            in_hunk = False
            continue
        if line.startswith("@@"):
            in_hunk = True
            continue
        if not in_hunk or not line:
            continue
        if line[0] == "-" or (not deleted_only and line[0] == "+"):
            payload.append(line[1:])
    return "\n".join(payload)


def _scan_patch_set(
    names: Sequence[str],
    patch: str,
    label: str,
    rules: Sequence[Rule],
    *,
    deleted_only: bool,
) -> list[str]:
    starts = [match.start() for match in re.finditer(r"(?m)^diff --git ", patch)]
    sections = [
        patch[start : starts[index + 1] if index + 1 < len(starts) else None]
        for index, start in enumerate(starts)
    ]
    if len(sections) != len(names):
        raise GitScanError("Git patch content does not match its file inventory")

    findings: list[str] = []
    for name, section in zip(names, sections, strict=True):
        payload = _patch_payload(section, deleted_only=deleted_only)
        if payload:
            findings.extend(
                _scan_text(
                    name,
                    payload,
                    rules,
                    display_name=f"{name}@{label}",
                )
            )
    return findings


def scan_worktree_patch(rules: Sequence[Rule] | None = None) -> list[str]:
    """Scan deleted tracked lines that a snapshot-only check cannot see."""

    selected_rules = RULES if rules is None else tuple(rules)
    names = [
        entry
        for entry in _git_output(
            ["diff", "--name-only", "--no-renames", "-z", "HEAD", "--"]
        ).split("\0")
        if entry
    ]
    return _scan_patch_set(
        names,
        _git_output(
            ["diff", "--no-ext-diff", "--no-renames", "--unified=0", "HEAD", "--"]
        ),
        "worktree-deletion",
        selected_rules,
        deleted_only=True,
    )


def scan_recent_history(
    commit_count: int, rules: Sequence[Rule] | None = None
) -> list[str]:
    """Scan added and deleted patch text for the newest Git commits."""

    if commit_count < 0:
        raise ValueError("commit count cannot be negative")
    if commit_count == 0:
        return []
    selected_rules = RULES if rules is None else tuple(rules)
    commits = _git_output(
        ["rev-list", "--max-count", str(commit_count), "HEAD"]
    ).splitlines()
    findings: list[str] = []
    for commit in commits:
        names = [
            entry
            for entry in _git_output(
                [
                    "diff-tree",
                    "--root",
                    "--first-parent",
                    "--no-commit-id",
                    "--name-only",
                    "--no-renames",
                    "-r",
                    "-z",
                    commit,
                ]
            ).split("\0")
            if entry
        ]
        findings.extend(
            _scan_patch_set(
                names,
                _git_output(
                    [
                        "show",
                        "--format=",
                        "--first-parent",
                        "--no-ext-diff",
                        "--no-renames",
                        "--unified=0",
                        commit,
                        "--",
                    ]
                ),
                commit[:12],
                selected_rules,
                deleted_only=False,
            )
        )
    return findings


def main(
    argv: Sequence[str] | None = None,
    *,
    environ: Mapping[str, str] | None = None,
) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "paths",
        nargs="*",
        help="files to scan; defaults to tracked and pending non-ignored files",
    )
    parser.add_argument(
        "--tracked-only",
        action="store_true",
        help="scan only the tracked release tree",
    )
    parser.add_argument(
        "--history-commits",
        type=int,
        default=0,
        metavar="N",
        help="also scan added and deleted text in the newest N commits",
    )
    arguments = parser.parse_args(argv)

    if arguments.history_commits < 0:
        parser.error("--history-commits must be non-negative")

    try:
        external_rules = load_external_rules(environ)
    except ExternalPolicyError as error:
        print(f"error: source provenance policy: {error}", file=sys.stderr)
        return 2

    if arguments.paths:
        paths = arguments.paths
        inventory = "selected"
    elif arguments.tracked_only:
        paths = tracked_files()
        inventory = "tracked"
    else:
        paths = worktree_files()
        inventory = "worktree"

    selected_rules = (*RULES, *external_rules)
    try:
        findings = scan(paths, selected_rules)
        if not arguments.paths and not arguments.tracked_only:
            findings.extend(scan_worktree_patch(selected_rules))
        findings.extend(scan_recent_history(arguments.history_commits, selected_rules))
    except GitScanError as error:
        print(f"error: source provenance history: {error}", file=sys.stderr)
        return 2
    if findings:
        for finding in findings:
            print(f"error: {finding}", file=sys.stderr)
        print(
            f"\n{len(findings)} provenance findings. Correct accidental text "
            f"or add a reviewed entry to ALLOWED in {SELF} explaining the "
            "citation, licence, or terminology reason. Do not remove required "
            "attribution.",
            file=sys.stderr,
        )
        return 1
    print(f"source provenance check passed: {len(paths)} {inventory} files scanned")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
