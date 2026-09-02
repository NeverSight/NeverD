#!/usr/bin/env python3
"""Validate NeverD's machine-readable capability claims."""

from __future__ import annotations

import argparse
import ast
import fnmatch
import json
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path, PurePosixPath
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[1]
SCHEMA_VERSION = 2
BUILD_COMPLETION_STAMP = ".neverd-capability-build-complete"
EVIDENCE_OUTPUT_LIMIT = 4 * 1024 * 1024
BUILD_OUTPUT_LIMIT = 64 * 1024 * 1024
CTEST_INVENTORY_OUTPUT_LIMIT = 64 * 1024 * 1024
GTEST_XML_OUTPUT_LIMIT = EVIDENCE_OUTPUT_LIMIT
CMAKE_GTEST_SKIP_PATTERNS = frozenset(
    {r"\[  SKIPPED \]", r"\\[  SKIPPED \\]"}
)
STATUSES = frozenset({"experimental", "supported", "unsupported"})
CAPABILITY_KINDS = frozenset(
    {
        "analysis",
        "debugging",
        "derivation",
        "execution",
        "rewrite",
        "synthesis",
        "translation",
    }
)
CAPABILITY_FIELDS = frozenset(
    {
        "id",
        "kind",
        "status",
        "owner",
        "targets",
        "docs",
        "limitations",
        "proof_test",
        "unknown_test",
        "poison_test",
        "tests",
        "public_surfaces",
    }
)
REQUIRED_CAPABILITY_FIELDS = frozenset(
    {
        "id",
        "kind",
        "status",
        "owner",
        "targets",
        "docs",
        "limitations",
        "tests",
        "public_surfaces",
    }
)
STRING_ARRAY_FIELDS = ("targets", "docs", "limitations")
PUBLIC_SURFACES = frozenset({"c", "python", "cli", "json"})
SURFACE_EXCLUSION_KEYS = frozenset({"owner", "reason", "names"})
REQUIRED_CAPABILITY_KINDS = {
    "debug.hardware": "debugging",
    "debug.local": "debugging",
    "debug.remote": "debugging",
    "exception.itanium.ada-d": "analysis",
    "exception.rewrite.end-to-end": "rewrite",
    "llvm.semantic.synthesis-rewrite": "rewrite",
    "safety.binary-sanitizer-publication": "rewrite",
    "safety.process-replay-native-execution": "execution",
    "semantic.mba.derivation": "derivation",
    "semantic.synthesis.candidate": "synthesis",
    "symbolic.execution.lowir-concolic": "analysis",
    "symbolic.execution.path-exploration": "analysis",
    "translation.executable-engine": "translation",
    "translation.runtime-contract": "translation",
}
REQUIRED_CAPABILITY_IDS = frozenset(REQUIRED_CAPABILITY_KINDS)
EVIDENCE_FIELDS = ("proof_test", "unknown_test", "poison_test")
TEST_EVIDENCE_KEYS = frozenset({"target", "source", "filter", "platforms", "runner"})
TEST_RUNNERS = frozenset({"gtest", "python-unittest"})
TEST_PLATFORMS = ("darwin", "linux", "windows")
GTEST_DECLARATION = re.compile(
    r"\b(TEST|TEST_F|TEST_P|TYPED_TEST|TYPED_TEST_P)\s*"
    r"\(\s*([A-Za-z_]\w*)\s*,\s*([A-Za-z_]\w*)\s*\)"
)
GTEST_VALUE_INSTANTIATION = re.compile(
    r"\bINSTANTIATE_TEST_(?:SUITE|CASE)_P\s*"
    r"\(\s*[A-Za-z_]\w*\s*,\s*([A-Za-z_]\w*)\s*,"
)
GTEST_TYPED_SUITE = re.compile(r"\bTYPED_TEST_(?:SUITE|CASE)\s*\(\s*([A-Za-z_]\w*)\s*,")
GTEST_TYPED_INSTANTIATION = re.compile(
    r"\bINSTANTIATE_TYPED_TEST_(?:SUITE|CASE)_P\s*"
    r"\(\s*[A-Za-z_]\w*\s*,\s*([A-Za-z_]\w*)\s*,"
)
GTEST_TYPED_REGISTRATION = re.compile(r"\bREGISTER_TYPED_TEST_(?:SUITE|CASE)_P\s*\(")
GTEST_FILTER_PATTERNS = (
    re.compile(r"^\*/([A-Za-z_]\w*)/\*\.([A-Za-z_]\w*)$"),  # TYPED_TEST_P
    re.compile(r"^\*/([A-Za-z_]\w*)\.([A-Za-z_]\w*)/\*$"),  # TEST_P
    re.compile(r"^([A-Za-z_]\w*)/\*\.([A-Za-z_]\w*)$"),  # TYPED_TEST
    re.compile(r"^([A-Za-z_]\w*)\.([A-Za-z_]\w*)$"),  # TEST/TEST_F
)
CXX_NON_CODE = re.compile(
    r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\r\n]*|/\*.*?\*/',
    re.DOTALL,
)
CMAKE_TARGET_DECLARATION = re.compile(
    r"\b(?:add_neverd_unittest|add_neverd_component_library|add_executable|"
    r"add_library|add_custom_target)\s*\(\s*([A-Za-z0-9_.:+-]+)",
    re.IGNORECASE,
)
CMAKE_LOGICAL_TARGET = re.compile(r"[A-Za-z0-9_.:+][A-Za-z0-9_.:+-]*")
CMAKE_BRACKET_COMMENT = re.compile(r"#\[(=*)\[.*?\]\1\]", re.DOTALL)
PUBLIC_C_DECLARATION = re.compile(
    r"\bNEVERD_API\b(?:(?![;{}]).)*?\b([A-Za-z_]\w*)\s*\(",
    re.DOTALL,
)
CLI_SUBCOMMAND_DECLARATION = re.compile(
    r'\b(?:llvm::)?cl::SubCommand\s+([A-Za-z_]\w*)\s*\(\s*"([^"]+)"'
)
CLI_OPTION_TYPE = r"(?:llvm::)?cl::(?:opt|list|alias)\s*(?:<[^;]*?>)?"
CLI_SUBCOMMAND_REFERENCE = re.compile(r"\b(?:llvm::)?cl::sub\(\s*([A-Za-z_]\w*)\s*\)")
CLI_LITERAL_CONTRACT = re.compile(
    r'\bNEVERD_CLI_LITERAL\s*\(\s*([A-Za-z_]\w*)\s*,\s*"([^"\\]+)"\s*\)'
)


@dataclass(frozen=True)
class TestDeclaration:
    """One source-level test declaration and its executable filter contract."""

    kind: str
    suite: str
    name: str
    platforms: frozenset[str]

    @property
    def canonical_filter(self) -> str:
        if self.kind == "PYTHON_UNITTEST":
            return f"{self.suite}.{self.name}"
        if self.kind in {"TEST", "TEST_F"}:
            return f"{self.suite}.{self.name}"
        if self.kind == "TEST_P":
            return f"*/{self.suite}.{self.name}/*"
        if self.kind == "TYPED_TEST":
            return f"{self.suite}/*.{self.name}"
        if self.kind == "TYPED_TEST_P":
            return f"*/{self.suite}/*.{self.name}"
        raise AssertionError(f"unhandled GoogleTest declaration kind: {self.kind}")


@dataclass(frozen=True)
class PlatformCondition:
    """Platforms on which a condition may evaluate true or false."""

    may_true: frozenset[str]
    may_false: frozenset[str]


@dataclass(frozen=True)
class ConfiguredTarget:
    sources: frozenset[str]
    artifacts: tuple[Path, ...]


@dataclass(frozen=True)
class ExecutedTestResult:
    """Machine-readable result of one evidence-runner invocation."""

    runtime_names: frozenset[str]
    tests: int
    failures: int
    errors: int
    disabled: int
    skipped: int


@dataclass(frozen=True)
class TestExecutionContext:
    working_directory: Path
    environment: tuple[tuple[str, str], ...]
    timeout: float


GTestSupport = dict[tuple[str, str, str], dict[str, frozenset[str]]]


def _source_files(
    root: Path, relative_roots: tuple[str, ...], suffixes: frozenset[str]
) -> list[Path]:
    resolved_root = root.resolve()
    files: list[Path] = []
    for relative_root in relative_roots:
        directory = root / relative_root
        if not directory.is_dir():
            continue
        for path in directory.rglob("*"):
            if not path.is_file() or path.suffix not in suffixes:
                continue
            try:
                path.resolve().relative_to(resolved_root)
            except ValueError:
                continue
            files.append(path)
    return sorted(set(files))


def _read_text(path: Path) -> str | None:
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeError):
        return None


def _read_utf8_with_limit(path: Path, *, limit: int, description: str) -> str:
    """Stat and bounded-read a subprocess artifact without unbounded allocation."""

    with path.open("rb") as stream:
        size = os.fstat(stream.fileno()).st_size
        if size > limit:
            raise ValueError(f"{description} exceeds {limit}-byte limit ({size} bytes)")
        payload = stream.read(limit + 1)
    if len(payload) > limit:
        raise ValueError(
            f"{description} exceeds {limit}-byte limit ({len(payload)} bytes read)"
        )
    return payload.decode("utf-8")


def current_test_platform() -> str:
    if sys.platform == "darwin":
        return "darwin"
    if sys.platform == "win32":
        return "windows"
    return "linux"


def _replace_preserving_newlines(match: re.Match[str]) -> str:
    text = match.group(0)
    return "\n" * text.count("\n") + " "


def _cxx_code(text: str) -> str:
    """Remove comments and literals without changing declaration line numbers."""

    return CXX_NON_CODE.sub(_replace_preserving_newlines, text)


def _cxx_without_comments(text: str) -> str:
    """Remove comments while retaining string literals used by CLI declarations."""

    return CXX_NON_CODE.sub(
        lambda match: (
            match.group(0)
            if match.group(0).startswith(('"', "'"))
            else _replace_preserving_newlines(match)
        ),
        text,
    )


def _top_level_boolean_parts(
    expression: str, *, word: str | None = None, symbol: str | None = None
) -> tuple[str, ...] | None:
    """Split one logical token while leaving every unknown predicate opaque."""

    parts: list[str] = []
    start = 0
    depth = 0
    quote: str | None = None
    escaped = False
    index = 0
    while index < len(expression):
        character = expression[index]
        if quote is not None:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == quote:
                quote = None
            index += 1
            continue
        if character in {'"', "'"}:
            quote = character
            index += 1
            continue
        if character == "(":
            depth += 1
            index += 1
            continue
        if character == ")" and depth:
            depth -= 1
            index += 1
            continue
        token_length = 0
        if depth == 0 and symbol is not None and expression.startswith(symbol, index):
            token_length = len(symbol)
        elif depth == 0 and word is not None:
            candidate = expression[index : index + len(word)]
            if len(candidate) == len(word):
                left_boundary = index == 0 or not (
                    expression[index - 1].isalnum() or expression[index - 1] == "_"
                )
                right_index = index + len(word)
                right_boundary = right_index == len(expression) or not (
                    expression[right_index].isalnum() or expression[right_index] == "_"
                )
                if candidate.upper() == word and left_boundary and right_boundary:
                    token_length = len(word)
        if token_length:
            parts.append(expression[start:index])
            index += token_length
            start = index
            continue
        index += 1
    if not parts:
        return None
    parts.append(expression[start:])
    return tuple(parts)


def _strip_boolean_parentheses(expression: str) -> str:
    """Remove only a balanced pair enclosing the complete expression."""

    value = expression.strip()
    while value.startswith("(") and value.endswith(")"):
        depth = 0
        quote: str | None = None
        escaped = False
        encloses_all = True
        for index, character in enumerate(value):
            if quote is not None:
                if escaped:
                    escaped = False
                elif character == "\\":
                    escaped = True
                elif character == quote:
                    quote = None
                continue
            if character in {'"', "'"}:
                quote = character
            elif character == "(":
                depth += 1
            elif character == ")":
                depth -= 1
                if depth == 0 and index != len(value) - 1:
                    encloses_all = False
                    break
                if depth < 0:
                    encloses_all = False
                    break
        if not encloses_all or depth != 0 or quote is not None:
            break
        value = value[1:-1].strip()
    return value


def _evaluate_boolean_expression(
    expression: str,
    *,
    environment: dict[str, bool | None],
    cmake: bool,
) -> bool | None:
    """Evaluate known boolean tokens and preserve unknown predicates as atoms."""

    def evaluate(value: str) -> bool | None:
        value = _strip_boolean_parentheses(value)
        or_parts = _top_level_boolean_parts(
            value, word="OR" if cmake else None, symbol=None if cmake else "||"
        )
        if or_parts is not None:
            results = [evaluate(part) for part in or_parts]
            if True in results:
                return True
            return False if all(result is False for result in results) else None
        and_parts = _top_level_boolean_parts(
            value, word="AND" if cmake else None, symbol=None if cmake else "&&"
        )
        if and_parts is not None:
            results = [evaluate(part) for part in and_parts]
            if False in results:
                return False
            return True if all(result is True for result in results) else None
        if cmake:
            not_match = re.match(r"(?i)^NOT\b", value)
            if not_match is not None:
                result = evaluate(value[not_match.end() :])
                return None if result is None else not result
        elif value.startswith("!") and not value.startswith("!="):
            result = evaluate(value[1:])
            return None if result is None else not result

        atom = value.strip()
        if cmake:
            quoted = re.fullmatch(r'"((?:\\.|[^"\\])*)"', atom)
            if quoted is not None:
                atom = quoted.group(1)
            if not atom:
                return False
            if re.fullmatch(
                r"(?i)(?:FALSE|OFF|NO|N|IGNORE|NOTFOUND|.*-NOTFOUND)", atom
            ):
                return False
            if re.fullmatch(r"(?i)(?:TRUE|ON|YES|Y)", atom):
                return True
            try:
                return int(atom, 10) != 0
            except ValueError:
                pass
            expansion = re.fullmatch(r"\$\{([A-Za-z_]\w*)\}", atom)
            if expansion is not None:
                atom = expansion.group(1)
            defined = re.fullmatch(r"(?i)DEFINED\s+([A-Za-z_]\w*)", atom)
            if defined is not None:
                name = defined.group(1)
                return environment.get(name) if name in environment else None
        else:
            integer = re.fullmatch(
                r"(?i)([+-]?(?:0x[0-9a-f]+|0b[01]+|0[0-7]*|"
                r"[1-9][0-9]*))(?:u(?:ll?)?|ll?u?)?",
                atom,
            )
            if integer is not None:
                literal = integer.group(1)
                unsigned = literal.lstrip("+-")
                if unsigned.lower().startswith("0x"):
                    base = 16
                elif unsigned.lower().startswith("0b"):
                    base = 2
                elif len(unsigned) > 1 and unsigned.startswith("0"):
                    base = 8
                else:
                    base = 10
                return int(literal, base) != 0
        return environment.get(atom) if atom in environment else None

    return evaluate(expression)


