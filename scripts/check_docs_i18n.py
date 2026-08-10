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
GUIDE_STEMS = ("evm", "sbf")
GUIDE_REQUIRED_TOKENS = {
    "evm": (
        "frontier",
        "fusaka",
        "--language=c",
        "--language=solidity",
        "EVMOpcodes.def",
        "Instruction.def",
        "TableGen",
        "_BitInt",
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
    ),
}
ENGLISH_DOCS = (
    Path("README.md"),
    Path("docs/README.md"),
    Path("docs/roadmap/README.md"),
    Path("docs/testing.md"),
    *(Path(f"docs/{stem}.md") for stem in GUIDE_STEMS),
)


def localized_paths(locale: str) -> tuple[Path, ...]:
    return (
        Path(f"docs/i18n/README.{locale}.md"),
        Path(f"docs/README.{locale}.md"),
        Path(f"docs/roadmap/README.{locale}.md"),
        Path(f"docs/testing.{locale}.md"),
        Path(f"docs/evm.{locale}.md"),
        Path(f"docs/sbf.{locale}.md"),
    )


LOCALIZED_DOCS = tuple(path for locale in LOCALES for path in localized_paths(locale))
MARKDOWN_DOCS = ENGLISH_DOCS + LOCALIZED_DOCS
PROHIBITED_STAGED_PREFIXES = ("docs/superpowers/",)

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
                self._text_cache[relative_path] = (
                    REPO_ROOT / relative_path
                ).read_text(encoding="utf-8")
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
                )
                self._index_exists_cache[relative_path] = bool(
                    result.stdout.strip()
                )
        return self._index_exists_cache[relative_path]

    def exists(self, path: Path) -> bool:
        relative_path = self.relative(path)
        if self.use_index:
            return self.index_exists(relative_path)
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

    selector_tokens = {
        stem: (f"{stem}.md", *(f"{stem}.{locale}.md" for locale in LOCALES))
        for stem in GUIDE_STEMS
    }
    for stem in GUIDE_STEMS:
        require_tokens(
            Path(f"docs/{stem}.md"),
            (*selector_tokens[stem], *GUIDE_REQUIRED_TOKENS[stem]),
            errors,
            view,
        )

    for locale in LOCALES:
        project_readme, index, roadmap, testing, evm_guide, sbf_guide = (
            localized_paths(locale)
        )
        require_tokens(
            project_readme,
            (
                "EVM256",
                "Solana SBF",
                "v0-v4",
                "--language=solidity",
                "--language=rust",
                f"../evm.{locale}.md",
                f"../sbf.{locale}.md",
            ),
            errors,
            view,
        )
        require_tokens(
            index,
            (f"evm.{locale}.md", f"sbf.{locale}.md"),
            errors,
            view,
        )
        require_tokens(
            roadmap,
            (
                "v0-v4",
                "Solidity",
                "Rust",
                f"../evm.{locale}.md",
                f"../sbf.{locale}.md",
            ),
            errors,
            view,
        )
        require_tokens(
            testing,
            (
                "NeverDEVMOpcodeTests",
                "NeverDEVMSemanticTests",
                "NeverDEVMIntegrationTests",
                "NeverDSBFMetadataTests",
                "NeverDSBFSemanticTests",
                "NeverDSBFIntegrationTests",
                "-R 'EVM'",
                "-R 'SBF'",
            ),
            errors,
            view,
        )
        require_tokens(
            evm_guide,
            (*selector_tokens["evm"], *GUIDE_REQUIRED_TOKENS["evm"]),
            errors,
            view,
        )
        require_tokens(
            sbf_guide,
            (*selector_tokens["sbf"], *GUIDE_REQUIRED_TOKENS["sbf"]),
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
    staged = tuple(line for line in result.stdout.splitlines() if line)
    prohibited = sorted(
        path
        for path in staged
        if path.startswith(PROHIBITED_STAGED_PREFIXES)
        or "/plans/" in f"/{path}"
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
