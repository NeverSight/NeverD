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
        path="unittests/fixtures/x86/3dnow-context/README.md",
        rule="provenance-phrase",
        reason=(
            "The bootstrap attribution links NeverC's 3DNow fixture at "
            "880d1e96ff48f41c5069c6d8f9839eaa8f37d64a, credits NeverC "
            "contributors under AGPL-3.0-only, and documents the stack and "
            "CR0.NE changes. The three source headers preserve those notices; "
            "THIRD_PARTY_NOTICES.md also records the adaptation."
        ),
    ),
    Allowance(
        path="cmake/NeverDMimalloc.cmake",
        rule="provenance-phrase",
        reason=(
            "The allocator integration credits NeverC contributors and "
            "AGPL-3.0-only, with a source permalink at revision "
            "52e83efb3e3902c97130b9e3c83611080fa18ac4. "
            "THIRD_PARTY_NOTICES.md records the source file and the changes "
            "to allocator linkage, ownership and sanitizer detection."
        ),
    ),
    Allowance(
        path="lib/web/desktop/tauri/PHF.cpp",
        rule="provenance-phrase",
        reason=(
            "The adaptation notice identifies rust-siphash 0.3.10 and rust-phf "
            "0.10.1; LICENSES/phf/SOURCE.md links the exact source releases, "
            "and COPYING, LICENSE-MIT, LICENSE-APACHE and PHF-LICENSE preserve "
            "the original authors and licenses."
        ),
    ),
    Allowance(
        path="LICENSES/node/LICENSE",
        rule="provenance-phrase",
        reason=(
            "The upstream Node license names SES/Caja, Google and Agoric "
            "in its required freeze_intrinsics.js attribution."
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
        path="LICENSES/node/LICENSE",
        rule="provenance-phrase",
        reason=(
            "The verbatim Node.js license identifies SES/Caja and its copyright "
            "holder; this required upstream attribution is not a NeverD "
            "implementation claim."
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
        path="docs/evidence/x86-hle-completion-2026-11-01.json",
        rule="private-path",
        commits=frozenset({
            "0893b13b74a674fb462c35711d323a2baed7f4c2",
            "c86798cc87afb1962d01da6f70c5ae87caf93cdf",
        }),
        line="",
        reason=(
            "Reviewed normalization of published x86 qualification checkout "
            "and toolchain roots. Revisions, outcomes and artifact hashes "
            "are retained. Only these exact historical line digests in the "
            "publication and cleanup commits are covered; current paths, "
            "future commits and external policy findings remain errors."
        ),
        line_sha256=frozenset({
            "5c8e87049719b559ab9f76a98307ac2a9bba8f4032fd877af6d2ee8f104066e8",
            "6d36d025e5eec9184b091f5ee2a3252091375058ce1ffe1a7570ad81baab477e",
        }),
    ),
    HistoryAllowance(
        path="docs/evidence/x86-hyperv-hypercalls-2026-10-31.json",
        rule="private-path",
        commits=frozenset({
            "436a0797ef0ce371140ce5296c128f2adaa9a7ec",
            "c86798cc87afb1962d01da6f70c5ae87caf93cdf",
        }),
        line="",
        reason=(
            "Reviewed normalization of published x86 qualification checkout "
            "and toolchain roots. Revisions, outcomes and artifact hashes "
            "are retained. Only these exact historical line digests in the "
            "publication and cleanup commits are covered; current paths, "
            "future commits and external policy findings remain errors."
        ),
        line_sha256=frozenset({
            "0920149cb35315a6b241719976f7e3a50f4a660f837486829520a956e9577f53",
            "0c56afef280d03fb25722cbd9dddad24d4ba4b86074160a6407e700e24905f80",
            "0dede1a2eeb2fa71f5c135cc4a3362951147f963928c7a734d8de051dcde1c65",
            "121beaa1c62f50174579db9004c65b34282c3ce8a21182f34b6d35bc5ec8fa3d",
            "18d0f8c7ff16af66205886f510c36c200a59b19fb0f9ec0839fcda7c618992b7",
            "208319c7c05fe796606afbb59a19fddc54e9055d647cc2dd2de52460c414bce2",
            "2be1c29692dbd5da84b2bf44cf38a2cd28b527d199b32e2b4ad6051eb8d58f67",
            "2f06334d955a56669f762d02a25962709f5cd4f77b52c75e1bb5d85f4347e6ac",
            "38c4ab887e38f6fb342c4e30e8e6a56e5ced0a07c15b4bed83deb3ed5c65072c",
            "39563a2a74a1428dff05ddb96a276d851c32af3401632471e2dd099b5628bd88",
            "395e55fd3cffc496eb57542243952572e16fb24eecd8c231cac958ef97a6f604",
            "3bd409e41bbc9f6a63191c169f28c0d45ebadbcf1c28cc8b81b192719d39f205",
            "4246ba876527cb0942f29463e5c94b4a0f9153e4b72f6578863bb7d7770cd957",
            "4e602737ff420485effb9c0e30a451948b0faebbd5fe95e3a172403f76876f14",
            "557fbcdb4f245878cf826b3230e8bc2bcbabf46516b55f878342f11d0acfa8a3",
            "5abef32298058d3854a53963ce323aeb297d7ff642b4ef49763087f85b5475c9",
            "5d2b4e0370da3b83a082662f8d1c2569814de002eebce62a9774f039c1670373",
            "5eef0d9ac78cb60409f09693ee6da9103bbf88183d0a0329a2d6d1dd715de012",
            "6b764c06e3b58953bbb9d05217825c002436f52d90b3a3200f595b76a98a893d",
            "76d7f339f1f0cba0f4c40577c6d9351336abe22a3076896010198dcd0d0d798e",
            "7af1a5bc7395350dc75ff41e63ccbeccd6b17b0423a1eb4f62b8bc9e80251ab1",
            "8a56452716cfe7a99dad1431b1a5df6ce414645b8e09751f22cff30f2dacfa91",
            "8d5dda3f0b6282d53abd5a10bd90bdfae704539f89788ea6b9604b10564c1261",
            "8f97bc38588489655af0aad8e494d45c8da25a640f433d6001f2df62faa68735",
            "914d8a0fd2b036635a56cf2bc68671652fc2859f52ba427f0d8da9d445977203",
            "941e385605370a0112b25bf3bba39d38cf08b85296298a9202f4709634eb4f15",
            "a61a5892b459e4965c7ea6b79418a38e866b185a055f8028837d60e41c92bcc2",
            "a759d74b4cf0e740e1bad0fa53b2d29bb0e54a3347ad8d99cde0ea55bbd7aa32",
            "a778f033651d8cc8ac6319ba33b3140901504f9a3a6d0bbf426a1fae7e1c389c",
            "ab2be4490c6129473b4481380c207f138d120fbd1b3baeda8cf3a6447a8af09d",
            "ac05f847cde4880064a66e719675773148a207dc720d0b5791f7aa1f118d7791",
            "ac3994f993505dda2359adcc58f48cc350d2ce1cccce695e7ad9cf33f2fc9678",
            "b7961aabba5e47ade869200ff7e2e924394229732bf102a2bcbda80068c17298",
            "d20837610afb0f6476b1a0fe699185c9b7db068b51800c1a229c814fceaa4765",
            "d5889650fe535eb0a3e8d02fc4c94feaf1e03fe11f00f45cd37c141128a38fa4",
            "d5a5b40003dc069a70820a4e514ea47e3fe33ffcf6770f2441fb57cc34c321b8",
            "d6e7bef94f6ad02472b0de2e3fddd65e8816c25e9c4f9c822872911cf25dc9e6",
            "dae31b1452e5a5dc075f9e4201ac993cea5d3b31531fa2e3b33f0c3cb9bde614",
            "dbcbe62c2646375c55b24c1beb9c31eec3d61b14bd9f1d6e223d67e9fb6c0157",
            "f12953a9c5f93a51a463c0ef28308d2e37ac6796e0668abffb059fce09ba19b7",
            "f19b08ecb47dad559207203231f9bad560f145ef728a129a1b9e2b7b06bf4afa",
            "f20bc68d41580bd1a41c5b16d1ce7bffc0368dcf1ec500043ad906bdaee7337d",
            "fbacec7393016727b4157eb34aa321b0b061803993ee4a5b036867b44a097d76",
        }),
    ),
    HistoryAllowance(
        path="docs/evidence/x86-rtm-abort-pair-2026-11-01.json",
        rule="private-path",
        commits=frozenset({
            "c86798cc87afb1962d01da6f70c5ae87caf93cdf",
            "d7f588127eb57b0e30adf02ee24b50e44c83dc37",
        }),
        line="",
        reason=(
            "Reviewed normalization of published x86 qualification checkout "
            "and toolchain roots. Revisions, outcomes and artifact hashes "
            "are retained. Only these exact historical line digests in the "
            "publication and cleanup commits are covered; current paths, "
            "future commits and external policy findings remain errors."
        ),
        line_sha256=frozenset({
            "6b764c06e3b58953bbb9d05217825c002436f52d90b3a3200f595b76a98a893d",
            "d5a5b40003dc069a70820a4e514ea47e3fe33ffcf6770f2441fb57cc34c321b8",
        }),
    ),
    HistoryAllowance(
        path="docs/evidence/x86-snp-completion-2026-10-31.json",
        rule="private-path",
        commits=frozenset({
            "738bced979e11c5b94a5cfb2d292f7309f8777bf",
            "c86798cc87afb1962d01da6f70c5ae87caf93cdf",
        }),
        line="",
        reason=(
            "Reviewed normalization of published x86 qualification checkout "
            "and toolchain roots. Revisions, outcomes and artifact hashes "
            "are retained. Only these exact historical line digests in the "
            "publication and cleanup commits are covered; current paths, "
            "future commits and external policy findings remain errors."
        ),
        line_sha256=frozenset({
            "5c8e87049719b559ab9f76a98307ac2a9bba8f4032fd877af6d2ee8f104066e8",
            "6d36d025e5eec9184b091f5ee2a3252091375058ce1ffe1a7570ad81baab477e",
        }),
    ),
    HistoryAllowance(
        path="docs/evidence/x86-kvm-hypercalls-2026-10-31.json",
        rule="private-path",
        commits=frozenset({
            "4abbf80edbfaf5f8c0f1f7896d224226ba2411c2",
            "fc4a38d3a78a9c60a67a95aea3427a587fa5bd65",
        }),
        line="",
        reason=(
            "Reviewed normalization of local checkout and toolchain roots "
            "in this published x86 qualification record. Revisions, test "
            "results and artifact/tool digests are unchanged. Only these "
            "exact historical line digests in the publication and cleanup "
            "commits are covered; current and future paths, adjacent lines "
            "and external policy findings remain errors."
        ),
        line_sha256=frozenset({
            "2573ba3bb2890099e8cb8f3a5b91f6459eebb3b2a1d9a237266b4c36a0d084ae",
            "2d72bac9414776668cec1066f196cabaff0090fe212b01486e226dbb69f849c7",
            "4e602737ff420485effb9c0e30a451948b0faebbd5fe95e3a172403f76876f14",
            "6b764c06e3b58953bbb9d05217825c002436f52d90b3a3200f595b76a98a893d",
            "6fa080b11f174cdd9678ff4879eef9d9ddcc42001993b5a320cf30b74be77ace",
            "d5a5b40003dc069a70820a4e514ea47e3fe33ffcf6770f2441fb57cc34c321b8",
            "dbcbe62c2646375c55b24c1beb9c31eec3d61b14bd9f1d6e223d67e9fb6c0157",
        }),
    ),
    HistoryAllowance(
        path="docs/evidence/x86-mpx-source-2026-10-31.json",
        rule="private-path",
        commits=frozenset({
            "c144bc42eaa233b2a70bb42bc15ce6b243979b16",
            "fc4a38d3a78a9c60a67a95aea3427a587fa5bd65",
        }),
        line="",
        reason=(
            "Reviewed normalization of local checkout and toolchain roots "
            "in this published x86 qualification record. Revisions, test "
            "results and artifact/tool digests are unchanged. Only these "
            "exact historical line digests in the publication and cleanup "
            "commits are covered; current and future paths, adjacent lines "
            "and external policy findings remain errors."
        ),
        line_sha256=frozenset({
            "05bb87e59655375e56f686cfd13dc59d20803819c97c9ca22a37065d9a420866",
            "5bf34f2402001678e27bad3bf7a168a8c79e33f4f2f86c1faa3cc097bd4920e3",
            "6fa080b11f174cdd9678ff4879eef9d9ddcc42001993b5a320cf30b74be77ace",
            "919b8fbfc43de170154e17f27963e5d15172a3da3f9efce28e0456434e9c812a",
            "d4fce1536ae292cf7f33061cd270852f26aaa04b1d9a9557aa980dc8dc4ce2e1",
        }),
    ),
    HistoryAllowance(
        path="docs/evidence/x86-amx-stack-memory-2026-10-30.json",
        rule="private-path",
        commits=frozenset({
            "3155bc4b3aa9b01d967e5cf84dfd8b6e565105ae",
            "6a41c17f2406e5d4d36f98fdfd931b4aa715eb95",
            "9b76b90e84b9d3c394614a11f009b77a5837bb78",
        }),
        line="",
        reason=(
            "Reviewed normalization of developer checkout and toolchain path "
            "prefixes in this published native qualification record. Test "
            "results, identities, limits, versions and artifact/tool digests "
            "are unchanged. Only these exact historical line digests in the "
            "listed full commits are covered; current and future paths, "
            "adjacent lines and external policy findings remain errors."
        ),
        line_sha256=frozenset({
            "0ea62b71d20a48aa7c01db59b431ee865d6cc9a54b9ebaaf894a17a484de36f9",
            "23170ca6b62d4390b959e0431b6dfa302a3ac3aa08ebffde5a224c9d93d89421",
            "23e7f13343122463601d6e357f4878177b9190d22dcfeeb3d3281e813321e7e5",
            "28734656d5f15df9c1dd53cec465b6903fd5fb8a745a0a47d30b671f580e3d32",
            "336d0c5cff693604166c5093067c19a4bd169482d8146c89169e9c2aa9d7d197",
            "3c4dc75cdc091c92e8ea3780f43ad0f8c8beb6d70a19043d326ac0ab415f7e69",
            "6c98d0e5da4f1ac1a9917e145f31219d0ad800858dc141e62943fd961f27d23e",
            "bfa7df2f4449cd692cc039d09d826700d8b62baa4cc416de8ec4e3dcc66cbff7",
            "fe6a9ca47324e8c9c4699f2451f7126084698f47c60500a9d4a3af91a3ef1778",
        }),
    ),
    HistoryAllowance(
        path="docs/evidence/x86-mpx-native-frames-2026-10-30.json",
        rule="private-path",
        commits=frozenset({
            "3155bc4b3aa9b01d967e5cf84dfd8b6e565105ae",
            "77bf33f5afb80e0e5aba86e661ed8a8337e0fcce",
            "8d07f9a4f7e5aa974863654607789f79fd39b4b6",
        }),
        line="",
        reason=(
            "Reviewed normalization of developer checkout and toolchain path "
            "prefixes in this published native qualification record. Test "
            "results, identities, limits, versions and artifact/tool digests "
            "are unchanged. Only these exact historical line digests in the "
            "listed full commits are covered; current and future paths, "
            "adjacent lines and external policy findings remain errors."
        ),
        line_sha256=frozenset({
            "23e7f13343122463601d6e357f4878177b9190d22dcfeeb3d3281e813321e7e5",
            "3c4dc75cdc091c92e8ea3780f43ad0f8c8beb6d70a19043d326ac0ab415f7e69",
            "4e1abb563a5577ee35a07d95c420e83208a77b1a10105d694dac527d345c3baf",
            "842b77aaae2a41ae4ad2fe2aabe4d40ddb091eab05db7ff62d6ad0282818ffea",
            "8b452a917e9acc76377f1f238d80b3bfc6c7fe9cbf45f6d93304901864937def",
            "bfa7df2f4449cd692cc039d09d826700d8b62baa4cc416de8ec4e3dcc66cbff7",
            "e06f85a7cc73a6fd8faedcfa1a9a04001bdf3c9190d414d0346d27de23e98518",
            "fe6a9ca47324e8c9c4699f2451f7126084698f47c60500a9d4a3af91a3ef1778",
        }),
    ),
    HistoryAllowance(
        path="docs/evidence/x86-vmrun-context-2026-10-30.json",
        rule="private-path",
        commits=frozenset({
            "185bcabd0e9a6239ba9708d42320a2f84d445076",
            "3155bc4b3aa9b01d967e5cf84dfd8b6e565105ae",
            "31c1e124172811f74816398d609596af64dad2a9",
        }),
        line="",
        reason=(
            "Reviewed normalization of developer checkout and toolchain path "
            "prefixes in this published native qualification record. Test "
            "results, identities, limits, versions and artifact/tool digests "
            "are unchanged. Only these exact historical line digests in the "
            "listed full commits are covered; current and future paths, "
            "adjacent lines and external policy findings remain errors."
        ),
        line_sha256=frozenset({
            "b76f53de9ee2b795afd98699509cd58f3b1e1c16ff6f3607da74c908565236f8",
            "d4fce1536ae292cf7f33061cd270852f26aaa04b1d9a9557aa980dc8dc4ce2e1",
            "ed9a038683f08567222d5d0bca8df6b4142b11ab3c5e675f20a0d96d75ec2de0",
        }),
    ),
    HistoryAllowance(
        path="docs/evidence/x86-vmrun-source-2026-10-31.json",
        rule="private-path",
        commits=frozenset({
            "3155bc4b3aa9b01d967e5cf84dfd8b6e565105ae",
            "8e977533a4f6913ab1f9e9aa7a4518654500bbcf",
        }),
        line="",
        reason=(
            "Reviewed normalization of developer checkout and toolchain path "
            "prefixes in this published native qualification record. Test "
            "results, identities, limits, versions and artifact/tool digests "
            "are unchanged. Only these exact historical line digests in the "
            "listed full commits are covered; current and future paths, "
            "adjacent lines and external policy findings remain errors."
        ),
        line_sha256=frozenset({
            "046ce6593a8ac20556f4692ba9f1d1abfb16dccd1c62698a5ef895ad680b48c2",
            "4c257a87e813b6fee570215e1361fa0570807041028df0d351d369281633346b",
            "b76f53de9ee2b795afd98699509cd58f3b1e1c16ff6f3607da74c908565236f8",
            "cba940d0e6734b7f3bb1b6b7c631ba9a1519ab872ed1cbd032eb831b27ff0b5b",
            "d4fce1536ae292cf7f33061cd270852f26aaa04b1d9a9557aa980dc8dc4ce2e1",
            "f747cdd7279b903b77037c432db19eeff39bb9ae9f1cf17a761a172446bec630",
            "ff8e0454a6bfce3cb4a32c6f0cb0688f93d11d655fa4484fa3688d30fdff4869",
        }),
    ),
    HistoryAllowance(
        path="docs/evidence/arm64e-typed-call-abi-2026-10-28.json",
        rule="private-path",
        commits=frozenset({
            "64a8b64a1d54543d27524c31aed896b89acb0fd6",
            "16e426cb58317ec165b80ae3119c8e1d54901bf0",
        }),
        line="",
        reason=(
            "Reviewed removal of checkout prefixes from twelve arm64e retry "
            "command arguments. Test identities, outcomes, limits, timings and "
            "artifact hashes are unchanged. Only these exact historical line "
            "digests in the publication and cleanup commits are covered; "
            "current and future private paths remain findings."
        ),
        line_sha256=frozenset({
            "170cdc8c9dc54ea4f66ba902295daba256b38d72ef1d0fa377f2298be5c21344",
            "376438c318647fa4451e6df22d7876aa23fe17932d41c4570ab0311f9f6215d1",
            "3a2d102b3ea98311af5243a8293b6aa439a41c9c2440f77fdc388a0024c601cc",
            "611378506823a6b25f13f298d47f8d3cd1ef877407b845250bba58f8669a1c32",
            "bddcd1df1e73de0db4e1c59b9517be44390737cacc478437b85169a0fa6f9254",
            "df2f2c3fc5b2eb257259e60019f35709f8f5a18980319750ce3a5bd0e1d684ef",
            "ef5eb909a1791810c74b4f0de1fc3df79fea8f8a7afb280647cf6288c957bc17",
            "ff632c8149ef79e2c4cbdf8292ab0f7f78816cbe03f0d71e8bcf8d8928418914",
        }),
    ),
    HistoryAllowance(
        path="docs/evidence/x86-skinit-2026-10-27.json",
        rule="private-path",
        commits=frozenset({
            "89fd592fae65cc3d6011a386814543ad60270e40",
            "f567cd547e7aa8ed5702791ad9a17642a547d3ee",
        }),
        line="",
        reason=(
            "Reviewed removal of checkout prefixes from SKINIT skipped-case "
            "messages. Source locations, skip reasons, test outcomes and "
            "artifact hashes are unchanged. Only these exact historical "
            "line digests in the publication and cleanup commits are covered; "
            "current and future private paths remain findings."
        ),
        line_sha256=frozenset({
            "1616dea1c69c35a2c2b63af91ee86b047aa38403249e317318800b79fb788dcb",
            "1f6208a2e5eff91636c09dc93852387008f817c0c6e279f4747cf6bc6ce22ad2",
            "3220ef15c634898edf30928b12872cf84cbeb8d14459b4a86ebb8cbb2db9ca4b",
            "42167ebeecac407625a1c7f3553ac6df792c634b36bedfcdfe4aa30c6436e605",
            "49f895b9427262357e4b77780394489705a41eaf5a033ee9ad02ab5800b4fe37",
            "4c41031d7b3885420d02096e6796b51e83283ce8e31c4c215a122b1f0ff8b7e3",
            "552befd583b5ebf452743f5e9bccc2c9119680ecb8cb4eddfe1e8a188aa8d5fd",
            "5cdb7fc8338a02c60cda0a23c91131b522b53d77af58ae2c48f8a9e9c77bc090",
            "6816e320eaf388a54f0dc56fc2aea6369050ef2455a1304be460d38b8755818d",
            "7535687e00d4001bb82b56bdaff94c9c0fdb4cdd81600eff08c1936c9fd44e20",
            "77dc532743e5e997eb0d97ba226da3434bcdc7be90f4dffd10680a3d3925b13a",
            "93efac8e567e28f2d2f4da25f06838fdb94ab045f39c825151c07db97ee9dc67",
            "940189e4783a0423129632ca8c2c1aa575a2e179daab23dc0bc995305a071d34",
            "955343cf2e9a4e7e73eb97d2aaede111afe2bda529b003cbac31426227e25242",
            "9c42b609f7a473c24e69b7c185b35c64065edcb6f2e0230a2b9dbeb803cfe301",
            "9d1b2c151ea34bd138622651ecf0bd3099b18effdd47b23aa28d6c822ddf4c70",
            "9e315de9fdce824ffe35b6fc697b7f1a5b05d33cfc8e61e061ec12765119cd4f",
            "a04cb518faa72299b766eb950cb65b23be55c6d2acedf0afaf8c0af5f36f646a",
            "a57098a114bdba70e2b83bc5cc63871055efe91507224c8f0d836ce509af2747",
            "aff779e43b1ff16b6e8d4c08b828f3593d1fc7893726b646b17689a8754e0dc4",
            "b27a9918016de3ebafd65e3ecb45f049d6a7e20c31e47d76b555cf7d80446a26",
            "bd8b3e465fd9b677f54dc0c6bbcc64c0f6b37f2bc8bf16e6bf7cfa4c2271b861",
            "beaae86f84f08af106a0f8fc17705338e957bb545eacb555e89a8a99bec0c257",
            "cf4e718d01cbe113d54036d03b41fe4a6c16ddfd32e8af326945e2647b916b0e",
            "d66899d7df5977d965abe712498269b9defecba85bed1101fa89f45ac02df9c6",
            "f8237aaf336d79fdafedfd3237f6d6fa1b66aa77122b8fc3605de38808ab0294",
            "fac973982fbbab41f04ab3909f478956abe409e88bc1b986642e5eabae4a55b6",
        }),
    ),
    HistoryAllowance(
        path="docs/evidence/x86-atomic-segments-2026-10-27.json",
        rule="private-path",
        commits=frozenset({
            "b961f875158f1ab303339ca0cad08ac8300a3085",
            "038d6dea6ba70c4485a91a51dff2accd1c0c8dd3",
        }),
        line="",
        reason=(
            "Reviewed removal of the local RAO verification script path in "
            "two independently published cleanup commits. Only this exact "
            "deleted line digest is covered; the current document identifies "
            "the local-only oracle without a machine path. Current and future "
            "private paths remain findings."
        ),
        line_sha256=frozenset({
            "d826a809d43af9633a38c2c60bdaaa6af67f8cc9790c6bdb3d04b98a84049157",
        }),
    ),
    HistoryAllowance(
        path="docs/evidence/arm64e-raw-sp-memory-2026-10-27.json",
        rule="private-path",
        commits=frozenset({
            "85900e61461ed155a102148df6cddb682f7834b4",
            "ec76b47c855209573e6e6a6f972af2c7c5749311",
        }),
        line="",
        reason=(
            "Reviewed removal of developer checkout prefixes from six arm64e "
            "skipped-case outputs, including the identical cherry-pick. Only "
            "exact deleted line digests in these cleanup commits are covered. "
            "Test outcomes, timings and artifact hashes are unchanged; current "
            "and future private paths remain findings."
        ),
        line_sha256=frozenset({
            "0595402c250fe956e45823809ba272953422559436c8ca711148d49bb6b6f58f",
            "2245215fc74effce3d47cbd2f87ecd822e22e8741215e103155ff9f0ef93452d",
            "697a5b27da22d738d44e4cef7dd7f903dc8e7f30085d4271894ef998f60284b0",
            "6b1c07f536c8584bd39145bbd7f7fbd315742348b10b8f9fc1ff90e273d47953",
            "bb7f915de1f2a18f191a40ffb4f5292a5e401394ecd987dff4809e49adb3fa22",
            "d29c8ee4b68292d61a4ba827d10f0c39bd431a2d22b78ecba997172a536bf08c",
        }),
    ),
    HistoryAllowance(
        path="docs/evidence/patch-scaling-2026-10-25.json",
        rule="private-path",
        commits=frozenset({
            "6e662ff3f1b4b3c6e457c11b2580392dc4e4511f",
            "5c153451f79ed2161c16405f7db377263dcbdeb9",
        }),
        line="",
        reason=(
            "Reviewed normalization of published delivery command checkout "
            "prefixes. Repository-relative filenames, outcomes, timings and "
            "artifact hashes are retained. Only the exact publication and "
            "cleanup commits and line digests are covered; current and future "
            "private paths remain findings."
        ),
        line_sha256=frozenset({
            "7cefd0889458e4fabb7baeeece0756eae7fe277fdb911658dcc0fe0722b27fbe",
            "d3ada58b4f339865b8016cda1ac1fc42c4a4b64bf19fa24c84ac0f62293230db",
        }),
    ),
    HistoryAllowance(
        path="docs/evidence/patch-history-2026-10-25.json",
        rule="private-path",
        commits=frozenset({"2388152e9b28a5b613b55e2897be2699950c0ece"}),
        line="",
        reason=(
            "Reviewed deletion of checkout-specific benchmark command paths. "
            "The cleanup uses the current checkout and relative build paths; "
            "measurements and artifact hashes are unchanged. Only exact removed "
            "line digests in this published cleanup commit are covered. "
            "Current source and future occurrences remain forbidden."
        ),
        line_sha256=frozenset({
            "08d11975c9bcb42f8c9c815588e1b6733c25c07dd7a969fec05fe8edfaae45f2",
            "7cefd0889458e4fabb7baeeece0756eae7fe277fdb911658dcc0fe0722b27fbe",
            "d3ada58b4f339865b8016cda1ac1fc42c4a4b64bf19fa24c84ac0f62293230db",
        }),
    ),
    HistoryAllowance(
        path="docs/evidence/patch-scaling-2026-10-25.json",
        rule="private-path",
        commits=frozenset({"2388152e9b28a5b613b55e2897be2699950c0ece"}),
        line="",
        reason=(
            "Reviewed deletion of checkout-specific delivered-test paths. "
            "The cleanup retains command arguments, measurements and hashes "
            "while making build paths relative. Only the exact removed line "
            "digests in this published cleanup commit are covered; current "
            "source and future occurrences remain forbidden."
        ),
        line_sha256=frozenset({
            "7cefd0889458e4fabb7baeeece0756eae7fe277fdb911658dcc0fe0722b27fbe",
            "d3ada58b4f339865b8016cda1ac1fc42c4a4b64bf19fa24c84ac0f62293230db",
        }),
    ),
    HistoryAllowance(
        path="docs/web-analysis-implementation.md",
        rule="private-path",
        commits=frozenset({
            "2a5d6f60dc231c1e2c90516e762a1a407ae78ce9",
            "f5c1f1c145767f8da374c3a0449076ea00200cea",
            "3274140f8d21928e1c7c930203472762c5cab9ce",
        }),
        line="",
        reason=(
            "Reviewed removal of a local evidence directory from the issue "
            "closure notes. The exact line digest and introduction/removal "
            "commits record the cleanup without republishing the path. "
            "Current files and future occurrences remain forbidden."
        ),
        line_sha256=frozenset({
            "90101851e50b6c41cdb14014f7669182fa5deef52e4d6214bba21b6be517581e",
        }),
    ),
    HistoryAllowance(
        path="docs/web-claude-code-qualification.md",
        rule="private-path",
        commits=frozenset({
            "4ccd8a4df593ffcfc8b4001f06b3e8df307a4207",
            "8a1a303a0924fc4a0b918fd32b59d00b48d09e36",
        }),
        line="",
        reason=(
            "Reviewed historical qualification artifact paths. The current "
            "document uses a local-recovery placeholder while retaining the "
            "relative filenames, source hashes and observations. Exact line "
            "digests acknowledge only these published commits without "
            "republishing private prefixes or permitting future occurrences."
        ),
        line_sha256=frozenset({
            "626ab8b1c1b594ca2508f7f8d082bb6691e8daa30a32515ff67859d24afaa17d",
            "d37d85569c24cd796a4ff011da73c1d686836c3aba580a5234249e169092264b",
        }),
    ),
    HistoryAllowance(
        path="docs/web-claude-code-qualification.md",
        rule="private-path",
        commits=frozenset({
            "500c51062c3aba4e2251b8f3c76569f058fd3ef7",
            "ec58750a8ed49319af857f5cb1aa77670810e39e",
            "d729f6d64b3ee497f020a271176fe833b779b847",
            "9b4a32c6249a4f690394c6797c5f4dce9bc81ef9",
        }),
        line="",
        reason=(
            "Reviewed replacement of a developer-specific output directory "
            "with its portable recovery-workspace location. Artifact names, "
            "measurements and hashes are preserved. Only the exact original "
            "and deletion commits and line digest are covered; current and "
            "future private paths remain findings."
        ),
        line_sha256=frozenset({
            "c39893ef1f25d39cb6d25ade741abae3b95c601f77f3f228a02321667b4952f0",
        }),
    ),
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
    HistoryAllowance(
        path="docs/evidence/x86-x87-environment-2026-10-28.json",
        rule="private-path",
        commits=frozenset({
            "2880984410f70f32505cd0b147818e14d18c43ac",
            "579734bbe2a48b79784aefee2ce83e540f10bfaa",
        }),
        line="",
        reason=(
            "Reviewed normalization of the published NeverC executable path "
            "relative to its pinned checkout. Source revisions, test results "
            "and artifact hashes are unchanged. Only this exact line digest "
            "in the publication and cleanup commits is covered; current and "
            "future private paths remain findings."
        ),
        line_sha256=frozenset({
            "8507f9dd32a05243f4878770a6f932d5712d9cd9f705846a506ecd83482cdbdd",
        }),
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
            ["diff", "--name-only", "--no-renames", "--ignore-submodules=all",
             "-z", "HEAD", "--"]
        ).split("\0")
        if entry
    ]
    return _scan_patch_set(
        names,
        _git_output(
            ["diff", "--no-ext-diff", "--no-renames", "--ignore-submodules=all",
             "--unified=0", "HEAD", "--"]
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
                    "--ignore-submodules=all",
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
                        "--ignore-submodules=all",
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