def _condition_platforms(expression: str, *, cmake: bool) -> PlatformCondition:
    """Evaluate the possible truth values of a build condition per platform."""

    value = expression.strip()
    if not cmake:
        value = re.sub(r"defined\s*\(\s*([A-Za-z_]\w*)\s*\)", r"\1", value)
        value = re.sub(r"defined\s+([A-Za-z_]\w*)", r"\1", value)

    may_true: set[str] = set()
    may_false: set[str] = set()
    for platform in TEST_PLATFORMS:
        environment: dict[str, bool | None] = {
            "APPLE": platform == "darwin",
            "__APPLE__": platform == "darwin",
            "WIN32": platform == "windows",
            "_WIN32": platform == "windows",
            "UNIX": platform in {"darwin", "linux"},
            "LINUX": platform == "linux",
            "__linux__": platform == "linux",
            "__linux": platform == "linux",
            # Compiler predicates imply Windows but are not true for every
            # Windows toolchain.  Keep both outcomes possible on Windows.
            "MSVC": None if platform == "windows" else False,
            "_MSC_VER": None if platform == "windows" else False,
        }
        result = _evaluate_boolean_expression(
            value, environment=environment, cmake=cmake
        )
        if result is not False:
            may_true.add(platform)
        if result is not True:
            may_false.add(platform)
    return PlatformCondition(
        may_true=frozenset(may_true), may_false=frozenset(may_false)
    )


def _cmake_logical_condition_lines(text: str) -> list[str]:
    """Fold multiline condition commands onto their first physical line."""

    lines = text.splitlines()
    logical_lines = list(lines)
    pattern = re.compile(
        r"(?im)^[ \t]*(if|elseif|else|endif)\s*\(",
    )
    cursor = 0
    while match := pattern.search(text, cursor):
        depth = 1
        quote = False
        escaped = False
        end = match.end()
        while end < len(text) and depth:
            character = text[end]
            if quote:
                if escaped:
                    escaped = False
                elif character == "\\":
                    escaped = True
                elif character == '"':
                    quote = False
            elif character == '"':
                quote = True
            elif character == "(":
                depth += 1
            elif character == ")":
                depth -= 1
            end += 1
        if depth:
            cursor = match.end()
            continue
        start_line = text.count("\n", 0, match.start())
        end_line = text.count("\n", 0, end - 1)
        body = text[match.end() : end - 1]
        logical_lines[start_line] = f"{match.group(1)}({body})"
        for line_index in range(start_line + 1, end_line + 1):
            logical_lines[line_index] = ""
        cursor = end
    return logical_lines


def _preprocessor_logical_lines(text: str) -> list[str]:
    """Fold backslash-continued preprocessor directives onto their first line."""

    lines = text.splitlines()
    logical_lines = list(lines)
    line_index = 0
    while line_index < len(lines):
        end_line = line_index
        parts: list[str] = []
        while lines[end_line].endswith("\\") and end_line + 1 < len(lines):
            parts.append(lines[end_line][:-1])
            end_line += 1
        if end_line != line_index:
            parts.append(lines[end_line])
            logical_lines[line_index] = " ".join(parts)
            for continuation in range(line_index + 1, end_line + 1):
                logical_lines[continuation] = ""
        line_index = end_line + 1
    return logical_lines


def _platforms_by_line(text: str, *, cmake: bool) -> list[frozenset[str]]:
    """Return the active OS set before each source line."""

    all_platforms = frozenset(TEST_PLATFORMS)
    active = all_platforms
    # The second stack item is the set on which no prior branch is guaranteed
    # to have run. Unknown feature predicates can leave a platform possible in
    # both a branch and its later elif/else sibling without erasing known OS
    # constraints.
    stack: list[tuple[frozenset[str], frozenset[str]]] = []
    result: list[frozenset[str]] = []
    logical_lines = (
        _cmake_logical_condition_lines(text)
        if cmake
        else _preprocessor_logical_lines(text)
    )
    for line in logical_lines:
        result.append(active)
        stripped = line.strip()
        directive: str | None = None
        condition: PlatformCondition | None = None
        if cmake:
            match = re.match(r"(?is)^if\s*\((.*)\)\s*$", stripped)
            if match:
                directive = "if"
                condition = _condition_platforms(match.group(1), cmake=True)
            else:
                match = re.match(r"(?is)^elseif\s*\((.*)\)\s*$", stripped)
            if directive is None and match:
                directive = "elif"
                condition = _condition_platforms(match.group(1), cmake=True)
            elif re.match(r"(?i)^else\s*\(?.*\)?\s*$", stripped):
                directive = "else"
            elif re.match(r"(?i)^endif\b", stripped):
                directive = "endif"
        else:
            match = re.match(r"^#\s*ifdef\s+([A-Za-z_]\w*)", stripped)
            negate = False
            if match is None:
                match = re.match(r"^#\s*ifndef\s+([A-Za-z_]\w*)", stripped)
                negate = match is not None
            if match is not None:
                directive = "if"
                expression = ("!" if negate else "") + match.group(1)
                condition = _condition_platforms(expression, cmake=False)
            else:
                match = re.match(r"^#\s*if\s+(.+)$", stripped)
            if directive is None and match:
                directive = "if"
                condition = _condition_platforms(match.group(1), cmake=False)
            elif directive is None:
                match = re.match(r"^#\s*elif\s+(.+)$", stripped)
            if directive is None and match:
                directive = "elif"
                condition = _condition_platforms(match.group(1), cmake=False)
            elif re.match(r"^#\s*else\b", stripped):
                directive = "else"
            elif re.match(r"^#\s*endif\b", stripped):
                directive = "endif"

        if directive == "if":
            assert condition is not None
            parent = active
            stack.append((parent, parent & condition.may_false))
            active = parent & condition.may_true
        elif directive == "elif" and stack:
            assert condition is not None
            parent, remaining = stack[-1]
            active = remaining & condition.may_true
            stack[-1] = (parent, remaining & condition.may_false)
        elif directive == "else" and stack:
            parent, remaining = stack[-1]
            active = remaining
            stack[-1] = (parent, frozenset())
        elif directive == "endif" and stack:
            active, _consumed = stack.pop()
    return result


def _cmake_calls(text: str) -> list[tuple[str, str, int]]:
    """Return selected CMake calls as (name, body, zero-based line)."""

    calls: list[tuple[str, str, int]] = []
    pattern = re.compile(
        r"\b(add_neverd_unittest|add_executable|add_library|target_sources|"
        r"add_test|add_subdirectory)\s*\(",
        re.IGNORECASE,
    )
    for match in pattern.finditer(text):
        depth = 1
        quote = False
        escaped = False
        cursor = match.end()
        while cursor < len(text) and depth:
            char = text[cursor]
            if quote:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == '"':
                    quote = False
            elif char == '"':
                quote = True
            elif char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
            cursor += 1
        if depth == 0:
            calls.append(
                (
                    match.group(1).lower(),
                    text[match.end() : cursor - 1],
                    text.count("\n", 0, match.start()),
                )
            )
    return calls


def _cmake_top_level_code(raw_text: str) -> str:
    """Strip comments and non-executed function/macro definitions.

    Static preflight deliberately does not pretend to interpret arbitrary CMake
    functions.  Registrations hidden behind a wrapper therefore require the
    configured build audit; an uncalled wrapper can never become source proof.
    """

    text = CMAKE_BRACKET_COMMENT.sub(_replace_preserving_newlines, raw_text)
    text = "\n".join(line.split("#", 1)[0] for line in text.splitlines())
    depth = 0
    result: list[str] = []
    for line in text.splitlines():
        stripped = line.strip()
        if re.match(r"(?i)^(?:function|macro)\s*\(", stripped):
            depth += 1
            result.append("")
            continue
        if re.match(r"(?i)^end(?:function|macro)\b", stripped):
            depth = max(0, depth - 1)
            result.append("")
            continue
        result.append("" if depth else line)
    return "\n".join(result)


def _cmake_tokens(body: str) -> list[str]:
    return [
        token[1:-1] if token.startswith('"') and token.endswith('"') else token
        for token in re.findall(r'"(?:\\.|[^"\\])*"|[^\s()]+', body)
    ]


def _cmake_source_path(root: Path, cmake_path: Path, token: str) -> str | None:
    source_suffixes = {".c", ".cc", ".cpp", ".cxx", ".m", ".mm", ".py"}
    if not any(token.endswith(suffix) for suffix in source_suffixes):
        return None
    if token.startswith("${CMAKE_SOURCE_DIR}/"):
        candidate = root / token.removeprefix("${CMAKE_SOURCE_DIR}/")
    elif token.startswith("${CMAKE_CURRENT_SOURCE_DIR}/"):
        candidate = cmake_path.parent / token.removeprefix(
            "${CMAKE_CURRENT_SOURCE_DIR}/"
        )
    elif "${" in token or "$<" in token:
        return None
    else:
        candidate = cmake_path.parent / token
    try:
        return candidate.resolve(strict=False).relative_to(root.resolve()).as_posix()
    except ValueError:
        return None


def collect_cmake_test_sources(
    root: Path,
) -> dict[tuple[str, str], frozenset[str]]:
    """Map registered CMake test targets to their literal source ownership."""

    parsed_files: list[
        tuple[Path, list[frozenset[str]], list[tuple[str, str, int]]]
    ] = []
    registered_targets: dict[str, set[str]] = {}
    for path, inherited_platforms in _cmake_file_platforms(root).items():
        raw_text = path.read_text(encoding="utf-8")
        text = _cmake_top_level_code(raw_text)
        calls = _cmake_calls(text)
        line_platforms = [
            platforms & inherited_platforms
            for platforms in _platforms_by_line(text, cmake=True)
        ]
        parsed_files.append((path, line_platforms, calls))
        for name, body, line in calls:
            if name == "add_neverd_unittest":
                tokens = _cmake_tokens(body)
                if tokens:
                    platforms = (
                        line_platforms[line]
                        if line < len(line_platforms)
                        else frozenset()
                    )
                    registered_targets.setdefault(tokens[0], set()).update(platforms)

    ownership: dict[tuple[str, str], set[str]] = {}
    for path, line_platforms, calls in parsed_files:
        for name, body, line in calls:
            if name not in {"add_neverd_unittest", "target_sources"}:
                continue
            tokens = _cmake_tokens(body)
            if not tokens:
                continue
            target = tokens[0]
            registration_platforms = registered_targets.get(target)
            if registration_platforms is None:
                continue
            call_platforms = (
                line_platforms[line] if line < len(line_platforms) else frozenset()
            )
            platforms = call_platforms & registration_platforms
            for token in tokens[1:]:
                source = _cmake_source_path(root, path, token)
                if source is not None:
                    ownership.setdefault((target, source), set()).update(platforms)
    return {key: frozenset(value) for key, value in ownership.items()}


def collect_python_test_sources(
    root: Path,
) -> dict[tuple[str, str], frozenset[str]]:
    """Map reachable CTest unittest registrations to their owned sources."""

    ownership: dict[tuple[str, str], set[str]] = {}
    for path, inherited_platforms in _cmake_file_platforms(root).items():
        raw_text = path.read_text(encoding="utf-8")
        text = _cmake_top_level_code(raw_text)
        line_platforms = [
            platforms & inherited_platforms
            for platforms in _platforms_by_line(text, cmake=True)
        ]
        for name, body, line in _cmake_calls(text):
            if name != "add_test":
                continue
            tokens = _cmake_tokens(body)
            if "NAME" not in tokens or "unittest" not in tokens:
                continue
            name_index = tokens.index("NAME") + 1
            if name_index >= len(tokens):
                continue
            target = tokens[name_index]
            platforms = (
                line_platforms[line] if line < len(line_platforms) else frozenset()
            )
            unittest_index = tokens.index("unittest")
            runner_arguments = tokens[unittest_index + 1 :]
            source_directory: Path | None = None
            pattern = "test*.py"
            if runner_arguments and runner_arguments[0] == "discover":
                if "-s" not in runner_arguments:
                    continue
                source_index = runner_arguments.index("-s") + 1
                if source_index >= len(runner_arguments):
                    continue
                source_token = runner_arguments[source_index]
                if source_token.startswith("${CMAKE_SOURCE_DIR}/"):
                    source_directory = root / source_token.removeprefix(
                        "${CMAKE_SOURCE_DIR}/"
                    )
                elif source_token.startswith("${CMAKE_CURRENT_SOURCE_DIR}/"):
                    source_directory = path.parent / source_token.removeprefix(
                        "${CMAKE_CURRENT_SOURCE_DIR}/"
                    )
                if "-p" in runner_arguments and runner_arguments.index("-p") + 1 < len(
                    runner_arguments
                ):
                    pattern = runner_arguments[runner_arguments.index("-p") + 1]
                if source_directory is None or not source_directory.is_dir():
                    continue
                for source_path in source_directory.glob(pattern):
                    source = (
                        source_path.resolve().relative_to(root.resolve()).as_posix()
                    )
                    ownership.setdefault((target, source), set()).update(platforms)
                continue

            for module in runner_arguments:
                if re.fullmatch(r"[A-Za-z_]\w*(?:\.[A-Za-z_]\w*)+", module) is None:
                    continue
                parts = module.split(".")
                for end in range(len(parts), 0, -1):
                    candidate = root / Path(*parts[:end]).with_suffix(".py")
                    if not candidate.is_file():
                        continue
                    source = candidate.relative_to(root).as_posix()
                    ownership.setdefault((target, source), set()).update(platforms)
                    break
    return {key: frozenset(value) for key, value in ownership.items()}


