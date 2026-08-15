#!/usr/bin/env python3
"""Audit a complete CTest JSON inventory and select a CI execution profile."""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


SEMANTIC_LABEL = "NeverDSemanticTests"
PATCH_LABEL = "NeverDPatchFullTests"
# The suites backed by the pinned `testbins` submodule.  Each is built only
# when the configure step was told to look for the corpus, and each reads
# several hundred real binaries that nothing else in the tree covers.
CORPUS_LABELS = (
    "NeverDAdaDEHCorpusTests",
    "NeverDCxxItaniumEHCorpusTests",
    "NeverDGoEHCorpusTests",
    "NeverDObjCEHCorpusTests",
    "NeverDRustEHCorpusTests",
    "NeverDWindowsEHCorpusTests",
)
PROFILE_EXCLUSIONS = {
    "linux-semantic": r"^NeverDPatchFullTests$",
    "macos-patch": r"^NeverDSemanticTests$",
    "windows-focused": r"^NeverD(Semantic|PatchFull)Tests$",
}
PROFILE_HEAVY_OWNERS = {
    "linux-semantic": frozenset({SEMANTIC_LABEL}),
    "macos-patch": frozenset({PATCH_LABEL}),
    "windows-focused": frozenset(),
}


class InventoryError(ValueError):
    """Raised when discovery or profile invariants are violated."""


@dataclass(frozen=True)
class TestRecord:
    name: str
    labels: frozenset[str]


@dataclass(frozen=True)
class AuditResult:
    profile: str
    exclude_label_regex: str
    full_count: int
    semantic_count: int
    patch_count: int
    selected_count: int
    excluded_count: int
    selected_names: tuple[str, ...]


def _parse_test(index: int, raw_test: object) -> TestRecord:
    if not isinstance(raw_test, dict):
        raise InventoryError(f"test entry {index} is not an object")

    name = raw_test.get("name")
    if not isinstance(name, str) or not name:
        raise InventoryError(f"test entry {index} has no non-empty name")

    properties = raw_test.get("properties", [])
    if not isinstance(properties, list):
        raise InventoryError(f"test {name!r} properties are not a list")

    labels: set[str] = set()
    for prop in properties:
        if not isinstance(prop, dict):
            raise InventoryError(f"test {name!r} has a malformed property")
        if prop.get("name") != "LABELS":
            continue
        value = prop.get("value")
        if not isinstance(value, list) or any(
            not isinstance(label, str) or not label for label in value
        ):
            raise InventoryError(f"test {name!r} has malformed LABELS")
        labels.update(value)
    return TestRecord(name=name, labels=frozenset(labels))


def parse_inventory(document: object) -> tuple[TestRecord, ...]:
    if not isinstance(document, dict) or document.get("kind") != "ctestInfo":
        raise InventoryError("input is not CTest json-v1 ctestInfo")
    version = document.get("version")
    if not isinstance(version, dict) or version.get("major") != 1:
        raise InventoryError("unsupported CTest JSON version")
    raw_tests = document.get("tests")
    if not isinstance(raw_tests, list):
        raise InventoryError("CTest JSON has no tests list")

    records = tuple(_parse_test(index, test) for index, test in enumerate(raw_tests))
    if not records:
        raise InventoryError("CTest discovered zero tests")
    return records


