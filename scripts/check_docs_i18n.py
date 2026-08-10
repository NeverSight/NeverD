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


def localized_paths(locale: str) -> tuple[Path, ...]:
    return (
        Path(f"docs/i18n/README.{locale}.md"),
        Path(f"docs/README.{locale}.md"),
        Path(f"docs/roadmap/README.{locale}.md"),
        Path(f"docs/testing.{locale}.md"),
        Path(f"docs/sbf.{locale}.md"),
    )


LOCALIZED_DOCS = tuple(path for locale in LOCALES for path in localized_paths(locale))
MARKDOWN_DOCS = (Path("docs/sbf.md"),) + LOCALIZED_DOCS
STAGED_ALLOWLIST = frozenset(
    {"scripts/check_docs_i18n.py", *(path.as_posix() for path in MARKDOWN_DOCS)}
)

LINK_RE = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")
HEADING_RE = re.compile(r"^(#{1,6})\s+(.+?)\s*#*\s*$")
EXPLICIT_ANCHOR_RE = re.compile(
    r"<a\s+(?:[^>]*?\s)?(?:id|name)=[\"']([^\"']+)[\"'][^>]*>",
    re.IGNORECASE,
)
FENCE_RE = re.compile(r"^\s*(`{3,}|~{3,})")


class RepositoryView:
    """Read either the working tree or the exact Git index snapshot."""

    def __init__(self, use_index: bool) -> None:
        self.use_index = use_index
        self._text_cache: dict[Path, str] = {}
        self._index_cache: dict[Path, str | None] = {}

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
            indexed = self.index_text(relative_path) if self.use_index else None
            self._text_cache[relative_path] = (
                indexed
                if indexed is not None
                else (REPO_ROOT / relative_path).read_text(encoding="utf-8")
            )
        return self._text_cache[relative_path]

    def exists(self, path: Path) -> bool:
        relative_path = self.relative(path)
        if self.use_index and self.index_text(relative_path) is not None:
            return True
        return (REPO_ROOT / relative_path).exists()


def report(errors: list[str], message: str) -> None:
    errors.append(message)


def require_tokens(
    path: Path,
    tokens: tuple[str, ...],
    errors: list[str],
    view: RepositoryView,
) -> None:
    text = view.read_text(path)
    for token in tokens:
        if token not in text:
            report(errors, f"{path}: missing required token {token!r}")


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
                report(errors, f"{relative_path}: link escapes repository: {target}")
                continue
            if not view.exists(destination_relative):
                report(errors, f"{relative_path}: missing link target: {target}")
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
                    f"{relative_path}: missing Markdown anchor {decoded_fragment!r} in "
                    f"{destination.relative_to(REPO_ROOT)}",
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
            report(errors, f"{relative_path}:{line_number}: unclosed Markdown fence")


def validate_matrix(errors: list[str], view: RepositoryView) -> None:
    for path in MARKDOWN_DOCS:
        if not view.exists(path):
            report(errors, f"missing localized documentation file: {path}")
    if errors:
        return

    selector_tokens = ("sbf.md", *(f"sbf.{locale}.md" for locale in LOCALES))
    require_tokens(Path("docs/sbf.md"), selector_tokens, errors, view)
    for locale in LOCALES:
        project_readme, index, roadmap, testing, guide = localized_paths(locale)
        require_tokens(
            project_readme,
            ("Solana SBF", "v0-v4", "--language=rust", f"../sbf.{locale}.md"),
            errors,
            view,
        )
        require_tokens(index, (f"sbf.{locale}.md",), errors, view)
        require_tokens(
            roadmap,
            ("v0-v4", "Rust", f"../sbf.{locale}.md"),
            errors,
            view,
        )
        require_tokens(
            testing,
            (
                "NeverDSBFMetadataTests",
                "NeverDSBFSemanticTests",
                "NeverDSBFIntegrationTests",
                "check-neverd-sbf",
            ),
            errors,
            view,
        )
        require_tokens(
            guide,
            (
                *selector_tokens,
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
            ),
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
    staged = frozenset(line for line in result.stdout.splitlines() if line)
    missing = sorted(STAGED_ALLOWLIST - staged)
    unexpected = sorted(staged - STAGED_ALLOWLIST)
    if missing:
        report(errors, "staged allowlist is missing: " + ", ".join(missing))
    if unexpected:
        report(errors, "staged paths are not allowed: " + ", ".join(unexpected))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check-staged",
        action="store_true",
        help="also require the Git index to contain exactly the localization change set",
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