def _macro_arguments(code: str, open_paren: int) -> list[str] | None:
    """Split one already comment/literal-stripped macro invocation."""

    depth = 1
    start = open_paren + 1
    cursor = start
    arguments: list[str] = []
    while cursor < len(code):
        character = code[cursor]
        if character == "(":
            depth += 1
        elif character == ")":
            depth -= 1
            if depth == 0:
                arguments.append(code[start:cursor].strip())
                return arguments
        elif character == "," and depth == 1:
            arguments.append(code[start:cursor].strip())
            start = cursor + 1
        cursor += 1
    return None


def _record_gtest_support(
    support: GTestSupport,
    *,
    kind: str,
    suite: str,
    name: str = "",
    source: str,
    platforms: frozenset[str],
) -> None:
    sources = support.setdefault((kind, suite, name), {})
    sources[source] = sources.get(source, frozenset()) | platforms


def collect_test_declarations(
    root: Path,
) -> tuple[dict[tuple[str, str], TestDeclaration], GTestSupport]:
    """Collect source declarations plus target-linkable parameterization support."""

    declarations: dict[tuple[str, str], TestDeclaration] = {}
    support: GTestSupport = {}
    for path in _source_files(
        root, ("unittests",), frozenset({".cc", ".cpp", ".cxx", ".mm"})
    ):
        text = _read_text(path)
        if text is None:
            continue
        code = _cxx_code(text)
        line_platforms = _platforms_by_line(code, cmake=False)
        source = path.relative_to(root).as_posix()
        for match in GTEST_DECLARATION.finditer(code):
            line = code.count("\n", 0, match.start())
            platforms = (
                line_platforms[line]
                if line < len(line_platforms)
                else frozenset(TEST_PLATFORMS)
            )
            kind, suite, name = match.groups()
            declarations[(source, f"{suite}.{name}")] = TestDeclaration(
                kind=kind,
                suite=suite,
                name=name,
                platforms=platforms,
            )

        for match in GTEST_VALUE_INSTANTIATION.finditer(code):
            line = code.count("\n", 0, match.start())
            _record_gtest_support(
                support,
                kind="TEST_P",
                suite=match.group(1),
                source=source,
                platforms=(
                    line_platforms[line]
                    if line < len(line_platforms)
                    else frozenset(TEST_PLATFORMS)
                ),
            )
        for match in GTEST_TYPED_SUITE.finditer(code):
            line = code.count("\n", 0, match.start())
            _record_gtest_support(
                support,
                kind="TYPED_TEST",
                suite=match.group(1),
                source=source,
                platforms=(
                    line_platforms[line]
                    if line < len(line_platforms)
                    else frozenset(TEST_PLATFORMS)
                ),
            )
        for match in GTEST_TYPED_INSTANTIATION.finditer(code):
            line = code.count("\n", 0, match.start())
            _record_gtest_support(
                support,
                kind="TYPED_TEST_P",
                suite=match.group(1),
                source=source,
                platforms=(
                    line_platforms[line]
                    if line < len(line_platforms)
                    else frozenset(TEST_PLATFORMS)
                ),
            )
        for match in GTEST_TYPED_REGISTRATION.finditer(code):
            arguments = _macro_arguments(code, match.end() - 1)
            if arguments is None or len(arguments) < 2:
                continue
            suite = arguments[0]
            if re.fullmatch(r"[A-Za-z_]\w*", suite) is None:
                continue
            line = code.count("\n", 0, match.start())
            platforms = (
                line_platforms[line]
                if line < len(line_platforms)
                else frozenset(TEST_PLATFORMS)
            )
            for name in arguments[1:]:
                if re.fullmatch(r"[A-Za-z_]\w*", name) is not None:
                    _record_gtest_support(
                        support,
                        kind="TYPED_TEST_P_REGISTER",
                        suite=suite,
                        name=name,
                        source=source,
                        platforms=platforms,
                    )

    for path in _source_files(
        root,
        ("scripts/tests", "pluginsdk/python/tests", "unittests"),
        frozenset({".py"}),
    ):
        text = _read_text(path)
        if text is None:
            continue
        try:
            module = ast.parse(text, filename=str(path))
        except SyntaxError:
            continue
        source = path.relative_to(root).as_posix()
        for node in module.body:
            if not isinstance(node, ast.ClassDef):
                continue
            for member in node.body:
                if isinstance(member, (ast.FunctionDef, ast.AsyncFunctionDef)) and (
                    member.name.startswith("test_")
                ):
                    declarations[(source, f"{node.name}.{member.name}")] = (
                        TestDeclaration(
                            kind="PYTHON_UNITTEST",
                            suite=node.name,
                            name=member.name,
                            platforms=frozenset(TEST_PLATFORMS),
                        )
                    )
    return declarations, support


def _python_string_sequence(
    node: ast.AST | None,
    sequences: dict[str, tuple[str, ...]],
) -> tuple[str, ...] | None:
    if isinstance(node, (ast.List, ast.Tuple, ast.Set)):
        values: list[str] = []
        for element in node.elts:
            if isinstance(element, ast.Starred):
                nested = _python_string_sequence(element.value, sequences)
                if nested is None:
                    return None
                values.extend(nested)
            elif isinstance(element, ast.Constant) and isinstance(element.value, str):
                values.append(element.value)
            else:
                return None
        return tuple(values)
    if isinstance(node, ast.Name):
        return sequences.get(node.id)
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
        left = _python_string_sequence(node.left, sequences)
        right = _python_string_sequence(node.right, sequences)
        if left is not None and right is not None:
            return left + right
    return None


def _python_package_exports(
    root: Path,
) -> tuple[tuple[str, ...], dict[str, tuple[Path, str]]]:
    package = root / "pluginsdk" / "python" / "neverd_plugin"
    init_path = package / "__init__.py"
    init_text = _read_text(init_path)
    if init_text is None:
        return (), {}
    try:
        module = ast.parse(init_text, filename=str(init_path))
    except SyntaxError:
        return (), {}

    sequences: dict[str, tuple[str, ...]] = {}
    exports: tuple[str, ...] = ()
    bindings: dict[str, tuple[Path, str]] = {}
    for node in module.body:
        if isinstance(node, ast.ImportFrom) and node.level == 1 and node.module:
            source_path = package / Path(*node.module.split(".")).with_suffix(".py")
            for alias in node.names:
                if alias.name != "*":
                    bindings[alias.asname or alias.name] = (source_path, alias.name)
            continue
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            bindings[node.name] = (init_path, node.name)
            continue
        if isinstance(node, (ast.Assign, ast.AnnAssign)):
            value = _python_string_sequence(node.value, sequences)
            targets = node.targets if isinstance(node, ast.Assign) else [node.target]
            for target in targets:
                if not isinstance(target, ast.Name) or value is None:
                    continue
                sequences[target.id] = value
                if target.id == "__all__":
                    exports = value
            continue
        if (
            isinstance(node, ast.AugAssign)
            and isinstance(node.op, ast.Add)
            and isinstance(node.target, ast.Name)
        ):
            value = _python_string_sequence(node.value, sequences)
            if value is None:
                continue
            combined = sequences.get(node.target.id, ()) + value
            sequences[node.target.id] = combined
            if node.target.id == "__all__":
                exports = combined
            continue
        if not isinstance(node, ast.Expr) or not isinstance(node.value, ast.Call):
            continue
        call = node.value
        if (
            not isinstance(call.func, ast.Attribute)
            or not isinstance(call.func.value, ast.Name)
            or call.func.value.id != "__all__"
            or len(call.args) != 1
        ):
            continue
        current = sequences.get("__all__", exports)
        if call.func.attr == "extend":
            value = _python_string_sequence(call.args[0], sequences)
            if value is not None:
                exports = current + value
        elif (
            call.func.attr == "append"
            and isinstance(call.args[0], ast.Constant)
            and isinstance(call.args[0].value, str)
        ):
            exports = current + (call.args[0].value,)
        sequences["__all__"] = exports
    return exports, bindings


@lru_cache(maxsize=32)
def collect_public_surfaces(root: Path) -> dict[str, frozenset[str]]:
    """Collect public API and CLI declarations from authoritative sources."""

    c_declarations: set[str] = set()
    for path in _source_files(
        root, ("include/neverd/sdk",), frozenset({".h", ".hh", ".hpp"})
    ):
        text = _read_text(path)
        if text is not None:
            code = CXX_NON_CODE.sub(" ", text)
            code = "\n".join(
                "" if line.lstrip().startswith("#") else line
                for line in code.splitlines()
            )
            c_declarations.update(PUBLIC_C_DECLARATION.findall(code))

    python_exports, python_bindings = _python_package_exports(root)
    python_declarations: set[str] = set()
    module_cache: dict[Path, ast.Module | None] = {}
    for public_name in python_exports:
        binding = python_bindings.get(public_name)
        if binding is None:
            continue
        path, source_name = binding
        if path not in module_cache:
            text = _read_text(path)
            if text is None:
                module_cache[path] = None
            else:
                try:
                    module_cache[path] = ast.parse(text, filename=str(path))
                except SyntaxError:
                    module_cache[path] = None
        module = module_cache[path]
        if module is None:
            continue
        declaration = next(
            (
                node
                for node in module.body
                if isinstance(
                    node,
                    (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef),
                )
                and node.name == source_name
            ),
            None,
        )
        if isinstance(declaration, (ast.FunctionDef, ast.AsyncFunctionDef)):
            python_declarations.add(public_name)
        elif isinstance(declaration, ast.ClassDef):
            python_declarations.update(
                f"{public_name}.{member.name}"
                for member in declaration.body
                if isinstance(member, (ast.FunctionDef, ast.AsyncFunctionDef))
                and not member.name.startswith("_")
            )

    cli_declarations: set[str] = set()
    cli_sources = _source_files(
        root,
        ("tools/neverd",),
        frozenset({".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inc"}),
    )
    cli_units = [
        (path, _cxx_without_comments(text))
        for path in cli_sources
        if (text := _read_text(path)) is not None
    ]
    if cli_units:
        aliases_by_path: dict[Path, dict[str, str]] = {
            path: {
                name: expression.strip()
                for name, expression in re.findall(
                    r"\busing\s+([A-Za-z_]\w*)\s*=\s*([^;]+);",
                    code,
                    flags=re.DOTALL,
                )
            }
            for path, code in cli_units
        }

        def resolve_option_aliases(
            aliases: dict[str, str], inherited: set[str]
        ) -> set[str]:
            option_aliases = set(inherited)
            changed = True
            while changed:
                changed = False
                for name, expression in aliases.items():
                    if name in option_aliases:
                        continue
                    if (
                        re.search(
                            r"\b(?:llvm::)?cl::(?:opt|list|alias)\s*<",
                            expression,
                        )
                        or expression in option_aliases
                    ):
                        option_aliases.add(name)
                        changed = True
            return option_aliases

        global_option_aliases: set[str] = set()
        for aliases in aliases_by_path.values():
            global_option_aliases.update(resolve_option_aliases(aliases, set()))

        required_values: dict[str, set[str]] = {}
        values_text = _read_text(root / "tools" / "neverd" / "NeverDCLIValues.def")
        if values_text is not None:
            for variable, value in CLI_LITERAL_CONTRACT.findall(values_text):
                required_values.setdefault(variable, set()).add(value)

        global_commands: dict[str, set[str]] = {}
        for _path, code in cli_units:
            for variable, command in CLI_SUBCOMMAND_DECLARATION.findall(code):
                global_commands.setdefault(variable, set()).add(command)

        for path, code in cli_units:
            local_aliases = aliases_by_path[path]
            inherited_aliases = global_option_aliases - set(local_aliases)
            option_aliases = resolve_option_aliases(local_aliases, inherited_aliases)
            option_types = [CLI_OPTION_TYPE]
            option_types.extend(re.escape(name) for name in sorted(option_aliases))
            option_declaration = re.compile(
                rf"\b(?:{'|'.join(option_types)})\s+"
                r"([A-Za-z_]\w*)\s*\((.*?)\);",
                re.DOTALL,
            )
            local_commands: dict[str, set[str]] = {}
            for variable, command in CLI_SUBCOMMAND_DECLARATION.findall(code):
                local_commands.setdefault(variable, set()).add(command)
                cli_declarations.add(f"neverd {command}")
            for match in option_declaration.finditer(code):
                variable = match.group(1)
                body = match.group(2)
                option = re.match(r'\s*"([^"]+)"', body)
                if option is None:
                    continue
                for subcommand_variable in CLI_SUBCOMMAND_REFERENCE.findall(body):
                    # Anonymous namespaces routinely reuse short identifiers in
                    # separate translation units.  Resolve a same-file command
                    # first; only an extern-style reference may fall back to
                    # the repository-wide candidates.  Ambiguity expands proof
                    # obligations instead of silently dropping a real surface.
                    commands = local_commands.get(
                        subcommand_variable,
                        global_commands.get(subcommand_variable, set()),
                    )
                    for command in commands:
                        spelling = f"neverd {command} --{option.group(1)}"
                        values = required_values.get(variable)
                        if values:
                            cli_declarations.update(
                                f"{spelling}={value}" for value in sorted(values)
                            )
                        else:
                            cli_declarations.add(spelling)

    frozen_c = frozenset(c_declarations)
    return {
        "c": frozen_c,
        "python": frozenset(python_declarations),
        "cli": frozenset(cli_declarations),
        "json": frozenset(
            name for name in c_declarations if re.search(r"_json(?:_|$)", name)
        ),
    }