def audit_inventory(
    document: object,
    profile: str,
    exclude_label_regex: str,
    *,
    semantic_minimum: int = 20_000,
    patch_minimum: int = 22_000,
) -> AuditResult:
    expected_expression = PROFILE_EXCLUSIONS.get(profile)
    if expected_expression is None:
        raise InventoryError(f"unknown CI test profile: {profile}")
    if exclude_label_regex != expected_expression:
        raise InventoryError(
            f"exclusion {exclude_label_regex!r} does not match profile {profile!r}; "
            f"expected {expected_expression!r}"
        )

    records = parse_inventory(document)
    names = [record.name for record in records]
    # Focused binaries deliberately compile the same TU as a large suite so
    # one host can skip the suite and still run the cases.  Identity is
    # therefore (name, labels), not the gtest name alone.
    identities = [(record.name, record.labels) for record in records]
    duplicates = sorted(
        name
        for (name, _labels), count in Counter(identities).items()
        if count > 1
    )
    if duplicates:
        preview = ", ".join(repr(name) for name in duplicates[:10])
        suffix = " ..." if len(duplicates) > 10 else ""
        raise InventoryError(f"duplicate CTest names: {preview}{suffix}")

    not_built = sorted(name for name in names if name.endswith("_NOT_BUILT"))
    if not_built:
        preview = ", ".join(repr(name) for name in not_built[:10])
        raise InventoryError(f"CTest targets are NOT_BUILT: {preview}")

    # A corpus suite exists only where the configure step was told to look for
    # the submodule, and nothing else fails when it was not: every remaining
    # test still passes, and several hundred pinned binaries stop being read.
    # That is a regression no test can catch, because the test is the thing
    # that went missing, so the inventory has to insist on it.
    present_labels = {label for record in records for label in record.labels}
    absent = [label for label in CORPUS_LABELS if label not in present_labels]
    if absent:
        raise InventoryError(
            "the pinned binary corpus is not under test: "
            + ", ".join(absent)
            + "; configure with -DNEVERD_ENABLE_BINARY_CORPUS_TESTS=ON and "
            "check out the unittests/corpus submodule"
        )

    semantic_names = {
        record.name for record in records if SEMANTIC_LABEL in record.labels
    }
    patch_names = {record.name for record in records if PATCH_LABEL in record.labels}
    if len(semantic_names) < semantic_minimum:
        raise InventoryError(
            f"semantic inventory {len(semantic_names)} is below minimum "
            f"{semantic_minimum}"
        )
    if len(patch_names) < patch_minimum:
        raise InventoryError(
            f"patch inventory {len(patch_names)} is below minimum {patch_minimum}"
        )

    overlap = sorted(semantic_names & patch_names)
    if overlap:
        preview = ", ".join(repr(name) for name in overlap[:10])
        raise InventoryError(
            f"semantic and patch owner labels overlap: {preview}"
        )

    excluded_pattern = re.compile(exclude_label_regex)
    selected = tuple(
        record
        for record in records
        if not any(excluded_pattern.search(label) for label in record.labels)
    )
    selected_names = tuple(sorted(record.name for record in selected))
    if not selected:
        raise InventoryError(f"profile {profile!r} selected zero tests")

    heavy_sets = {
        SEMANTIC_LABEL: semantic_names,
        PATCH_LABEL: patch_names,
    }
    expected_owners = PROFILE_HEAVY_OWNERS[profile]
    for label, owned_names in heavy_sets.items():
        selected_owned = {record.name for record in selected if label in record.labels}
        if label in expected_owners and selected_owned != owned_names:
            raise InventoryError(f"profile {profile!r} does not select all of {label}")
        if label not in expected_owners and selected_owned:
            raise InventoryError(f"profile {profile!r} unexpectedly selects {label}")

    selected_set = set(selected)
    excluded = {record for record in records if record not in selected_set}
    expression_excluded = {
        record
        for record in records
        if any(excluded_pattern.search(label) for label in record.labels)
    }
    if excluded != expression_excluded:
        raise InventoryError("selected inventory is not the exact label set difference")

    return AuditResult(
        profile=profile,
        exclude_label_regex=exclude_label_regex,
        full_count=len(records),
        semantic_count=len(semantic_names),
        patch_count=len(patch_names),
        selected_count=len(selected),
        excluded_count=len(excluded),
        selected_names=selected_names,
    )


def _append(path: Path | None, text: str) -> None:
    if path is None:
        return
    with path.open("a", encoding="utf-8", newline="\n") as stream:
        stream.write(text)


def write_github_reports(
    result: AuditResult,
    matrix_name: str,
    *,
    output_path: Path | None,
    summary_path: Path | None,
) -> str:
    outputs = "".join(
        (
            f"count={result.selected_count}\n",
            f"full_count={result.full_count}\n",
            f"semantic_count={result.semantic_count}\n",
            f"patch_count={result.patch_count}\n",
            f"excluded_count={result.excluded_count}\n",
            f"profile={result.profile}\n",
            f"label_exclude={result.exclude_label_regex}\n",
        )
    )
    owner_labels = PROFILE_HEAVY_OWNERS[result.profile]
    owner = ", ".join(sorted(owner_labels)) if owner_labels else "focused suites only"
    summary = (
        f"### {matrix_name}\n\n"
        f"Profile: `{result.profile}`; heavy owner: `{owner}`; "
        f"excluded labels: `{result.exclude_label_regex}`.\n\n"
        "| Inventory | Tests |\n"
        "|---|---:|\n"
        f"| Full discovered | {result.full_count} |\n"
        f"| Semantic | {result.semantic_count} |\n"
        f"| Patch full | {result.patch_count} |\n"
        f"| Excluded | {result.excluded_count} |\n"
        f"| Selected | {result.selected_count} |\n"
    )
    _append(output_path, outputs)
    _append(summary_path, summary)
    return summary


def _github_path(variable: str) -> Path | None:
    value = os.environ.get(variable)
    return Path(value) if value else None


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=sorted(PROFILE_EXCLUSIONS), required=True)
    parser.add_argument("--exclude-label-regex", required=True)
    parser.add_argument("--matrix-name", required=True)
    args = parser.parse_args(argv)

    try:
        document = json.load(sys.stdin)
        result = audit_inventory(
            document,
            args.profile,
            args.exclude_label_regex,
        )
    except (json.JSONDecodeError, InventoryError, re.error) as error:
        print(f"CTest inventory audit failed: {error}", file=sys.stderr)
        return 1

    summary = write_github_reports(
        result,
        args.matrix_name,
        output_path=_github_path("GITHUB_OUTPUT"),
        summary_path=_github_path("GITHUB_STEP_SUMMARY"),
    )
    print(summary, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
