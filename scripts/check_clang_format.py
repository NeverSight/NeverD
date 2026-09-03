#!/usr/bin/env python3

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any, Callable, Sequence, TextIO


SOURCE_ROOTS = (
    "include",
    "lib",
    "plugins",
    "pluginsdk",
    "tools",
    "unittests",
)
SOURCE_SUFFIXES = frozenset(
    {
        ".c",
        ".cc",
        ".cpp",
        ".cxx",
        ".def",
        ".h",
        ".hh",
        ".hpp",
        ".hxx",
        ".inc",
        ".inl",
        ".tcc",
    }
)
PINNED_CLANG_FORMAT_VERSION = "22.1.2"
FORMAT_BATCH_SIZE = 64


@dataclass(frozen=True)
class ExecutionContext:
    root: Path
    formatter: str
    run_command: Callable[..., Any]
    stderr: TextIO


def is_first_party_source(path: str) -> bool:
    candidate = PurePosixPath(path)
    return (
        len(candidate.parts) > 1
        and candidate.parts[0] in SOURCE_ROOTS
        and candidate.suffix.lower() in SOURCE_SUFFIXES
    )


def parse_paths(output: bytes) -> list[str]:
    paths = (
        raw_path.decode("utf-8", errors="surrogateescape")
        for raw_path in output.split(b"\0")
        if raw_path
    )
    return sorted(path for path in paths if is_first_party_source(path))


def create_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Check changed first-party C/C++ files with clang-format."
    )
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--all", action="store_true", help="check all tracked files")
    selection.add_argument("--base", help="base Git revision for a changed-file check")
    parser.add_argument("--head", default="HEAD", help="head Git revision")
    parser.add_argument(
        "--clang-format",
        default="clang-format",
        help="clang-format executable to invoke",
    )
    return parser


def create_git_command(arguments: argparse.Namespace) -> list[str]:
    if arguments.all:
        return ["git", "ls-files", "-z", "--", *SOURCE_ROOTS]

    base = arguments.base
    if not base or not base.strip("0"):
        base = f"{arguments.head}^"
    return [
        "git",
        "diff",
        "--name-only",
        "--diff-filter=ACMR",
        "-z",
        base,
        arguments.head,
        "--",
        *SOURCE_ROOTS,
    ]


def discover_paths(
    arguments: argparse.Namespace, context: ExecutionContext
) -> list[str] | None:
    discovered = context.run_command(
        create_git_command(arguments),
        cwd=context.root,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if discovered.returncode == 0:
        return parse_paths(discovered.stdout)

    detail = discovered.stderr.decode("utf-8", errors="replace").strip()
    print(
        f"clang-format check: Git path discovery failed: {detail}",
        file=context.stderr,
    )
    return None


def run_formatter(
    arguments: list[str], context: ExecutionContext, operation: str
) -> subprocess.CompletedProcess[str] | None:
    try:
        return context.run_command(
            [context.formatter, *arguments],
            cwd=context.root,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    except OSError as error:
        print(f"clang-format check: {operation} failed: {error}", file=context.stderr)
        return None


def has_pinned_formatter(context: ExecutionContext) -> bool:
    version_probe = run_formatter(["--version"], context, "formatter version probe")
    if version_probe is None:
        return False

    match = re.search(r"\bversion\s+(\d+\.\d+\.\d+)\b", version_probe.stdout)
    if (
        version_probe.returncode == 0
        and match is not None
        and match.group(1) == PINNED_CLANG_FORMAT_VERSION
    ):
        return True

    actual = match.group(1) if match is not None else "unknown"
    print(
        "clang-format check: requires clang-format "
        f"{PINNED_CLANG_FORMAT_VERSION}, found {actual}",
        file=context.stderr,
    )
    return False


def check_batch(paths: list[str], context: ExecutionContext) -> int:
    checked = run_formatter(
        ["--dry-run", "--Werror", *paths], context, "formatter invocation"
    )
    if checked is None:
        return 2

    if checked.stdout:
        print(checked.stdout, end="", file=context.stderr)
    if checked.stderr:
        print(checked.stderr, end="", file=context.stderr)
    if checked.returncode == 0:
        return 0
    if checked.returncode != 1:
        print(
            f"clang-format check: formatter exited with status {checked.returncode}",
            file=context.stderr,
        )
        return 2

    print("clang-format check: batch containing violations:", file=context.stderr)
    for path in paths:
        print(f"  {path}", file=context.stderr)
    return 1


def check_paths(paths: list[str], context: ExecutionContext) -> int:
    result = 0
    for offset in range(0, len(paths), FORMAT_BATCH_SIZE):
        batch_result = check_batch(
            paths[offset : offset + FORMAT_BATCH_SIZE], context
        )
        if batch_result == 2:
            return 2
        result = max(result, batch_result)
    return result


def main(
    argv: Sequence[str] | None = None,
    *,
    repo_root: Path | None = None,
    run_command: Callable[..., Any] = subprocess.run,
    stderr: TextIO = sys.stderr,
) -> int:
    arguments = create_argument_parser().parse_args(argv)
    context = ExecutionContext(
        root=repo_root or Path(__file__).resolve().parents[1],
        formatter=arguments.clang_format,
        run_command=run_command,
        stderr=stderr,
    )
    paths = discover_paths(arguments, context)
    if paths is None:
        return 2
    if not paths:
        return 0
    if not has_pinned_formatter(context):
        return 2
    return check_paths(paths, context)


if __name__ == "__main__":
    raise SystemExit(main())