def _cmake_file_platforms(root: Path) -> dict[Path, frozenset[str]]:
    """Map each reachable CMakeLists to its inherited possible OS set."""

    resolved_root = root.resolve()
    entry = root / "CMakeLists.txt"
    if not entry.is_file():
        return {}
    pending: list[tuple[Path, frozenset[str]]] = [(entry, frozenset(TEST_PLATFORMS))]
    inherited: dict[Path, set[str]] = {}
    while pending:
        path, candidate_platforms = pending.pop()
        resolved = path.resolve(strict=False)
        if not path.is_file() or not candidate_platforms:
            continue
        try:
            relative = resolved.relative_to(resolved_root)
        except ValueError:
            continue
        # NeverD's capability ledger owns this repository, not vendored build
        # systems.  Ignoring third_party is conservative: it can remove proof,
        # never manufacture it.
        if relative.parts and relative.parts[0] == "third_party":
            continue
        known_platforms = inherited.setdefault(resolved, set())
        new_platforms = candidate_platforms - known_platforms
        if not new_platforms:
            continue
        known_platforms.update(new_platforms)
        text = _cmake_top_level_code(path.read_text(encoding="utf-8"))
        line_platforms = _platforms_by_line(text, cmake=True)
        for name, body, line in _cmake_calls(text):
            if name != "add_subdirectory":
                continue
            local_platforms = (
                line_platforms[line]
                if line < len(line_platforms)
                else frozenset(TEST_PLATFORMS)
            )
            platforms = frozenset(new_platforms) & local_platforms
            if not platforms:
                continue
            tokens = _cmake_tokens(body)
            if not tokens:
                continue
            token = tokens[0]
            if token.startswith("${CMAKE_SOURCE_DIR}/"):
                directory = root / token.removeprefix("${CMAKE_SOURCE_DIR}/")
            elif token.startswith("${CMAKE_CURRENT_SOURCE_DIR}/"):
                directory = path.parent / token.removeprefix(
                    "${CMAKE_CURRENT_SOURCE_DIR}/"
                )
            elif "${" in token or "$<" in token or Path(token).is_absolute():
                continue
            else:
                directory = path.parent / token
            pending.append((directory / "CMakeLists.txt", platforms))
    return {
        root / path.relative_to(resolved_root): frozenset(platforms)
        for path, platforms in inherited.items()
    }


def _cmake_files(root: Path) -> list[Path]:
    """Return only literal CMakeLists reachable from the project root."""

    return sorted(_cmake_file_platforms(root))


def collect_cmake_targets(root: Path) -> frozenset[str]:
    """Collect literal target declarations without inspecting build trees."""

    targets: set[str] = set()
    for path in _cmake_files(root):
        text = _cmake_top_level_code(path.read_text(encoding="utf-8"))
        targets.update(CMAKE_TARGET_DECLARATION.findall(text))
    return frozenset(targets)


def _json_object(path: Path) -> dict[str, Any] | None:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return None
    return value if isinstance(value, dict) else None


def _find_file_api_reference(value: Any, kind: str) -> str | None:
    if isinstance(value, dict):
        if value.get("kind") == kind and isinstance(value.get("jsonFile"), str):
            return value["jsonFile"]
        for nested in value.values():
            reference = _find_file_api_reference(nested, kind)
            if reference is not None:
                return reference
    elif isinstance(value, list):
        for nested in value:
            reference = _find_file_api_reference(nested, kind)
            if reference is not None:
                return reference
    return None


def _find_codemodel_reference(value: Any) -> str | None:
    return _find_file_api_reference(value, "codemodel")


def _latest_file_api_index(build_directory: Path) -> Path | None:
    reply = build_directory / ".cmake" / "api" / "v1" / "reply"
    indices = list(reply.glob("index-*.json")) if reply.is_dir() else []
    if not indices:
        # Unit fixtures and future CMake versions need not encode a timestamp
        # in the filename.  The reply directory itself is the trust boundary.
        indices = list(reply.glob("index*.json")) if reply.is_dir() else []
    try:
        return max(indices, key=lambda path: path.stat().st_mtime_ns)
    except (OSError, ValueError):
        return None


def audit_configured_build_identity(root: Path, build_directory: Path) -> list[str]:
    """Prove --build-dir belongs to this repository before invoking a build."""

    cache = build_directory / "CMakeCache.txt"
    try:
        cache_text = cache.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        return [f"build audit: cannot read configured cache {cache}: {error}"]
    home_match = re.search(
        r"(?m)^CMAKE_HOME_DIRECTORY(?::[^=\r\n]+)?=(.*)$", cache_text
    )
    if home_match is None or not home_match.group(1).strip():
        return ["build audit: CMake cache has no CMAKE_HOME_DIRECTORY"]
    configured_root = Path(home_match.group(1).strip()).resolve(strict=False)
    resolved_root = root.resolve(strict=False)
    if configured_root != resolved_root:
        return [
            "build audit: CMake cache source directory does not match "
            f"repository root: {configured_root}"
        ]

    index_path = _latest_file_api_index(build_directory)
    if index_path is None:
        return ["build audit: no CMake File API index for build identity audit"]
    index = _json_object(index_path)
    if index is None:
        return [f"build audit: invalid CMake File API index {index_path}"]
    codemodel_reference = _find_codemodel_reference(index)
    if codemodel_reference is None:
        return ["build audit: CMake File API index has no codemodel-v2 reply"]
    codemodel = _json_object(index_path.parent / codemodel_reference)
    if codemodel is None:
        return [f"build audit: invalid codemodel reply {codemodel_reference}"]
    paths = codemodel.get("paths")
    source_value = paths.get("source") if isinstance(paths, dict) else None
    if not isinstance(source_value, str) or not source_value:
        return ["build audit: codemodel has no source directory"]
    codemodel_root = Path(source_value)
    if not codemodel_root.is_absolute():
        return ["build audit: codemodel source directory is not absolute"]
    codemodel_root = codemodel_root.resolve(strict=False)
    if codemodel_root != resolved_root:
        return [
            "build audit: codemodel source directory does not match "
            f"repository root: {codemodel_root}"
        ]
    return []


def audit_build_freshness(
    root: Path,
    build_directory: Path,
    document: dict[str, Any],
    *,
    manifest_path: Path | None = None,
) -> list[str]:
    """Reject configured/build evidence that predates its current inputs."""

    stamp = build_directory / BUILD_COMPLETION_STAMP
    if not stamp.is_file():
        return [
            f"build audit: missing post-build completion stamp {stamp}; "
            "create it only after the requested build succeeds"
        ]
    index = _latest_file_api_index(build_directory)
    if index is None:
        return ["build audit: no current CMake File API index for freshness audit"]
    cache = build_directory / "CMakeCache.txt"
    if not cache.is_file():
        return [f"build audit: missing configured cache {cache}"]

    try:
        stamp_time = stamp.stat().st_mtime_ns
        index_time = index.stat().st_mtime_ns
        cache_time = cache.stat().st_mtime_ns
    except OSError as error:
        return [f"build audit: cannot stat configured snapshot: {error}"]

    diagnostics: list[str] = []
    if index_time < cache_time:
        diagnostics.append(
            "build audit: CMake File API snapshot predates the configured "
            "cache; reconfigure with a current query before auditing evidence"
        )
    if stamp_time < index_time or stamp_time < cache_time:
        diagnostics.append(
            "build audit: post-build completion stamp predates the configured "
            "CMake snapshot; rebuild before auditing evidence"
        )

    configure_inputs = list(_cmake_files(root))
    cmake_directory = root / "cmake"
    if cmake_directory.is_dir():
        configure_inputs.extend(cmake_directory.rglob("*.cmake"))
    cmake_files_query = (
        build_directory / ".cmake" / "api" / "v1" / "query" / "cmakeFiles-v1"
    )
    index_document = _json_object(index)
    cmake_files_reference = (
        _find_file_api_reference(index_document, "cmakeFiles")
        if index_document is not None
        else None
    )
    if cmake_files_query.is_file() and cmake_files_reference is None:
        diagnostics.append(
            "build audit: requested CMake File API cmakeFiles-v1 reply is missing"
        )
    elif cmake_files_reference is not None:
        cmake_files_document = _json_object(index.parent / cmake_files_reference)
        if cmake_files_document is None:
            diagnostics.append(
                "build audit: requested CMake File API cmakeFiles-v1 reply is invalid"
            )
        else:
            paths = cmake_files_document.get("paths")
            source_root = root
            if isinstance(paths, dict) and isinstance(paths.get("source"), str):
                source_root = Path(paths["source"])
                if not source_root.is_absolute():
                    source_root = root / source_root
            inputs = cmake_files_document.get("inputs")
            if isinstance(inputs, list):
                for entry in inputs:
                    if (
                        not isinstance(entry, dict)
                        or entry.get("isGenerated") is True
                        or entry.get("isExternal") is True
                        or not isinstance(entry.get("path"), str)
                    ):
                        continue
                    path = Path(entry["path"])
                    if not path.is_absolute():
                        path = source_root / path
                    try:
                        path.resolve(strict=False).relative_to(root.resolve())
                    except ValueError:
                        continue
                    configure_inputs.append(path)
    for path in sorted(set(configure_inputs)):
        try:
            is_stale = index_time < path.stat().st_mtime_ns
        except OSError:
            continue
        if is_stale:
            diagnostics.append(
                "build audit: CMake File API snapshot predates configure input "
                f"{path.relative_to(root).as_posix()}"
            )

    build_inputs: set[Path] = set()
    if manifest_path is not None:
        build_inputs.add(manifest_path)
    checker = root / "scripts" / "check_capabilities.py"
    if checker.is_file():
        build_inputs.add(checker)
    current_platform = current_test_platform()
    for _label, evidence in _iter_manifest_evidence(document):
        platforms = evidence.get("platforms")
        source = evidence.get("source")
        if (
            isinstance(platforms, list)
            and current_platform in platforms
            and isinstance(source, str)
        ):
            build_inputs.add(root / source)
    for path in sorted(build_inputs):
        if not path.is_file():
            continue
        try:
            is_stale = stamp_time < path.stat().st_mtime_ns
        except OSError:
            continue
        if is_stale:
            try:
                name = path.relative_to(root).as_posix()
            except ValueError:
                name = str(path)
            diagnostics.append(
                "build audit: post-build completion stamp predates input " + name
            )
    return sorted(set(diagnostics))


def collect_configured_targets(
    root: Path,
    build_directory: Path,
    *,
    build_config: str | None = None,
) -> tuple[dict[str, ConfiguredTarget], list[str]]:
    """Read target/source/artifact truth from a CMake File API codemodel."""

    diagnostics: list[str] = []
    reply = build_directory / ".cmake" / "api" / "v1" / "reply"
    index_path = _latest_file_api_index(build_directory)
    if index_path is None:
        return {}, [
            f"build audit: no CMake File API reply in {reply}; create the "
            "codemodel-v2 query before configure"
        ]
    index = _json_object(index_path)
    if index is None:
        return {}, [f"build audit: invalid CMake File API index {index_path}"]
    codemodel_file = _find_codemodel_reference(index)
    if codemodel_file is None:
        return {}, ["build audit: CMake File API index has no codemodel-v2 reply"]
    codemodel = _json_object(reply / codemodel_file)
    if codemodel is None:
        return {}, [f"build audit: invalid codemodel reply {codemodel_file}"]
    configurations = codemodel.get("configurations")
    if not isinstance(configurations, list) or not configurations:
        return {}, ["build audit: codemodel has no configurations"]
    selected: dict[str, Any] | None = None
    if build_config is not None:
        selected = next(
            (
                configuration
                for configuration in configurations
                if isinstance(configuration, dict)
                and configuration.get("name") == build_config
            ),
            None,
        )
        if selected is None:
            diagnostics.append(
                f"build audit: codemodel has no configuration {build_config}"
            )
            return {}, diagnostics
    elif len(configurations) == 1 and isinstance(configurations[0], dict):
        selected = configurations[0]
    else:
        diagnostics.append(
            "build audit: codemodel has multiple configurations; pass --build-config"
        )
        return {}, diagnostics

    configured: dict[str, ConfiguredTarget] = {}
    targets = selected.get("targets") if selected is not None else None
    if not isinstance(targets, list):
        return {}, ["build audit: selected codemodel configuration has no targets"]
    resolved_root = root.resolve()
    for reference in targets:
        if not isinstance(reference, dict) or not isinstance(
            reference.get("jsonFile"), str
        ):
            continue
        target = _json_object(reply / reference["jsonFile"])
        if target is None or not isinstance(target.get("name"), str):
            continue
        name = target["name"]
        source_names: set[str] = set()
        for source in target.get("sources", []):
            if not isinstance(source, dict) or not isinstance(source.get("path"), str):
                continue
            source_path = Path(source["path"])
            resolved = (
                source_path.resolve(strict=False)
                if source_path.is_absolute()
                else (root / source_path).resolve(strict=False)
            )
            try:
                source_names.add(resolved.relative_to(resolved_root).as_posix())
            except ValueError:
                continue
        artifacts: list[Path] = []
        for artifact in target.get("artifacts", []):
            if not isinstance(artifact, dict) or not isinstance(
                artifact.get("path"), str
            ):
                continue
            artifact_path = Path(artifact["path"])
            artifacts.append(
                artifact_path
                if artifact_path.is_absolute()
                else build_directory / artifact_path
            )
        configured[name] = ConfiguredTarget(
            sources=frozenset(source_names),
            artifacts=tuple(artifacts),
        )
    return configured, diagnostics


def parse_gtest_list_tests(output: str) -> frozenset[str]:
    """Parse GoogleTest's suite/test inventory into full runtime names."""

    suite: str | None = None
    tests: set[str] = set()
    for line in output.splitlines():
        content = line.split("#", 1)[0].rstrip()
        if not content:
            continue
        if not line[:1].isspace() and content.endswith("."):
            suite = content[:-1].strip()
            continue
        if suite is not None and line[:1].isspace():
            name = content.strip()
            if name:
                tests.add(f"{suite}.{name}")
    return frozenset(tests)


