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
import hashlib
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
        path="LICENSES/node/LICENSE",
        rule="provenance-phrase",
        reason=(
            "Verbatim Node.js dependency notices identify the SES/Caja "
            "copyright holders and Apache-2.0 license. Preserve this required "
            "upstream attribution; it is not a NeverD implementation claim."
        ),
    ),
    Allowance(
        path="LICENSES/bun/LICENSE.md",
        rule="provenance-phrase",
        reason=(
            "Verbatim upstream license credits link to the original project; "
            "the required notice is not a NeverD implementation claim."
        ),
    ),
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


@dataclass(frozen=True, slots=True)
class HistoryAllowance:
    path: str
    rule: str
    commits: frozenset[str]
    line: str
    reason: str
    # Exact UTF-8 line hashes let a reviewed cleanup avoid republishing private
    # text in this table. Path and full commit restrictions still apply.
    line_sha256: frozenset[str] = frozenset()


HISTORY_ALLOWED: tuple[HistoryAllowance, ...] = (
    HistoryAllowance(
        path="tools/neverd-gui/benchmarks/results/linux-x86_64-20261010-linear-copy-performance.json",
        rule="private-path",
        commits=frozenset({
            "b3acf92849e63cfe95f25eeb25903f941f5289ce",
            "ceca16a8a76bce78ac0cbf75cea40c9549c551d8",
        }),
        line="",
        reason=(
            "The published GUI linear-copy report was removed from tracked "
            "sources with the other local performance artifacts. Review binds "
            "only the exact path-line digests in its original publication and "
            "deletion commits; current and future text remains fully checked."
        ),
        line_sha256=frozenset({
            "00870df07891e305ba7fad70012faf7bba817c7ec27288e433e4eac2e54b2e19",
            "08c68f1b773654cd75c5a1183026635f0c4fbfabca5e60f761bf1ccb4261552d",
            "144287a60c3e1690a17c277185076d5833f2eb7cf2873ea02e705698734a56ad",
            "1dabfb5f95cf28171adea8f5bf4db308842ff337e066ed998cd54a3d17ff3b79",
            "2994f63321fe5f0512f35e3dede07db18394d89deaa6e5cc8ef0daee8c485907",
            "2f1e4308455cf8a0863f360a25ff313550c7a955c1efa716c654cc2f32b1ef43",
            "424669a717456884633db23e2687ee973f75e204cf9fe1eff3c594ad0ae513ee",
            "5b7f31f6aa247ff64b276ef4f852f6540e8228ffe4a8669ebb142a54f58d2cf3",
            "5bb9528f4766e3c319e0f422b0a4244cef73a2e1524478fd68e6ec990e8926c6",
            "5d06dc546760ca6ea0a5ed9a4b50bd9df120bd2aebe9abfc531fa84dbb4aebc1",
            "84cf35a9fcc970b2bfd1cd6c0ee6a53447e2c205a97eaa0f5c1b94d3d81f4611",
            "981c2cfcc6ac0eea8767e03da7327a683a59f9b3c7d996bca04736c87e82dfbc",
            "d5887401515e4271c72491cdbfa5425f9b1010c04c2c7eb0161ee56edf30596c",
            "dccd904594647396c97fa1d2ff5d83436adf4cc23f1e0a87550031a0d4ecc449",
            "deb466b1d26945f392137d17db884bda9653bdd3bb9fe9cd4ac57d23cbc45909",
            "dfb8c743f59afb7992bfe2519a46eefc990333b27a55b4f82eb968249e7e8181",
            "fe12b9c137ca8667b23aed9bf1ee5d189d0e5bdec5199fc95741fedf8251d6c9",
            "ff3ec4dd41b019a322b02ded8cc2e3946fd718afb1ccd21b02dc47d0750160f6",
        }),
    ),
    HistoryAllowance(
        path="tools/neverd-gui/benchmarks/results/linux-x86_64-20261010-rust-pseudocode-round4.json",
        rule="private-path",
        commits=frozenset({"8bf008bdedc5fb59bbe043e7185a0d91c2b1b567"}),
        line="",
        reason=(
            "Reviewed deletion of the benchmark's local SDK compiler path. "
            "The cleanup changed only that path; all measurements and binary "
            "hashes are unchanged. The exact historical line digest avoids "
            "republishing the removed prefix and permits no current or future "
            "occurrence."
        ),
        line_sha256=frozenset({
            "f1965d87619623cafc0db93fb92f69f0c75563edca54be2304e562bcae0cca49",
        }),
    ),
    HistoryAllowance(
        path="docs/benchmarks/2026-10-05-arm64-hvf-completion-atomic.json",
        rule="private-path",
        commits=frozenset({
            "a50bff6e89aae6514cfb82464531a2738651e1c6",
            "72e28d1c99e49fe067eb7a485d2605c2f200a100",
            "ac78f59c2bf2bf75d734f5ef8ca35a8e8093c120",
        }),
        line="",
        reason=(
            "Reviewed historical ARM64 HVF benchmark paths. Publication "
            "normalizes only checkout/cache prefixes and preserves measurements "
            "and binary hashes. Exact line digests avoid republishing the "
            "private prefixes; no current or future occurrence is permitted."
        ),
        line_sha256=frozenset({
            "117b049ad55781d3b77eb19f2f5b2bdae9fbb3fb71dd45ae6332152912bfb86c",
            "5125e718871cef21f168a6449649f8a6bda61f76db780ed10c59de1c2ff2c1ae",
            "71abb0673f1506ba1eff9aca60596139f32f841297c695ec00fd24c91828ec9f",
            "8895bddb55b50a7a37ebb1bcfe56e442ed7f9d15dda200e917a1d35bdf028424",
            "a93a76c6d5165b7409301cfda34eb7c8514fde1068e8e2cd970177f87ea90aff",
            "d228c49e6de0d0ff8aa9aae5dcce3f46aa872bb0603e00a260c1a8b26a83c664",
            "da5a7589b071e998ea2b3569eba3c21e4fd088225d158747ee7f367a23831914",
            "ea7f836e085b39ab14cf8d7c67728b9292b772b1cac19cc45fe774060c24e5cf",
        }),
    ),
    HistoryAllowance(
        path="docs/benchmarks/2026-10-05-arm64-hvf-completion-blocking.json",
        rule="private-path",
        commits=frozenset({
            "a50bff6e89aae6514cfb82464531a2738651e1c6",
            "72e28d1c99e49fe067eb7a485d2605c2f200a100",
            "ac78f59c2bf2bf75d734f5ef8ca35a8e8093c120",
        }),
        line="",
        reason=(
            "Reviewed historical ARM64 HVF benchmark paths. Publication "
            "normalizes only checkout/cache prefixes and preserves measurements "
            "and binary hashes. Exact line digests avoid republishing the "
            "private prefixes; no current or future occurrence is permitted."
        ),
        line_sha256=frozenset({
            "6749c867ff97a650cf2d55e195158269ca59f96946c8280979e3e1b1e1b12d6a",
            "71abb0673f1506ba1eff9aca60596139f32f841297c695ec00fd24c91828ec9f",
            "ae135f6f4741c3dbcae82bd3038a408dcbfb15f3858ec55eb476ad5f5f30c45e",
            "d228c49e6de0d0ff8aa9aae5dcce3f46aa872bb0603e00a260c1a8b26a83c664",
            "d8fffa7dc8dd869deb39276ed89191cbcf64ad1da13ef308c755d9352e0c27a9",
            "da5a7589b071e998ea2b3569eba3c21e4fd088225d158747ee7f367a23831914",
            "df7dd3a9cd21b0e8e3153c31a14f91f6cbbdb8b3338b05a300988529da645e59",
            "ea7f836e085b39ab14cf8d7c67728b9292b772b1cac19cc45fe774060c24e5cf",
        }),
    ),
    HistoryAllowance(
        path="docs/benchmarks/2026-10-05-arm64-hvf-completion-metadata.json",
        rule="private-path",
        commits=frozenset({
            "a50bff6e89aae6514cfb82464531a2738651e1c6",
            "72e28d1c99e49fe067eb7a485d2605c2f200a100",
            "ac78f59c2bf2bf75d734f5ef8ca35a8e8093c120",
        }),
        line="",
        reason=(
            "Reviewed historical ARM64 HVF benchmark paths. Publication "
            "normalizes only checkout/cache prefixes and preserves measurements "
            "and binary hashes. Exact line digests avoid republishing the "
            "private prefixes; no current or future occurrence is permitted."
        ),
        line_sha256=frozenset({
            "30cce435771a4e82d1bd7ba6270a12cdcc12f0695681bdae63235a2259f293e1",
            "32068396a6531eed8ebe7d5cf6d8931d1919100d3c9f7aca7f50a8aef138a1b3",
            "4c6cf7808a4c5b68083e1107750570ccc539f52873bd98b3b2d1795fd760eb1f",
            "6b4bc77a6c220a20f6df0ecaa87db0476a2ef03ba2167297dddf1615c8d33b66",
            "7d41e56a75b8b4ebd0b5fa0b14918ea4d8c455eecbb3a8bb46f086d852455231",
            "b95a48a96ac1debfa8b7c43209118fb23fbef6fe63eeb546a5015031dac9df5f",
            "c7ce6a2662ad1dad8c532c8909a45658d9ca24ae6247d10ea10c1c6f55451094",
            "d11fb797aa844886e33cda5d2a58e9ef1c7f2b1b23cfdf8e66a692ecc33380e8",
            "e3d1e9d5548f09c4756b3182364604c1ba8c42af9def4204dab2f777c50f4e6e",
            "f87e91d4618b654be74d28cccbbfec76d6db122f6f35bb73e898714a7ef2c321",
            "fdbd79b59897334a3cc92d4e3e5652c2c92696529da1dccfc34b9c3422f4ecb3",
        }),
    ),
    HistoryAllowance(
        path="docs/benchmarks/2026-10-05-arm64-hvf-watchdog.json",
        rule="private-path",
        commits=frozenset({
            "1655143e6cbac4e080aadb65f6a6fbff05c63cd8",
            "72e28d1c99e49fe067eb7a485d2605c2f200a100",
            "ac78f59c2bf2bf75d734f5ef8ca35a8e8093c120",
        }),
        line="",
        reason=(
            "Reviewed historical ARM64 HVF benchmark paths. Publication "
            "normalizes only checkout/cache prefixes and preserves measurements "
            "and binary hashes. Exact line digests avoid republishing the "
            "private prefixes; no current or future occurrence is permitted."
        ),
        line_sha256=frozenset({
            "1af51e627f88bc5e24c1fb921cb43d2520c2604e1760e5cdf925a128c77869c6",
            "1b6b6a19768095b78684d9e67dd16f7abaae0acb99267bfaf0c0680660f963a6",
            "2184761a48ce1860e7bbe302d4222c19aec4e59ecc767fc6211cbbad1e21a524",
            "3d4748943838bba4e49a8def71577c79e0ebd6ebe51e5d5d437a13f38efe1ab0",
            "758d4bec8e9496a3fab64903ec8f28c2e89c33b8b600d47078b47f7e30cd2df4",
            "9761aea20a100092e8cdad8059623f6932dacd9524c894448a259e1686d83ab6",
            "9b05769070913f2d16df97b8b5a2e72d530a25c33f84080c8f6ba2a7c624e227",
            "aeacc6d5650dbc43883a24cff65d377e0665dc8613e2207bb04efbd1482fc49c",
        }),
    ),
    HistoryAllowance(
        path="unittests/semantic/x86/X86_X87TranscendentalRTTests.cpp",
        rule="provenance-phrase",
        commits=frozenset({"4fa1b1f9b384a24335828131150b285667c2d7cc"}),
        line="// Adapted from Packmad's MIT-licensed fprem-anti-emulation, revision",
        reason=(
            "This commit replaced the attribution wording with a pinned source "
            "URL while preserving Packmad's MIT attribution and the references "
            "to THIRD_PARTY_NOTICES.md and LICENSES/FPREM-Anti-Emulation.txt. "
            "Review covers only this exact historical deletion; it permits no "
            "new occurrences or changes to the required attribution."
        ),
    ),
    HistoryAllowance(
        path="scripts/tests/test_first_fatal_snapshots.py",
        rule="private-path",
        commits=frozenset({
            "db077ee2cb8a5fbb8d9431a3e385fbebfc47e4f1",
            "322dc95d96133313c55f6782cb1ac65e1290ac62",
        }),
        line=(
            '            command = ["/home/runner/work/NeverD/NeverD/'
            'build-ci/bin/NeverDSemanticTests",'
        ),
        reason=(
            "The public GitHub runner checkout path was retained from Main "
            "34631601930 attempt 1, Linux artifact 10276713971. The first "
            "commit added this regression fixture and the second normalized "
            "its checkout prefix. Review covers only that exact historical "
            "line, including its deletion; it permits no new path occurrences."
        ),
    ),
)


