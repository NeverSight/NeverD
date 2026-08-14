#!/usr/bin/env python3
"""Validate NeverD's machine-readable capability claims."""

from __future__ import annotations

import argparse
import ast
import json
import re
import subprocess
import sys
from pathlib import Path, PurePosixPath
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[1]
STATUSES = frozenset({"experimental", "supported", "unsupported"})
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
ARRAY_FIELDS = ("targets", "docs", "limitations", "tests")
PUBLIC_SURFACES = frozenset({"c", "python", "cli", "json"})
REQUIRED_CAPABILITY_IDS = frozenset(
    {
        "debug.hardware",
        "debug.local",
        "debug.remote",
        "exception.rewrite.end-to-end",
        "llvm.semantic.synthesis-rewrite",
        "semantic.mba.derivation",
        "semantic.synthesis.candidate",
        "symbolic.execution.path-exploration",
        "translation.executable-engine",
        "translation.runtime-contract",
    }
)
EVIDENCE_FIELDS = ("proof_test", "unknown_test", "poison_test")
EVIDENCE_KEYS = frozenset({"target", "source", "filter"})
GTEST_DECLARATION = re.compile(
    r"\b(?:TEST|TEST_F|TEST_P|TYPED_TEST|TYPED_TEST_P)\s*"
    r"\(\s*([A-Za-z_]\w*)\s*,\s*([A-Za-z_]\w*)\s*\)"
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
CMAKE_BRACKET_COMMENT = re.compile(r"#\[(=*)\[.*?\]\1\]", re.DOTALL)
PUBLIC_C_DECLARATION = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
CLI_SUBCOMMAND_DECLARATION = re.compile(
    r'\bcl::SubCommand\s+([A-Za-z_]\w*)\s*\(\s*"([^"]+)"'
)
CLI_OPTION_DECLARATION = re.compile(
    r"\bcl::(?:opt|list|alias)\s*(?:<[^;]*?>)?\s+[A-Za-z_]\w*\s*"
    r"\((.*?)\);",
    re.DOTALL,
)
CLI_SUBCOMMAND_REFERENCE = re.compile(r"\bcl::sub\(\s*([A-Za-z_]\w*)\s*\)")


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


def collect_declared_tests(root: Path) -> frozenset[str]:
    """Collect literal C++ and Python test declarations from source trees."""

    declarations: set[str] = set()
    cpp_files = _source_files(
        root, ("unittests",), frozenset({".cc", ".cpp", ".cxx", ".mm"})
    )
    for path in cpp_files:
        text = _read_text(path)
        if text is None:
            continue
        declarations.update(
            f"{suite}.{name}"
            for suite, name in GTEST_DECLARATION.findall(CXX_NON_CODE.sub(" ", text))
        )

    python_files = _source_files(
        root,
        ("scripts/tests", "pluginsdk/python/tests", "unittests"),
        frozenset({".py"}),
    )
    for path in python_files:
        text = _read_text(path)
        if text is None:
            continue
        try:
            module = ast.parse(text, filename=str(path))
        except SyntaxError:
            continue
        for node in ast.walk(module):
            if not isinstance(node, ast.ClassDef):
                continue
            for member in node.body:
                if isinstance(member, (ast.FunctionDef, ast.AsyncFunctionDef)) and (
                    member.name.startswith("test_")
                ):
                    declarations.add(f"{node.name}.{member.name}")
    return frozenset(declarations)


def collect_public_surfaces(root: Path) -> dict[str, frozenset[str]]:
    """Collect public API and CLI declarations from authoritative sources."""

    c_declarations: set[str] = set()
    for path in _source_files(
        root, ("include/neverd/sdk",), frozenset({".h", ".hh", ".hpp"})
    ):
        text = _read_text(path)
        if text is not None:
            c_declarations.update(
                PUBLIC_C_DECLARATION.findall(CXX_NON_CODE.sub(" ", text))
            )

    python_declarations: set[str] = set()
    for path in _source_files(
        root, ("pluginsdk/python/neverd_plugin",), frozenset({".py"})
    ):
        text = _read_text(path)
        if text is None:
            continue
        try:
            module = ast.parse(text, filename=str(path))
        except SyntaxError:
            continue
        python_declarations.update(
            node.name
            for node in ast.walk(module)
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        )

    cli_declarations: set[str] = set()
    cli_path = root / "tools" / "neverd" / "NeverDCLIOptions.cpp"
    cli_text = _read_text(cli_path)
    if cli_text is not None:
        commands = dict(CLI_SUBCOMMAND_DECLARATION.findall(cli_text))
        for command in commands.values():
            cli_declarations.add(f"neverd {command}")
        for match in CLI_OPTION_DECLARATION.finditer(cli_text):
            body = match.group(1)
            option = re.match(r'\s*"([^"]+)"', body)
            if option is None:
                continue
            for variable in CLI_SUBCOMMAND_REFERENCE.findall(body):
                command = commands.get(variable)
                if command is not None:
                    cli_declarations.add(f"neverd {command} --{option.group(1)}")

    frozen_c = frozenset(c_declarations)
    return {
        "c": frozen_c,
        "python": frozenset(python_declarations),
        "cli": frozenset(cli_declarations),
        "json": frozen_c,
    }


def _inside_cmake_build(path: Path, root: Path) -> bool:
    for directory in path.parents:
        if directory == root:
            break
        if (directory / "CMakeCache.txt").is_file():
            return True
    return False


def _cmake_files(root: Path) -> list[Path]:
    try:
        inventory = subprocess.run(
            ("git", "ls-files", "--cached", "--others", "--exclude-standard"),
            cwd=root,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
    except OSError:
        inventory = None
    if inventory is not None and inventory.returncode == 0:
        return [
            root / name
            for name in sorted(inventory.stdout.splitlines())
            if Path(name).name == "CMakeLists.txt"
            and not _inside_cmake_build(root / name, root)
        ]

    files: list[Path] = []
    for path in root.rglob("CMakeLists.txt"):
        relative = path.relative_to(root)
        if any(
            part in {".git", ".hg", ".svn", "build", "_build"}
            or part.startswith("build-")
            for part in relative.parts[:-1]
        ):
            continue
        if _inside_cmake_build(path, root):
            continue
        files.append(path)
    return sorted(files)


def collect_cmake_targets(root: Path) -> frozenset[str]:
    """Collect literal target declarations without inspecting build trees."""

    targets: set[str] = set()
    for path in _cmake_files(root):
        text = path.read_text(encoding="utf-8")
        text = CMAKE_BRACKET_COMMENT.sub(" ", text)
        text = "\n".join(line.split("#", 1)[0] for line in text.splitlines())
        targets.update(CMAKE_TARGET_DECLARATION.findall(text))
    return frozenset(targets)


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


def validate_manifest(
    document: Any,
    root: Path = REPO_ROOT,
    *,
    required_capability_ids: frozenset[str] | set[str] = frozenset(),
) -> list[str]:
    """Return deterministic diagnostics for an in-memory manifest."""

    if not isinstance(document, dict):
        return ["manifest: expected an object"]
    diagnostics: list[str] = []
    for field in sorted(set(document) - {"schema", "capabilities"}):
        diagnostics.append(f"unexpected top-level field: {field}")
    if type(document.get("schema")) is not int or document.get("schema") != 1:
        diagnostics.append("schema: expected integer 1")
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
        and document.get("schema") == 1
        and set(document) <= {"schema", "capabilities"}
    ):
        diagnostics.append("capabilities: expected at least one capability")
    seen_ids: set[str] = set()
    duplicate_ids: set[str] = set()
    cmake_targets: frozenset[str] | None = None
    declared_tests: frozenset[str] | None = None
    declared_surfaces: dict[str, frozenset[str]] | None = None
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
        for field in ARRAY_FIELDS:
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
                if (
                    field == "tests"
                    and capability[field]
                    and not duplicates
                    and all(
                        isinstance(entry, str) and entry.strip()
                        for entry in capability[field]
                    )
                ):
                    if declared_tests is None:
                        declared_tests = collect_declared_tests(root)
                    for entry_index, entry in enumerate(capability[field]):
                        if entry not in declared_tests:
                            diagnostics.append(
                                f"capabilities[{index}].tests[{entry_index}]: "
                                f"test declaration not found: {entry}"
                            )
        for field in EVIDENCE_FIELDS:
            if field in capability and not isinstance(capability[field], dict):
                diagnostics.append(f"capabilities[{index}].{field}: expected an object")
                continue
            if field in capability:
                evidence = capability[field]
                for key in sorted(EVIDENCE_KEYS - set(evidence)):
                    diagnostics.append(f"capabilities[{index}].{field}: missing {key}")
                for key in sorted(set(evidence) - EVIDENCE_KEYS):
                    diagnostics.append(
                        f"capabilities[{index}].{field}: unexpected field {key}"
                    )
                for key in sorted(EVIDENCE_KEYS & set(evidence)):
                    if not isinstance(evidence[key], str) or not evidence[key].strip():
                        diagnostics.append(
                            f"capabilities[{index}].{field}.{key}: expected a "
                            "non-empty string"
                        )
                if set(evidence) != EVIDENCE_KEYS or any(
                    not isinstance(evidence.get(key), str) or not evidence[key].strip()
                    for key in EVIDENCE_KEYS
                ):
                    continue
                source = evidence.get("source")
                if isinstance(source, str) and source.strip():
                    source_path, path_error = _repository_file(root, source)
                    if path_error == "noncanonical":
                        diagnostics.append(
                            f"capabilities[{index}].{field}.source: path must be "
                            "a canonical repository-relative POSIX path: "
                            f"{source}"
                        )
                    elif path_error == "missing":
                        diagnostics.append(
                            f"capabilities[{index}].{field}.source: path does "
                            f"not exist: {source}"
                        )
                    elif source_path is not None:
                        declared_filters = {
                            f"{suite}.{name}"
                            for suite, name in GTEST_DECLARATION.findall(
                                CXX_NON_CODE.sub(
                                    " ", source_path.read_text(encoding="utf-8")
                                )
                            )
                        }
                        test_filter = evidence.get("filter")
                        if (
                            isinstance(test_filter, str)
                            and test_filter.strip()
                            and test_filter not in declared_filters
                        ):
                            diagnostics.append(
                                f"capabilities[{index}].{field}.filter: not "
                                f"found in {source}: {test_filter}"
                            )
                target = evidence.get("target")
                if isinstance(target, str) and target.strip():
                    if cmake_targets is None:
                        cmake_targets = collect_cmake_targets(root)
                    if target not in cmake_targets:
                        diagnostics.append(
                            f"capabilities[{index}].{field}.target: CMake target "
                            f"does not exist: {target}"
                        )
        if capability.get("status") not in STATUSES:
            diagnostics.append(
                f"capabilities[{index}].status: expected one of "
                "experimental, supported, unsupported"
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
    return sorted(diagnostics)


def main(argv: list[str] | None = None, *, root: Path = REPO_ROOT) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "manifest",
        nargs="?",
        type=Path,
        default=root / "docs" / "capabilities.json",
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
    if diagnostics:
        for diagnostic in diagnostics:
            print(f"capability manifest error: {diagnostic}", file=sys.stderr)
        return 1

    count = len(document["capabilities"])
    noun = "capability" if count == 1 else "capabilities"
    print(f"capability manifest valid: {count} {noun}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