def parse_gtest_xml_result(output: str) -> ExecutedTestResult:
    """Parse GoogleTest XML without treating discovered tests as executed."""

    try:
        document = ET.fromstring(output)
    except ET.ParseError as error:
        raise ValueError(f"invalid GoogleTest XML: {error}") from error
    if document.tag != "testsuites":
        raise ValueError("GoogleTest XML root must be testsuites")

    counters: dict[str, int] = {}
    for field in ("tests", "failures", "errors", "disabled"):
        value = document.get(field)
        try:
            counter = int(value) if value is not None else -1
        except ValueError as error:
            raise ValueError(f"GoogleTest XML has invalid {field} count") from error
        if counter < 0:
            raise ValueError(f"GoogleTest XML has no valid {field} count")
        counters[field] = counter

    skipped_attribute = document.get("skipped", "0")
    try:
        skipped = int(skipped_attribute)
    except ValueError as error:
        raise ValueError("GoogleTest XML has invalid skipped count") from error
    if skipped < 0:
        raise ValueError("GoogleTest XML has no valid skipped count")

    runtime_names: set[str] = set()
    skipped_cases = 0
    suite_skipped = 0
    for testsuite in document.iter("testsuite"):
        value = testsuite.get("skipped", "0")
        try:
            count = int(value)
        except ValueError as error:
            raise ValueError(
                "GoogleTest XML has invalid testsuite skipped count"
            ) from error
        if count < 0:
            raise ValueError("GoogleTest XML has invalid testsuite skipped count")
        suite_skipped += count
    for testcase in document.iter("testcase"):
        if testcase.find("skipped") is not None or testcase.get("result") == "skipped":
            skipped_cases += 1
        if testcase.get("status") != "run" or testcase.get("result") != "completed":
            continue
        suite = testcase.get("classname")
        name = testcase.get("name")
        if isinstance(suite, str) and suite and isinstance(name, str) and name:
            runtime_names.add(f"{suite}.{name}")

    return ExecutedTestResult(
        runtime_names=frozenset(runtime_names),
        tests=counters["tests"],
        failures=counters["failures"],
        errors=counters["errors"],
        disabled=counters["disabled"],
        skipped=max(skipped, suite_skipped, skipped_cases),
    )


def parse_python_unittest_result(output: str) -> ExecutedTestResult:
    """Parse verbose unittest output, preserving every non-normal outcome."""

    ran_matches = re.findall(r"(?m)^Ran ([0-9]+) tests? in [^\r\n]+$", output)
    if not ran_matches:
        raise ValueError("Python unittest output has no Ran N tests summary")
    tests = int(ran_matches[-1])
    summary_matches = re.findall(r"(?m)^(OK|FAILED)(?: \(([^\r\n]*)\))?$", output)
    if not summary_matches:
        raise ValueError("Python unittest output has no final result summary")
    disposition, details = summary_matches[-1]
    counts: dict[str, int] = {}
    if details:
        for field, value in re.findall(r"([a-z ]+)=([0-9]+)", details):
            counts[field.strip()] = int(value)
    failures = counts.get("failures", 0) + counts.get("expected failures", 0)
    failures += counts.get("unexpected successes", 0)
    if disposition == "FAILED" and failures == 0 and counts.get("errors", 0) == 0:
        failures = 1

    runtime_names = frozenset(
        match.group(1)
        for match in re.finditer(
            r"(?m)^[^\r\n]*\(([A-Za-z_]\w*(?:\.[A-Za-z_]\w*)+)\) "
            r"\.\.\. (?:ok|FAIL|ERROR|skipped\b[^\r\n]*)$",
            output,
        )
    )
    return ExecutedTestResult(
        runtime_names=runtime_names,
        tests=tests,
        failures=failures,
        errors=counts.get("errors", 0),
        disabled=0,
        skipped=counts.get("skipped", 0),
    )


def _ctest_labels(test: dict[str, Any]) -> frozenset[str]:
    for prop in test.get("properties", []):
        if (
            isinstance(prop, dict)
            and prop.get("name") == "LABELS"
            and isinstance(prop.get("value"), list)
        ):
            return frozenset(value for value in prop["value"] if isinstance(value, str))
    return frozenset()


def _ctest_property(test: dict[str, Any], name: str) -> Any:
    for prop in test.get("properties", []):
        if isinstance(prop, dict) and prop.get("name") == name:
            return prop.get("value")
    return None


def _ctest_execution_context(
    test: dict[str, Any],
    build_directory: Path,
    *,
    allow_cmake_gtest_skip: bool = False,
) -> tuple[TestExecutionContext | None, str | None]:
    def configured_truth(value: Any) -> bool:
        if value is None:
            return False
        if isinstance(value, bool):
            return value
        if isinstance(value, (int, float)):
            return value != 0
        if isinstance(value, str):
            normalized = value.strip().upper()
            if normalized in {"", "0", "FALSE", "OFF", "NO", "N"}:
                return False
            return True
        if isinstance(value, list):
            return bool(value)
        return True

    for name in ("DISABLED", "WILL_FAIL"):
        if configured_truth(_ctest_property(test, name)):
            return None, f"CTest {name} changes normal pass semantics"

    for name in (
        "SKIP_RETURN_CODE",
        "PASS_REGULAR_EXPRESSION",
        "FAIL_REGULAR_EXPRESSION",
        "TIMEOUT_AFTER_MATCH",
        "REQUIRED_FILES",
        "FIXTURES_REQUIRED",
        "FIXTURES_SETUP",
        "FIXTURES_CLEANUP",
        "DEPENDS",
        "RESOURCE_GROUPS",
        "RESOURCE_LOCK",
    ):
        value = _ctest_property(test, name)
        if value not in (None, [], ""):
            return None, f"CTest {name} is not replayable"

    skip_regex = _ctest_property(test, "SKIP_REGULAR_EXPRESSION")
    canonical_gtest_skip = (
        isinstance(skip_regex, list)
        and bool(skip_regex)
        and all(
            isinstance(pattern, str) and pattern in CMAKE_GTEST_SKIP_PATTERNS
            for pattern in skip_regex
        )
    )
    if skip_regex not in (None, [], "") and not (
        allow_cmake_gtest_skip and canonical_gtest_skip
    ):
        return None, "CTest SKIP_REGULAR_EXPRESSION is not replayable"
    modifications = _ctest_property(test, "ENVIRONMENT_MODIFICATION")
    if modifications not in (None, [], ""):
        return None, "CTest ENVIRONMENT_MODIFICATION is not replayable"

    working_value = _ctest_property(test, "WORKING_DIRECTORY")
    if working_value is None:
        working_directory = build_directory
    elif isinstance(working_value, str) and working_value:
        working_directory = Path(working_value)
        if not working_directory.is_absolute():
            working_directory = build_directory / working_directory
    else:
        return None, "CTest WORKING_DIRECTORY is invalid"

    timeout_value = _ctest_property(test, "TIMEOUT")
    if timeout_value is None:
        timeout = 120.0
    elif (
        isinstance(timeout_value, (int, float))
        and not isinstance(timeout_value, bool)
        and timeout_value > 0
    ):
        timeout = float(timeout_value)
    else:
        return None, "CTest TIMEOUT is invalid"

    environment_value = _ctest_property(test, "ENVIRONMENT")
    if environment_value is None:
        entries: list[Any] = []
    elif isinstance(environment_value, list):
        entries = environment_value
    elif isinstance(environment_value, str):
        entries = [environment_value]
    else:
        return None, "CTest ENVIRONMENT is invalid"
    environment: dict[str, str] = {}
    for entry in entries:
        if not isinstance(entry, str) or "=" not in entry:
            return None, "CTest ENVIRONMENT has a non-assignment entry"
        key, value = entry.split("=", 1)
        if re.fullmatch(r"[A-Za-z_]\w*", key) is None:
            return None, f"CTest ENVIRONMENT has an invalid variable {key}"
        environment[key] = value
    return (
        TestExecutionContext(
            working_directory=working_directory.resolve(strict=False),
            environment=tuple(sorted(environment.items())),
            timeout=timeout,
        ),
        None,
    )


def _ctest_gtest_execution_key(
    entry: dict[str, Any],
    build_directory: Path,
    configured_artifacts: frozenset[Path],
) -> tuple[
    tuple[Path, TestExecutionContext, tuple[str, ...]] | None,
    str | None,
]:
    """Resolve one replayable CTest entry owned by the exact target artifact."""

    command = entry.get("command")
    if not isinstance(command, list) or not command or not all(
        isinstance(argument, str) for argument in command
    ):
        return None, "CTest command is invalid"

    raw_artifact = Path(command[0])
    if raw_artifact.is_absolute():
        artifact = raw_artifact.resolve(strict=False)
        # Duplicate GoogleTest names can accumulate labels from an aggregate
        # executable.  An absolute command for another codemodel artifact is
        # unambiguously not evidence for this manifest target and must not
        # poison its execution context.
        if artifact not in configured_artifacts:
            return None, None

    context, context_error = _ctest_execution_context(
        entry,
        build_directory,
        allow_cmake_gtest_skip=True,
    )
    if context_error is not None or context is None:
        return None, context_error or "has no context"

    artifact = (
        raw_artifact.resolve(strict=False)
        if raw_artifact.is_absolute()
        else (context.working_directory / raw_artifact).resolve(strict=False)
    )
    if artifact not in configured_artifacts:
        return None, None
    base_arguments = tuple(
        argument
        for argument in command[1:]
        if not argument.startswith("--gtest_filter=")
        and argument != "--gtest_also_run_disabled_tests"
        and not argument.startswith("--gtest_output=")
        and not argument.startswith("--gtest_color=")
    )
    return (artifact, context, base_arguments), None


def _execution_environment(context: TestExecutionContext) -> dict[str, str]:
    environment = dict(os.environ)
    environment.update(context.environment)
    for variable in (
        "GTEST_ALSO_RUN_DISABLED_TESTS",
        "GTEST_FILTER",
        "GTEST_OUTPUT",
        "GTEST_RANDOM_SEED",
        "GTEST_REPEAT",
        "GTEST_SHARD_INDEX",
        "GTEST_SHARD_STATUS_FILE",
        "GTEST_SHUFFLE",
        "GTEST_TOTAL_SHARDS",
        "TEST_PREMATURE_EXIT_FILE",
    ):
        environment.pop(variable, None)
    return environment


def _process_failure_detail(completed: subprocess.CompletedProcess[str]) -> str:
    output = (completed.stderr or completed.stdout).strip()
    if len(output) > 800:
        output = output[-800:]
    return output or f"exit {completed.returncode}"


def _python_sources_from_command(command: list[str], root: Path) -> frozenset[str]:
    unittest_indices = [
        index
        for index in range(1, len(command))
        if command[index] == "unittest" and command[index - 1] == "-m"
    ]
    if len(unittest_indices) != 1:
        return frozenset()
    arguments = command[unittest_indices[0] + 1 :]
    sources: set[str] = set()
    if arguments and arguments[0] == "discover":
        if "-s" not in arguments or arguments.index("-s") + 1 >= len(arguments):
            return frozenset()
        directory = Path(arguments[arguments.index("-s") + 1])
        if not directory.is_absolute():
            directory = root / directory
        pattern = "test*.py"
        if "-p" in arguments and arguments.index("-p") + 1 < len(arguments):
            pattern = arguments[arguments.index("-p") + 1]
        if directory.is_dir():
            for path in directory.glob(pattern):
                try:
                    sources.add(path.resolve().relative_to(root.resolve()).as_posix())
                except ValueError:
                    continue
        return frozenset(sources)
    for module in arguments:
        if re.fullmatch(r"[A-Za-z_]\w*(?:\.[A-Za-z_]\w*)+", module) is None:
            continue
        parts = module.split(".")
        for end in range(len(parts), 0, -1):
            path = root / Path(*parts[:end]).with_suffix(".py")
            if path.is_file():
                sources.add(path.relative_to(root).as_posix())
                break
    return frozenset(sources)


def _iter_manifest_evidence(
    document: dict[str, Any],
) -> list[tuple[str, dict[str, Any]]]:
    evidence: list[tuple[str, dict[str, Any]]] = []
    for capability_index, capability in enumerate(document.get("capabilities", [])):
        if not isinstance(capability, dict):
            continue
        for test_index, test in enumerate(capability.get("tests", [])):
            if isinstance(test, dict):
                evidence.append(
                    (f"capabilities[{capability_index}].tests[{test_index}]", test)
                )
        for field in EVIDENCE_FIELDS:
            test = capability.get(field)
            if isinstance(test, dict):
                evidence.append((f"capabilities[{capability_index}].{field}", test))
    return evidence