def is_allowed_history_line(
    path: str, rule: Rule, line: str, history_commit: str | None
) -> bool:
    # External policy rules remain authoritative even when their names match.
    return rule in RULES and any(
        a.rule == rule.name
        and history_commit in a.commits
        and path == a.path
        and (
            line == a.line
            or hashlib.sha256(line.encode("utf-8")).hexdigest() in a.line_sha256
        )
        for a in HISTORY_ALLOWED
    )


def is_allowed(path: str, rule: str) -> bool:
    return any(a.rule == rule and fnmatch(path, a.path) for a in ALLOWED)


def _run_git(arguments: Sequence[str]) -> str:
    # Git emits UTF-8.  Windows Python otherwise decodes the pipe as the
    # locale (cp1252), which cannot read a CJK architecture patch.
    return subprocess.run(
        ["git", *arguments],
        cwd=REPO_ROOT,
        check=True,
        capture_output=True,
        text=True,
        encoding="utf-8",
    ).stdout


def tracked_files() -> list[str]:
    """Return every file Git tracks, leaving submodules opaque."""

    return [entry for entry in _run_git(["ls-files", "-z"]).split("\0") if entry]


def worktree_files() -> list[str]:
    """Return tracked plus pending, non-ignored files in the worktree."""

    return sorted(
        {
            entry
            for entry in _run_git(
                ["ls-files", "--cached", "--others", "--exclude-standard", "-z"]
            ).split("\0")
            if entry
        }
    )


def _scan_text(
    name: str,
    text: str,
    rules: Sequence[Rule],
    *,
    display_name: str | None = None,
    history_commit: str | None = None,
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
            if is_allowed_history_line(name, rule, line, history_commit):
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
        return _run_git(arguments)
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
    history_commit: str | None = None,
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
                    history_commit=history_commit,
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
        # A merge commit has no default diff-tree inventory, but `git show
        # --first-parent` still prints a first-parent patch.  GitHub Actions
        # checks out that synthetic merge for pull_request jobs.  The
        # first-parent regular commits already carry the same text, so
        # scanning the merge would only re-count it and trip the inventory
        # check.
        parents = _git_output(["rev-list", "--parents", "-n", "1", commit]).split()
        if len(parents) > 2:
            continue
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
                history_commit=commit,
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