def _terminate_build_process_tree(process: subprocess.Popen[bytes]) -> None:
    """Stop a timed-out command and every process it spawned."""

    if os.name == "nt":
        try:
            subprocess.run(
                ("taskkill", "/PID", str(process.pid), "/T", "/F"),
                check=False,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=15,
            )
        except (OSError, subprocess.TimeoutExpired):
            pass
    else:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except (ProcessLookupError, PermissionError):
            pass
        deadline = time.monotonic() + 0.5
        while time.monotonic() < deadline:
            try:
                os.killpg(process.pid, 0)
            except ProcessLookupError:
                break
            except PermissionError:
                # The group still exists even if this process can no longer
                # probe every member; retain the deadline and force-kill it.
                pass
            time.sleep(0.05)
        else:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def _run_bounded_process(
    command: tuple[str, ...] | list[str],
    *,
    timeout: float,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
    output_limit: int = EVIDENCE_OUTPUT_LIMIT,
    tail_bytes: int | None = None,
) -> tuple[subprocess.CompletedProcess[str] | None, str | None]:
    """Run with live output bounds and process-tree-safe failure handling."""

    creation_flags = (
        getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0) if os.name == "nt" else 0
    )
    with tempfile.TemporaryFile() as stdout_log, tempfile.TemporaryFile() as stderr_log:
        try:
            process = subprocess.Popen(
                command,
                cwd=cwd,
                env=env,
                stdout=stdout_log,
                stderr=stderr_log,
                start_new_session=os.name != "nt",
                creationflags=creation_flags,
            )
        except OSError as error:
            return None, f"cannot start: {error}"

        deadline = time.monotonic() + timeout
        failure: str | None = None
        while True:
            stdout_size = os.fstat(stdout_log.fileno()).st_size
            stderr_size = os.fstat(stderr_log.fileno()).st_size
            total_size = stdout_size + stderr_size
            if total_size > output_limit:
                if process.poll() is None:
                    _terminate_build_process_tree(process)
                final_size = (
                    os.fstat(stdout_log.fileno()).st_size
                    + os.fstat(stderr_log.fileno()).st_size
                )
                failure = (
                    f"combined output exceeded {output_limit}-byte limit "
                    f"({final_size} bytes)"
                )
                break
            return_code = process.poll()
            if return_code is not None:
                break
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                _terminate_build_process_tree(process)
                failure = f"timed out after {timeout:g} seconds"
                break
            time.sleep(min(0.01, remaining))

        def read_log(log: Any) -> str:
            size = os.fstat(log.fileno()).st_size
            if tail_bytes is not None:
                log.seek(max(0, size - tail_bytes), os.SEEK_SET)
            else:
                log.seek(0)
            return log.read().decode(encoding="utf-8", errors="replace")

        if failure is not None:
            if tail_bytes is not None:
                stdout = read_log(stdout_log)
                stderr = read_log(stderr_log)
                detail = (stderr or stdout).strip()
                if detail:
                    failure += f": {detail}"
            return None, failure

        stdout = read_log(stdout_log)
        stderr = read_log(stderr_log)
    return subprocess.CompletedProcess(command, return_code, stdout, stderr), None


def build_configured_evidence(
    document: dict[str, Any],
    build_directory: Path,
    *,
    build_config: str | None = None,
    timeout: float = 3600.0,
) -> list[str]:
    """Build the live graph, including evidence excluded from the all target."""

    base_command = ["cmake", "--build", str(build_directory)]
    if build_config is not None:
        base_command.extend(("--config", build_config))
    current_platform = current_test_platform()
    evidence_targets: set[str] = set()
    for label, evidence in _iter_manifest_evidence(document):
        if (
            evidence.get("runner") != "gtest"
            or not isinstance(evidence.get("platforms"), list)
            or current_platform not in evidence["platforms"]
        ):
            continue
        target = evidence.get("target")
        if (
            not isinstance(target, str)
            or CMAKE_LOGICAL_TARGET.fullmatch(target) is None
        ):
            return [f"{label}.target: unsafe CMake logical target name"]
        evidence_targets.add(target)
    phases: list[tuple[str, list[str]]] = [("default build", base_command)]
    if evidence_targets:
        phases.append(
            (
                "evidence target build",
                [*base_command, "--target", *sorted(evidence_targets)],
            )
        )

    for phase, command in phases:
        completed, process_error = _run_bounded_process(
            command,
            timeout=timeout,
            output_limit=BUILD_OUTPUT_LIMIT,
            tail_bytes=1200,
        )
        if process_error is not None or completed is None:
            detail = process_error or "unknown process error"
            if detail.startswith("cannot start: "):
                return [
                    f"build audit: cannot run {phase}: "
                    f"{detail.removeprefix('cannot start: ')}"
                ]
            return [f"build audit: {phase} {detail}"]
        if completed.returncode != 0:
            detail = _process_failure_detail(completed)
            return [
                f"build audit: {phase} failed with exit "
                f"{completed.returncode}: {detail}"
            ]
    return []


def audit_configured_evidence(
    document: dict[str, Any],
    root: Path,
    build_directory: Path,
    *,
    build_config: str | None = None,
    manifest_path: Path | None = None,
) -> list[str]:
    """Execute active evidence against one fresh configured build snapshot."""

    build_directory = build_directory.resolve()
    diagnostics = audit_configured_build_identity(root, build_directory)
    if diagnostics:
        return diagnostics
    diagnostics = build_configured_evidence(
        document,
        build_directory,
        build_config=build_config,
    )
    if diagnostics:
        return diagnostics
    diagnostics = audit_build_freshness(
        root,
        build_directory,
        document,
        manifest_path=manifest_path,
    )
    configured, configured_diagnostics = collect_configured_targets(
        root, build_directory, build_config=build_config
    )
    diagnostics.extend(configured_diagnostics)
    if configured_diagnostics:
        return sorted(diagnostics)
    command = ["ctest", "--test-dir", str(build_directory)]
    if build_config is not None:
        command.extend(("--build-config", build_config))
    command.append("--show-only=json-v1")
    completed, process_error = _run_bounded_process(
        command,
        timeout=180,
        output_limit=CTEST_INVENTORY_OUTPUT_LIMIT,
    )
    if process_error is not None or completed is None:
        return [
            "build audit: cannot read CTest inventory: "
            f"{process_error or 'unknown process error'}"
        ]
    if completed.returncode != 0:
        return [
            "build audit: CTest inventory failed: "
            + (completed.stderr.strip() or f"exit {completed.returncode}")
        ]
    try:
        ctest_document = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        return [f"build audit: invalid CTest JSON inventory: {error}"]
    ctest_tests = (
        ctest_document.get("tests", []) if isinstance(ctest_document, dict) else []
    )
    if not isinstance(ctest_tests, list):
        return ["build audit: CTest JSON inventory has no tests array"]

    ctest_by_label: dict[str, list[dict[str, Any]]] = {}
    ctest_by_name: dict[str, list[dict[str, Any]]] = {}
    for entry in ctest_tests:
        if not isinstance(entry, dict):
            continue
        name = entry.get("name")
        if isinstance(name, str):
            ctest_by_name.setdefault(name, []).append(entry)
        for label in _ctest_labels(entry):
            ctest_by_label.setdefault(label, []).append(entry)

    current_platform = current_test_platform()
    gtest_groups: dict[
        tuple[str, Path, TestExecutionContext, tuple[str, ...]],
        dict[str, set[str]],
    ] = {}
    python_groups: dict[
        tuple[str, str, tuple[str, ...], TestExecutionContext],
        dict[str, set[str]],
    ] = {}
    freshness_checked: set[tuple[str, Path]] = set()
    for label, test in _iter_manifest_evidence(document):
        platforms = test.get("platforms")
        if not isinstance(platforms, list) or current_platform not in platforms:
            continue
        runner = test.get("runner")
        target_name = test.get("target")
        source = test.get("source")
        test_filter = test.get("filter")
        if not all(
            isinstance(value, str)
            for value in (runner, target_name, source, test_filter)
        ):
            continue
        matching_ctest = list(ctest_by_label.get(target_name, ()))
        if runner == "python-unittest":
            for entry in ctest_by_name.get(target_name, ()):
                if not any(entry is existing for existing in matching_ctest):
                    matching_ctest.append(entry)
            source_entries: list[dict[str, Any]] = []
            for entry in matching_ctest:
                entry_command = entry.get("command")
                if not isinstance(entry_command, list) or not all(
                    isinstance(argument, str) for argument in entry_command
                ):
                    continue
                if source in _python_sources_from_command(entry_command, root):
                    source_entries.append(entry)
            if not source_entries:
                diagnostics.append(
                    f"{label}: configured CTest target {target_name} does not "
                    f"execute {source}"
                )
                continue

            execution_keys: set[tuple[tuple[str, ...], TestExecutionContext]] = set()
            for entry in source_entries:
                entry_command = entry["command"]
                assert isinstance(entry_command, list)
                context, context_error = _ctest_execution_context(
                    entry, build_directory
                )
                if context_error is not None or context is None:
                    diagnostics.append(
                        f"{label}: {target_name} {context_error or 'has no context'}"
                    )
                    continue
                unittest_indices = [
                    index
                    for index in range(1, len(entry_command))
                    if entry_command[index] == "unittest"
                    and entry_command[index - 1] == "-m"
                ]
                if len(unittest_indices) != 1:
                    diagnostics.append(
                        f"{label}: {target_name} is not a Python unittest command"
                    )
                    continue
                unittest_index = unittest_indices[0]
                execution_keys.add(
                    (tuple(entry_command[: unittest_index + 1]), context)
                )
            if len(execution_keys) != 1:
                if execution_keys:
                    diagnostics.append(
                        f"{label}: {target_name} has inconsistent Python "
                        "execution contexts"
                    )
                continue
            prefix, context = next(iter(execution_keys))
            group = python_groups.setdefault((target_name, source, prefix, context), {})
            group.setdefault(test_filter, set()).add(label)
            continue

        target = configured.get(target_name)
        if target is None:
            diagnostics.append(
                f"{label}: configured codemodel has no target {target_name}"
            )
            continue
        if source not in target.sources:
            diagnostics.append(
                f"{label}: configured target {target_name} does not own {source}"
            )
        configured_artifacts = frozenset(
            path.resolve(strict=False) for path in target.artifacts if path.is_file()
        )
        if not configured_artifacts:
            diagnostics.append(
                f"{label}: configured target {target_name} has no built artifact"
            )
            continue
        runtime_entries: list[dict[str, Any]] = []
        for entry in matching_ctest:
            entry_command = entry.get("command")
            if not isinstance(entry_command, list) or not all(
                isinstance(argument, str) for argument in entry_command
            ):
                continue
            runtime_filters = [
                argument.removeprefix("--gtest_filter=")
                for argument in entry_command
                if argument.startswith("--gtest_filter=")
            ]
            if any(
                fnmatch.fnmatchcase(runtime_filter, test_filter)
                for runtime_filter in runtime_filters
            ):
                runtime_entries.append(entry)
        if not runtime_entries:
            diagnostics.append(
                f"{label}: CTest inventory has no runtime test matching "
                f"{target_name} {test_filter}"
            )
            continue

        execution_keys: set[tuple[Path, TestExecutionContext, tuple[str, ...]]] = set()
        for entry in runtime_entries:
            execution_key, context_error = _ctest_gtest_execution_key(
                entry,
                build_directory,
                configured_artifacts,
            )
            if context_error is not None:
                diagnostics.append(
                    f"{label}: {target_name} {context_error}"
                )
                continue
            if execution_key is not None:
                execution_keys.add(execution_key)
        if len(execution_keys) != 1:
            if execution_keys:
                diagnostics.append(
                    f"{label}: {target_name} has inconsistent GoogleTest "
                    "execution contexts"
                )
            else:
                diagnostics.append(
                    f"{label}: CTest inventory has no replayable runtime test "
                    f"for built artifact of {target_name}"
                )
            continue
        artifact, context, base_arguments = next(iter(execution_keys))

        freshness_key = (target_name, artifact)
        if freshness_key not in freshness_checked:
            freshness_checked.add(freshness_key)
            try:
                artifact_time = artifact.stat().st_mtime_ns
            except OSError as error:
                diagnostics.append(
                    f"{label}: cannot stat built artifact {artifact}: {error}"
                )
            else:
                for owned_source in sorted(target.sources):
                    owned_path = root / owned_source
                    if not owned_path.is_file():
                        continue
                    try:
                        stale = artifact_time < owned_path.stat().st_mtime_ns
                    except OSError:
                        continue
                    if stale:
                        diagnostics.append(
                            f"{label}: built artifact {target_name} predates owned "
                            f"source {owned_source}"
                        )
        group = gtest_groups.setdefault(
            (target_name, artifact, context, base_arguments), {}
        )
        group.setdefault(test_filter, set()).add(label)

    with tempfile.TemporaryDirectory(prefix="neverd-capability-evidence-") as temp:
        temporary = Path(temp)
        sorted_gtest_groups = sorted(
            gtest_groups.items(),
            key=lambda item: (item[0][0], str(item[0][1]), item[0][3]),
        )
        for group_index, (group_key, claims) in enumerate(sorted_gtest_groups):
            target_name, artifact, context, base_arguments = group_key
            list_command = (
                str(artifact),
                *base_arguments,
                "--gtest_list_tests",
                "--gtest_color=no",
            )
            listed, process_error = _run_bounded_process(
                list_command,
                cwd=context.working_directory,
                env=_execution_environment(context),
                timeout=context.timeout,
            )
            if process_error is not None or listed is None:
                diagnostics.append(
                    f"build audit: cannot list {target_name} tests: "
                    f"{process_error or 'unknown process error'}"
                )
                continue
            if listed.returncode != 0:
                diagnostics.append(
                    f"build audit: {target_name} --gtest_list_tests failed: "
                    f"{_process_failure_detail(listed)}"
                )
                continue
            inventory = parse_gtest_list_tests(listed.stdout)
            selected_inventory = {
                name
                for name in inventory
                if any(fnmatch.fnmatchcase(name, test_filter) for test_filter in claims)
            }
            for test_filter, labels in claims.items():
                if not any(
                    fnmatch.fnmatchcase(name, test_filter) for name in inventory
                ):
                    for label in sorted(labels):
                        diagnostics.append(
                            f"{label}: built {target_name} has no runtime test "
                            f"matching {test_filter}"
                        )

            xml_path = temporary / f"gtest-{group_index}.xml"
            joined_filter = ":".join(sorted(claims))
            run_command = (
                str(artifact),
                *base_arguments,
                f"--gtest_filter={joined_filter}",
                "--gtest_color=no",
                "--gtest_repeat=1",
                "--gtest_shuffle=0",
                "--gtest_also_run_disabled_tests=0",
                f"--gtest_output=xml:{xml_path}",
            )
            completed, process_error = _run_bounded_process(
                run_command,
                cwd=context.working_directory,
                env=_execution_environment(context),
                timeout=context.timeout * max(1, len(selected_inventory)),
            )
            if process_error is not None or completed is None:
                diagnostics.append(
                    f"build audit: cannot execute {target_name} evidence: "
                    f"{process_error or 'unknown process error'}"
                )
                continue
            if completed.returncode != 0:
                diagnostics.append(
                    f"build audit: {target_name} evidence exited "
                    f"{completed.returncode}: {_process_failure_detail(completed)}"
                )
            try:
                xml_output = _read_utf8_with_limit(
                    xml_path,
                    limit=GTEST_XML_OUTPUT_LIMIT,
                    description="GoogleTest result",
                )
                result = parse_gtest_xml_result(xml_output)
            except (OSError, UnicodeError, ValueError) as error:
                diagnostics.append(
                    f"build audit: {target_name} produced no valid GoogleTest "
                    f"result: {error}"
                )
                continue
            if result.tests < 1:
                diagnostics.append(
                    f"build audit: {target_name} evidence executed zero tests"
                )
            if result.failures or result.errors or result.disabled or result.skipped:
                diagnostics.append(
                    f"build audit: {target_name} evidence was not a clean pass "
                    f"(failures={result.failures}, errors={result.errors}, "
                    f"disabled={result.disabled}, skipped={result.skipped})"
                )
            if len(result.runtime_names) != result.tests:
                diagnostics.append(
                    f"build audit: {target_name} did not identify every executed "
                    f"test ({len(result.runtime_names)} names for {result.tests} tests)"
                )
            for test_filter, labels in claims.items():
                if not any(
                    fnmatch.fnmatchcase(name, test_filter)
                    for name in result.runtime_names
                ):
                    for label in sorted(labels):
                        diagnostics.append(
                            f"{label}: {target_name} executed no test matching "
                            f"{test_filter}"
                        )

        sorted_python_groups = sorted(
            python_groups.items(),
            key=lambda item: (item[0][0], item[0][1], item[0][2]),
        )
        for group_key, claims in sorted_python_groups:
            target_name, source, prefix, context = group_key
            source_path = (root / source).resolve(strict=False)
            run_command: list[str] = [
                *prefix,
                "discover",
                "-s",
                str(source_path.parent),
                "-p",
                source_path.name,
            ]
            for test_filter in sorted(claims):
                run_command.extend(("-k", test_filter))
            run_command.append("-v")
            completed, process_error = _run_bounded_process(
                run_command,
                cwd=context.working_directory,
                env=_execution_environment(context),
                timeout=context.timeout * max(1, len(claims)),
            )
            if process_error is not None or completed is None:
                diagnostics.append(
                    f"build audit: cannot execute {target_name} evidence: "
                    f"{process_error or 'unknown process error'}"
                )
                continue
            output = completed.stdout + "\n" + completed.stderr
            if completed.returncode != 0:
                diagnostics.append(
                    f"build audit: {target_name} evidence exited "
                    f"{completed.returncode}: {_process_failure_detail(completed)}"
                )
            try:
                result = parse_python_unittest_result(output)
            except ValueError as error:
                diagnostics.append(
                    f"build audit: {target_name} produced no valid unittest "
                    f"result: {error}"
                )
                continue
            if result.tests < 1:
                diagnostics.append(
                    f"build audit: {target_name} evidence executed zero tests"
                )
            if result.failures or result.errors or result.skipped:
                diagnostics.append(
                    f"build audit: {target_name} evidence was not a clean pass "
                    f"(failures={result.failures}, errors={result.errors}, "
                    f"skipped={result.skipped})"
                )
            if len(result.runtime_names) != result.tests:
                diagnostics.append(
                    f"build audit: {target_name} did not identify every executed "
                    f"test ({len(result.runtime_names)} names for {result.tests} tests)"
                )
            for test_filter, labels in claims.items():
                suffix = "." + test_filter
                if not any(
                    name == test_filter or name.endswith(suffix)
                    for name in result.runtime_names
                ):
                    for label in sorted(labels):
                        diagnostics.append(
                            f"{label}: {target_name} executed no test matching "
                            f"{test_filter}"
                        )
    return sorted(set(diagnostics))


def _repository_file(root: Path, value: str) -> tuple[Path | None, str | None]:
    relative = PurePosixPath(value)
    if (
        "\\" in value
        or relative.is_absolute()
        or relative.as_posix() != value
        or ".." in relative.parts
    ):
        return None, "noncanonical"

    resolved_root = root.resolve()
    resolved = (root / value).resolve(strict=False)
    try:
        resolved.relative_to(resolved_root)
    except ValueError:
        return None, "noncanonical"
    if not resolved.is_file():
        return None, "missing"
    return resolved, None


def _gtest_filter_identity(test_filter: str) -> str | None:
    for pattern in GTEST_FILTER_PATTERNS:
        match = pattern.fullmatch(test_filter)
        if match is not None:
            return f"{match.group(1)}.{match.group(2)}"
    return None


def _target_owned_support_platforms(
    *,
    support: GTestSupport,
    support_key: tuple[str, str, str],
    target: str,
    cmake_sources: dict[tuple[str, str], frozenset[str]],
) -> frozenset[str]:
    platforms: set[str] = set()
    for source, source_platforms in support.get(support_key, {}).items():
        ownership_platforms = cmake_sources.get((target, source))
        if ownership_platforms is not None:
            platforms.update(source_platforms & ownership_platforms)
    return frozenset(platforms)


def _gtest_execution_platforms(
    declaration: TestDeclaration,
    *,
    target: str,
    support: GTestSupport,
    cmake_sources: dict[tuple[str, str], frozenset[str]],
) -> tuple[frozenset[str], tuple[str, ...]]:
    """Close declaration -> registration -> instantiation for one target."""

    requirements: tuple[tuple[tuple[str, str, str], str], ...]
    if declaration.kind in {"TEST", "TEST_F"}:
        requirements = ()
    elif declaration.kind == "TEST_P":
        requirements = (
            (
                ("TEST_P", declaration.suite, ""),
                "INSTANTIATE_TEST_SUITE_P",
            ),
        )
    elif declaration.kind == "TYPED_TEST":
        requirements = (
            (
                ("TYPED_TEST", declaration.suite, ""),
                "TYPED_TEST_SUITE",
            ),
        )
    elif declaration.kind == "TYPED_TEST_P":
        requirements = (
            (
                (
                    "TYPED_TEST_P_REGISTER",
                    declaration.suite,
                    declaration.name,
                ),
                "REGISTER_TYPED_TEST_SUITE_P",
            ),
            (
                ("TYPED_TEST_P", declaration.suite, ""),
                "INSTANTIATE_TYPED_TEST_SUITE_P",
            ),
        )
    else:
        return declaration.platforms, ()

    executable_platforms = declaration.platforms
    missing: list[str] = []
    for support_key, macro in requirements:
        requirement_platforms = _target_owned_support_platforms(
            support=support,
            support_key=support_key,
            target=target,
            cmake_sources=cmake_sources,
        )
        if not requirement_platforms:
            missing.append(macro)
        executable_platforms &= requirement_platforms
    return executable_platforms, tuple(missing)


def _validate_test_evidence(
    diagnostics: list[str],
    *,
    label: str,
    evidence: Any,
    root: Path,
    declarations: dict[tuple[str, str], TestDeclaration],
    gtest_support: GTestSupport,
    cmake_sources: dict[tuple[str, str], frozenset[str]],
    python_sources: dict[tuple[str, str], frozenset[str]],
) -> frozenset[str]:
    diagnostic_start = len(diagnostics)
    if not isinstance(evidence, dict):
        diagnostics.append(f"{label}: expected an object")
        return frozenset()
    for key in sorted(TEST_EVIDENCE_KEYS - set(evidence)):
        diagnostics.append(f"{label}: missing {key}")
    for key in sorted(set(evidence) - TEST_EVIDENCE_KEYS):
        diagnostics.append(f"{label}: unexpected field {key}")
    for key in ("target", "source", "filter", "runner"):
        if key in evidence and (
            not isinstance(evidence[key], str) or not evidence[key].strip()
        ):
            diagnostics.append(f"{label}.{key}: expected a non-empty string")
    platforms_value = evidence.get("platforms")
    if "platforms" in evidence and not isinstance(platforms_value, list):
        diagnostics.append(f"{label}.platforms: expected an array")
    elif isinstance(platforms_value, list):
        seen_platforms: set[str] = set()
        for platform_index, platform in enumerate(platforms_value):
            if not isinstance(platform, str) or platform not in TEST_PLATFORMS:
                diagnostics.append(
                    f"{label}.platforms[{platform_index}]: expected one of "
                    "darwin, linux, windows"
                )
            elif platform in seen_platforms:
                diagnostics.append(f"{label}.platforms: duplicate entry {platform}")
            else:
                seen_platforms.add(platform)
        if not platforms_value:
            diagnostics.append(f"{label}.platforms: expected at least one platform")
        elif all(
            isinstance(platform, str) and platform in TEST_PLATFORMS
            for platform in platforms_value
        ):
            canonical = [
                platform for platform in TEST_PLATFORMS if platform in seen_platforms
            ]
            if platforms_value != canonical:
                diagnostics.append(
                    f"{label}.platforms: expected canonical order "
                    "darwin, linux, windows"
                )

    if set(evidence) != TEST_EVIDENCE_KEYS:
        return frozenset()
    if any(
        not isinstance(evidence.get(key), str) or not evidence[key].strip()
        for key in ("target", "source", "filter", "runner")
    ) or not isinstance(platforms_value, list):
        return frozenset()
    if not platforms_value or any(
        not isinstance(platform, str) or platform not in TEST_PLATFORMS
        for platform in platforms_value
    ):
        return frozenset()

    runner = evidence["runner"]
    if runner not in TEST_RUNNERS:
        diagnostics.append(f"{label}.runner: expected one of gtest, python-unittest")
        return frozenset()
    target = evidence["target"]
    if runner == "gtest" and CMAKE_LOGICAL_TARGET.fullmatch(target) is None:
        diagnostics.append(f"{label}.target: expected a safe CMake logical target name")
        return frozenset()
    source = evidence["source"]
    _source_path, path_error = _repository_file(root, source)
    if path_error == "noncanonical":
        diagnostics.append(
            f"{label}.source: path must be a canonical repository-relative "
            f"POSIX path: {source}"
        )
        return frozenset()
    if path_error == "missing":
        diagnostics.append(f"{label}.source: path does not exist: {source}")
        return frozenset()

    ownership = cmake_sources if runner == "gtest" else python_sources
    registered_platforms = ownership.get((target, source))
    if registered_platforms is None:
        diagnostics.append(
            f"{label}.source: {source} is not registered to {runner} target {target}"
        )
    claimed_platforms = frozenset(platforms_value)
    # Evidence may deliberately claim a narrower applicability set than the
    # build (for example a native Darwin publication test in a portable TU),
    # but it may never claim an OS on which either the target or test vanishes.
    if (
        registered_platforms is not None
        and not claimed_platforms <= registered_platforms
    ):
        unavailable = ", ".join(
            platform
            for platform in TEST_PLATFORMS
            if platform in claimed_platforms and platform not in registered_platforms
        )
        diagnostics.append(
            f"{label}.platforms: target {target} does not register {source} on "
            f"{unavailable}"
        )

    test_filter = evidence["filter"]
    declaration_identity = (
        _gtest_filter_identity(test_filter) if runner == "gtest" else test_filter
    )
    declaration = (
        declarations.get((source, declaration_identity))
        if declaration_identity is not None
        else None
    )
    if declaration is None:
        diagnostics.append(f"{label}.filter: not found in {source}: {test_filter}")
        return frozenset()

    if test_filter != declaration.canonical_filter:
        diagnostics.append(
            f"{label}.filter: {declaration.kind} requires canonical executable "
            f"filter {declaration.canonical_filter}; got {test_filter}"
        )
        return frozenset()

    executable_platforms = declaration.platforms
    if runner == "gtest":
        executable_platforms, missing_support = _gtest_execution_platforms(
            declaration,
            target=target,
            support=gtest_support,
            cmake_sources=cmake_sources,
        )
        for macro in missing_support:
            diagnostics.append(
                f"{label}.filter: {declaration.kind} {declaration.suite}."
                f"{declaration.name} has no target-owned {macro} in {target}"
            )

    if not claimed_platforms <= executable_platforms:
        unavailable = ", ".join(
            platform
            for platform in TEST_PLATFORMS
            if platform in claimed_platforms and platform not in executable_platforms
        )
        availability = (
            "compiled"
            if declaration.kind in {"TEST", "TEST_F", "PYTHON_UNITTEST"}
            else "executable"
        )
        diagnostics.append(
            f"{label}.platforms: {test_filter} is not {availability} on {unavailable}"
        )
    if len(diagnostics) != diagnostic_start:
        return frozenset()
    return claimed_platforms


def validate_manifest(
    document: Any,
    root: Path = REPO_ROOT,
    *,
    required_capability_ids: frozenset[str] | set[str] = frozenset(),
    enforce_surface_completeness: bool | None = None,
) -> list[str]:
    """Return deterministic diagnostics for an in-memory manifest."""

    if not isinstance(document, dict):
        return ["manifest: expected an object"]
    diagnostics: list[str] = []
    top_level_fields = {"schema", "capabilities", "surface_exclusions"}
    for field in sorted(set(document) - top_level_fields):
        diagnostics.append(f"unexpected top-level field: {field}")
    if (
        type(document.get("schema")) is not int
        or document.get("schema") != SCHEMA_VERSION
    ):
        diagnostics.append(f"schema: expected integer {SCHEMA_VERSION}")
    if "capabilities" not in document:
        diagnostics.append("missing top-level field: capabilities")
    rows = document.get("capabilities", [])
    if not isinstance(rows, list):
        diagnostics.append("capabilities: expected an array")
        return sorted(diagnostics)
    if (
        "capabilities" in document
        and not rows
        and type(document.get("schema")) is int
        and document.get("schema") == SCHEMA_VERSION
        and set(document) <= top_level_fields
    ):
        diagnostics.append("capabilities: expected at least one capability")

    excluded_surfaces: dict[str, set[str]] = {
        surface: set() for surface in PUBLIC_SURFACES
    }
    exclusions = document.get("surface_exclusions")
    if "surface_exclusions" in document and not isinstance(exclusions, dict):
        diagnostics.append("surface_exclusions: expected an object")
    elif isinstance(exclusions, dict):
        for surface in sorted(PUBLIC_SURFACES - set(exclusions)):
            diagnostics.append(f"surface_exclusions: missing {surface}")
        for surface in sorted(set(exclusions) - PUBLIC_SURFACES):
            diagnostics.append(f"surface_exclusions: unexpected field {surface}")
        for surface in sorted(PUBLIC_SURFACES & set(exclusions)):
            groups = exclusions[surface]
            if not isinstance(groups, list):
                diagnostics.append(f"surface_exclusions.{surface}: expected an array")
                continue
            for group_index, group in enumerate(groups):
                label = f"surface_exclusions.{surface}[{group_index}]"
                if not isinstance(group, dict):
                    diagnostics.append(f"{label}: expected an object")
                    continue
                for key in sorted(SURFACE_EXCLUSION_KEYS - set(group)):
                    diagnostics.append(f"{label}: missing {key}")
                for key in sorted(set(group) - SURFACE_EXCLUSION_KEYS):
                    diagnostics.append(f"{label}: unexpected field {key}")
                for key in ("owner", "reason"):
                    value = group.get(key)
                    if not isinstance(value, str) or not value.strip():
                        diagnostics.append(
                            f"{label}.{key}: expected a non-empty string"
                        )
                names = group.get("names")
                if not isinstance(names, list):
                    diagnostics.append(f"{label}.names: expected an array")
                    continue
                if not names:
                    diagnostics.append(
                        f"{label}.names: expected at least one exact name"
                    )
                valid_names: list[str] = []
                for name_index, name in enumerate(names):
                    name_label = f"{label}.names[{name_index}]"
                    if not isinstance(name, str) or not name.strip():
                        diagnostics.append(f"{name_label}: expected a non-empty string")
                        continue
                    if any(marker in name for marker in ("*", "?", "[")):
                        diagnostics.append(
                            f"{name_label}: exclusions must use exact names"
                        )
                        continue
                    if name in excluded_surfaces[surface]:
                        diagnostics.append(
                            f"surface_exclusions.{surface}: duplicate entry {name}"
                        )
                    excluded_surfaces[surface].add(name)
                    valid_names.append(name)
                if valid_names != sorted(valid_names):
                    diagnostics.append(
                        f"{label}.names: expected canonical lexical order"
                    )

    seen_ids: set[str] = set()
    duplicate_ids: set[str] = set()
    test_declarations: dict[tuple[str, str], TestDeclaration] | None = None
    gtest_support: GTestSupport | None = None
    cmake_test_sources: dict[tuple[str, str], frozenset[str]] | None = None
    python_test_sources: dict[tuple[str, str], frozenset[str]] | None = None
    declared_surfaces: dict[str, frozenset[str]] | None = None
    claimed_surfaces: dict[str, set[str]] = {
        surface: set() for surface in PUBLIC_SURFACES
    }
    for index, capability in enumerate(rows):
        if not isinstance(capability, dict):
            diagnostics.append(f"capabilities[{index}]: expected an object")
            continue
        for field in sorted(set(capability) - CAPABILITY_FIELDS):
            diagnostics.append(f"capabilities[{index}]: unexpected field {field}")
        for field in sorted(REQUIRED_CAPABILITY_FIELDS - set(capability)):
            diagnostics.append(f"capabilities[{index}]: missing {field}")
        for field in ("id", "kind", "owner"):
            if field in capability and (
                not isinstance(capability[field], str) or not capability[field].strip()
            ):
                diagnostics.append(
                    f"capabilities[{index}].{field}: expected a non-empty string"
                )
        capability_id = capability.get("id")
        if isinstance(capability_id, str) and capability_id.strip():
            if capability_id in seen_ids:
                duplicate_ids.add(capability_id)
            seen_ids.add(capability_id)
        kind = capability.get("kind")
        if isinstance(kind, str) and kind.strip():
            if kind not in CAPABILITY_KINDS:
                diagnostics.append(
                    f"capabilities[{index}].kind: expected one of analysis, "
                    "debugging, derivation, execution, rewrite, synthesis, "
                    "translation"
                )
            expected_kind = (
                REQUIRED_CAPABILITY_KINDS.get(capability_id)
                if isinstance(capability_id, str)
                else None
            )
            if expected_kind is not None and kind != expected_kind:
                diagnostics.append(
                    f"capabilities[{index}].kind: {capability_id} requires "
                    f"kind {expected_kind}"
                )
        primary_evidence_platforms: set[str] = set()
        if "public_surfaces" in capability:
            surfaces = capability["public_surfaces"]
            if not isinstance(surfaces, dict):
                diagnostics.append(
                    f"capabilities[{index}].public_surfaces: expected an object"
                )
            else:
                for field in sorted(PUBLIC_SURFACES - set(surfaces)):
                    diagnostics.append(
                        f"capabilities[{index}].public_surfaces: missing {field}"
                    )
                for field in sorted(set(surfaces) - PUBLIC_SURFACES):
                    diagnostics.append(
                        f"capabilities[{index}].public_surfaces: "
                        f"unexpected field {field}"
                    )
                for field in sorted(PUBLIC_SURFACES & set(surfaces)):
                    if not isinstance(surfaces[field], list):
                        diagnostics.append(
                            f"capabilities[{index}].public_surfaces.{field}: "
                            "expected an array"
                        )
                        continue
                    seen_surface_entries: set[str] = set()
                    duplicate_surface_entries: set[str] = set()
                    for entry_index, entry in enumerate(surfaces[field]):
                        if not isinstance(entry, str) or not entry.strip():
                            diagnostics.append(
                                f"capabilities[{index}].public_surfaces."
                                f"{field}[{entry_index}]: expected a "
                                "non-empty string"
                            )
                        if isinstance(entry, str):
                            if entry in seen_surface_entries:
                                duplicate_surface_entries.add(entry)
                            seen_surface_entries.add(entry)
                            if entry.strip():
                                claimed_surfaces[field].add(entry)
                    for entry in sorted(duplicate_surface_entries):
                        diagnostics.append(
                            f"capabilities[{index}].public_surfaces.{field}: "
                            f"duplicate entry {entry}"
                        )
                    if (
                        surfaces[field]
                        and not duplicate_surface_entries
                        and all(
                            isinstance(entry, str) and entry.strip()
                            for entry in surfaces[field]
                        )
                    ):
                        if declared_surfaces is None:
                            declared_surfaces = collect_public_surfaces(root)
                        for entry_index, entry in enumerate(surfaces[field]):
                            if entry not in declared_surfaces[field]:
                                diagnostics.append(
                                    f"capabilities[{index}].public_surfaces."
                                    f"{field}[{entry_index}]: declaration not "
                                    f"found: {entry}"
                                )
        for field in STRING_ARRAY_FIELDS:
            if field in capability and not isinstance(capability[field], list):
                diagnostics.append(f"capabilities[{index}].{field}: expected an array")
                continue
            if field in capability:
                seen: set[str] = set()
                duplicates: set[str] = set()
                for entry_index, entry in enumerate(capability[field]):
                    if not isinstance(entry, str) or not entry.strip():
                        diagnostics.append(
                            f"capabilities[{index}].{field}[{entry_index}]: "
                            "expected a non-empty string"
                        )
                    if isinstance(entry, str):
                        if entry in seen:
                            duplicates.add(entry)
                        seen.add(entry)
                for entry in sorted(duplicates):
                    diagnostics.append(
                        f"capabilities[{index}].{field}: duplicate entry {entry}"
                    )
                if field == "docs":
                    for entry_index, entry in enumerate(capability[field]):
                        if isinstance(entry, str) and entry.strip():
                            _, path_error = _repository_file(root, entry)
                            if path_error == "noncanonical":
                                diagnostics.append(
                                    f"capabilities[{index}].docs[{entry_index}]: "
                                    "path must be a canonical repository-relative "
                                    f"POSIX path: {entry}"
                                )
                            elif path_error == "missing":
                                diagnostics.append(
                                    f"capabilities[{index}].docs[{entry_index}]: "
                                    f"path does not exist: {entry}"
                                )
        if "tests" in capability:
            tests = capability["tests"]
            if not isinstance(tests, list):
                diagnostics.append(f"capabilities[{index}].tests: expected an array")
            else:
                if tests and test_declarations is None:
                    cmake_test_sources = collect_cmake_test_sources(root)
                    python_test_sources = collect_python_test_sources(root)
                    test_declarations, gtest_support = collect_test_declarations(root)
                seen_tests: set[tuple[str, str, str]] = set()
                for entry_index, evidence in enumerate(tests):
                    label = f"capabilities[{index}].tests[{entry_index}]"
                    primary_evidence_platforms.update(
                        _validate_test_evidence(
                            diagnostics,
                            label=label,
                            evidence=evidence,
                            root=root,
                            declarations=test_declarations or {},
                            gtest_support=gtest_support or {},
                            cmake_sources=cmake_test_sources or {},
                            python_sources=python_test_sources or {},
                        )
                    )
                    if isinstance(evidence, dict):
                        identity = tuple(
                            evidence.get(key) for key in ("runner", "target", "filter")
                        )
                        if all(isinstance(value, str) for value in identity):
                            typed_identity = (identity[0], identity[1], identity[2])
                            if typed_identity in seen_tests:
                                diagnostics.append(
                                    f"capabilities[{index}].tests: duplicate entry "
                                    f"{'.'.join(typed_identity)}"
                                )
                            seen_tests.add(typed_identity)
        for field in EVIDENCE_FIELDS:
            if field in capability:
                if test_declarations is None:
                    cmake_test_sources = collect_cmake_test_sources(root)
                    python_test_sources = collect_python_test_sources(root)
                    test_declarations, gtest_support = collect_test_declarations(root)
                validated_platforms = _validate_test_evidence(
                    diagnostics,
                    label=f"capabilities[{index}].{field}",
                    evidence=capability[field],
                    root=root,
                    declarations=test_declarations,
                    gtest_support=gtest_support or {},
                    cmake_sources=cmake_test_sources or {},
                    python_sources=python_test_sources or {},
                )
                if field == "proof_test":
                    primary_evidence_platforms.update(validated_platforms)
        status = capability.get("status")
        if not isinstance(status, str) or status not in STATUSES:
            diagnostics.append(
                f"capabilities[{index}].status: expected one of "
                "experimental, supported, unsupported"
            )
        if status == "supported":
            current_platform = current_test_platform()
            has_current_evidence = current_platform in primary_evidence_platforms
            rewrite_missing_required_proof = (
                capability.get("kind") == "rewrite" and "proof_test" not in capability
            )
            if not has_current_evidence and not rewrite_missing_required_proof:
                diagnostics.append(
                    f"capabilities[{index}]: supported capability requires "
                    "tests or proof_test evidence executable on "
                    f"{current_platform}"
                )
        if (
            capability.get("kind") == "rewrite"
            and capability.get("status") == "supported"
        ):
            for field in EVIDENCE_FIELDS:
                if field not in capability:
                    diagnostics.append(f"missing {field}")
    for capability_id in sorted(duplicate_ids):
        diagnostics.append(f"duplicate capability id: {capability_id}")
    for capability_id in sorted(required_capability_ids - seen_ids):
        diagnostics.append(f"missing required capability id: {capability_id}")

    if enforce_surface_completeness is None:
        enforce_surface_completeness = (
            root.resolve() != REPO_ROOT.resolve()
            or REQUIRED_CAPABILITY_IDS <= set(required_capability_ids)
        )
    if enforce_surface_completeness or isinstance(exclusions, dict):
        if declared_surfaces is None:
            declared_surfaces = collect_public_surfaces(root)
        for surface in sorted(PUBLIC_SURFACES):
            for name in sorted(claimed_surfaces[surface] & excluded_surfaces[surface]):
                diagnostics.append(
                    f"public surface {surface}: {name} is both claimed and excluded"
                )
            for name in sorted(excluded_surfaces[surface] - declared_surfaces[surface]):
                diagnostics.append(
                    f"surface_exclusions.{surface}: declaration not found: {name}"
                )
            if enforce_surface_completeness:
                accounted = claimed_surfaces[surface] | excluded_surfaces[surface]
                for name in sorted(declared_surfaces[surface] - accounted):
                    diagnostics.append(
                        f"public surface {surface}: unclaimed declaration {name}"
                    )
    return sorted(diagnostics)


def main(argv: list[str] | None = None, *, root: Path = REPO_ROOT) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "manifest",
        nargs="?",
        type=Path,
        default=root / "docs" / "capabilities.json",
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        help="also audit configured CMake/CTest and built GoogleTest inventory",
    )
    parser.add_argument(
        "--build-config",
        help="CMake configuration name for a multi-config build",
    )
    arguments = parser.parse_args(argv)

    try:
        document = json.loads(arguments.manifest.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        print(f"capability manifest error: {error}", file=sys.stderr)
        return 1

    required_ids = (
        REQUIRED_CAPABILITY_IDS
        if root.resolve() == REPO_ROOT.resolve()
        else frozenset()
    )
    diagnostics = validate_manifest(
        document, root, required_capability_ids=required_ids
    )
    if not diagnostics and arguments.build_dir is not None:
        diagnostics.extend(
            audit_configured_evidence(
                document,
                root,
                arguments.build_dir,
                build_config=arguments.build_config,
                manifest_path=arguments.manifest.resolve(strict=False),
            )
        )
    if diagnostics:
        for diagnostic in diagnostics:
            print(f"capability manifest error: {diagnostic}", file=sys.stderr)
        return 1

    count = len(document["capabilities"])
    noun = "capability" if count == 1 else "capabilities"
    print(f"capability manifest valid: {count} {noun}")
    if arguments.build_dir is not None:
        print(f"configured capability evidence valid: {arguments.build_dir.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
