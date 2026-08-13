#!/usr/bin/env python3
"""Regenerate the source list of every NeverD component library from disk.

The component CMakeLists files carry an explicit source list, which makes a
directory reorganization a two-place edit: move the file, then remember to
retype its new path.  This script removes the second half by treating the
filesystem as the source of truth and rewriting only the source list, leaving
LINK_COMPONENTS, LINK_LIBS, comments, and every other command untouched.

Coverage is deliberately partial.  A directory qualifies only when target
membership follows from the layout: one target owning the whole directory, or
sibling targets each owning a subdirectory.  unittests/lift, unittests/evm,
unittests/sbf, and unittests/plugin give one target per source file with no
such rule, so they stay hand-maintained rather than guessed at.

Run from the repository root:

    python3 scripts/regen_component_sources.py           # rewrite in place
    python3 scripts/regen_component_sources.py --check   # report drift only
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Component CMakeLists whose source list is the recursive set of .cpp files
# under the same directory.
RECURSIVE_COMPONENTS = [
    "lib/ir/high",
    "lib/ir/intrinsics",
    "lib/ir/low",
    "lib/ir/med",
    "lib/evm",
    "lib/loader",
    "lib/sbf",
    "lib/decode",
    "lib/lift",
    "lib/backend/c",
    "lib/backend/codegen",
    "lib/backend/llvm",
    "lib/pipeline",
    "lib/pass/ir",
    "lib/pass/mir",
    "lib/debug",
    "lib/libc",
    "lib/support",
    "lib/symbolic",
    "lib/sigs",
]

# lib/ir owns only its own directory level; its subdirectories are separate
# component libraries that would otherwise be compiled twice.
FLAT_COMPONENTS = ["lib/ir"]

# lib/sdk is not an add_neverd_component_library() target: it builds the shared
# library from a CMake variable and gates one source on a feature option.
SDK_CMAKE = "lib/sdk/CMakeLists.txt"
SDK_CONDITIONAL_SOURCES = {"PythonPluginRuntime.cpp"}

# Test directories where one add_neverd_unittest() target owns every source.
SINGLE_TARGET_TEST_DIRS = ["unittests/symbolic", "unittests/libc"]

# unittests/semantic splits one broad suite from six focused probe binaries.
# Each probe owns a subdirectory so membership stays derivable from the layout;
# the broad suite takes everything the probes do not claim.
SEMANTIC_CMAKE = "unittests/semantic/CMakeLists.txt"
SEMANTIC_PROBE_TARGETS = {
    "NeverDPatchFullTests": "probe/patchfull",
    "NeverDSwitchXformTests": "probe/switch",
    "NeverDIndCallXformTests": "probe/indcall",
    "NeverDCFGLoopXformTests": "probe/cfgloop",
    "NeverDTwoTableXformTests": "probe/twotable",
    "NeverDAvxUpperXformTests": "probe/avxupper",
}
SEMANTIC_BROAD_TARGET = "NeverDSemanticTests"
SEMANTIC_REPRO_DIR = "probe/repro"

COMPONENT_CALL_RE = re.compile(
    r"^add_neverd_component_library\((\S+)\s*$", re.MULTILINE
)
UNITTEST_CALL_RE = re.compile(r"^add_neverd_unittest\((\S+)\s*$", re.MULTILINE)

# The five xform probes share one wrapper over add_neverd_unittest(), so a
# target's sources can sit behind either command.
TEST_COMMANDS = "add_neverd_unittest|add_neverd_xform_probe"
KEYWORD_RE = re.compile(
    r"^\s*(LINK_COMPONENTS|LINK_LIBS|TIMEOUT|DISCOVERY_TIMEOUT)\b"
)


def sources_for(directory: Path, recursive: bool) -> list[str]:
    pattern = "**/*.cpp" if recursive else "*.cpp"
    found = sorted(
        p.relative_to(directory).as_posix() for p in directory.glob(pattern)
    )
    if not found:
        raise SystemExit(f"no .cpp files found under {directory}")
    return found


def replace_source_list(
    text: str, cmake_path: Path, call_start: int, sources: list[str]
) -> str:
    """Swap the source list of the call whose opening line index is given."""
    lines = text.splitlines(keepends=True)
    start = call_start + 1

    # The source list runs until a keyword argument or the closing paren.
    end = start
    while end < len(lines):
        if KEYWORD_RE.match(lines[end]) or lines[end].strip() == ")":
            break
        end += 1
    else:
        raise SystemExit(f"{cmake_path}: unterminated source list")

    body = [f"  {src}\n" for src in sources]
    # Preserve the blank line that separates sources from the keyword section.
    if end < len(lines) and KEYWORD_RE.match(lines[end]):
        body.append("\n")

    return "".join(lines[:start] + body + lines[end:])


def rewrite_component(cmake_path: Path, sources: list[str]) -> str | None:
    """Return the new file text, or None when it already matches disk."""
    text = cmake_path.read_text()
    match = COMPONENT_CALL_RE.search(text)
    if not match:
        raise SystemExit(f"{cmake_path}: no add_neverd_component_library() call")

    call_start = text[: match.start()].count("\n")
    new_text = replace_source_list(text, cmake_path, call_start, sources)
    return None if new_text == text else new_text


def rewrite_single_target_test(cmake_path: Path) -> str | None:
    text = cmake_path.read_text()
    match = UNITTEST_CALL_RE.search(text)
    if not match:
        raise SystemExit(f"{cmake_path}: no add_neverd_unittest() call")

    sources = sources_for(cmake_path.parent, recursive=True)
    call_start = text[: match.start()].count("\n")
    new_text = replace_source_list(text, cmake_path, call_start, sources)
    return None if new_text == text else new_text


def rewrite_semantic(cmake_path: Path) -> str | None:
    directory = cmake_path.parent
    original = cmake_path.read_text()
    text = original

    claimed: set[str] = set()
    for rel_dir in list(SEMANTIC_PROBE_TARGETS.values()) + [SEMANTIC_REPRO_DIR]:
        probe_dir = directory / rel_dir
        if not probe_dir.is_dir():
            raise SystemExit(f"missing probe directory {probe_dir}")
        for src in probe_dir.glob("**/*.cpp"):
            claimed.add(src.relative_to(directory).as_posix())

    broad = [
        src
        for src in sources_for(directory, recursive=True)
        if src not in claimed
    ]

    plans = [(SEMANTIC_BROAD_TARGET, broad)]
    for target, rel_dir in SEMANTIC_PROBE_TARGETS.items():
        probe_sources = sorted(
            p.relative_to(directory).as_posix()
            for p in (directory / rel_dir).glob("**/*.cpp")
        )
        if not probe_sources:
            raise SystemExit(f"{cmake_path}: {rel_dir} holds no sources")
        plans.append((target, probe_sources))

    # Rewriting shifts line numbers, so relocate each call on the current text.
    for target, sources in plans:
        match = re.search(
            rf"^(?:{TEST_COMMANDS})\({re.escape(target)}\s*$",
            text,
            re.MULTILINE,
        )
        if not match:
            raise SystemExit(f"{cmake_path}: no test target named {target}")
        call_start = text[: match.start()].count("\n")
        text = replace_source_list(text, cmake_path, call_start, sources)

    repro_sources = sorted(
        p.relative_to(directory).as_posix()
        for p in (directory / SEMANTIC_REPRO_DIR).glob("**/*.cpp")
    )
    text = re.sub(
        r"add_executable\(PatchFullRepro EXCLUDE_FROM_ALL [^)]*\)",
        "add_executable(PatchFullRepro EXCLUDE_FROM_ALL "
        + " ".join(repro_sources)
        + ")",
        text,
        count=1,
    )

    return None if text == original else text


def rewrite_sdk(cmake_path: Path) -> str | None:
    text = cmake_path.read_text()
    directory = cmake_path.parent
    all_sources = sources_for(directory, recursive=True)

    unconditional = [
        s for s in all_sources if Path(s).name not in SDK_CONDITIONAL_SOURCES
    ]
    conditional = [
        s for s in all_sources if Path(s).name in SDK_CONDITIONAL_SOURCES
    ]

    block = "set(NEVERD_SDK_SOURCES\n"
    block += "".join(f"    {src}\n" for src in unconditional[:-1])
    block += f"    {unconditional[-1]})\n"

    new_text = re.sub(
        r"set\(NEVERD_SDK_SOURCES\n.*?\)\n",
        block,
        text,
        count=1,
        flags=re.DOTALL,
    )

    for src in conditional:
        new_text = re.sub(
            r"(list\(APPEND NEVERD_SDK_SOURCES )[^)]*(\))",
            rf"\g<1>{src}\g<2>",
            new_text,
            count=1,
        )

    return None if new_text == text else new_text


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="report which files would change without writing them",
    )
    args = parser.parse_args()

    drifted: list[str] = []

    targets = [(d, True) for d in RECURSIVE_COMPONENTS]
    targets += [(d, False) for d in FLAT_COMPONENTS]

    for rel_dir, recursive in targets:
        directory = REPO / rel_dir
        cmake_path = directory / "CMakeLists.txt"
        if not cmake_path.exists():
            raise SystemExit(f"missing {cmake_path}")
        new_text = rewrite_component(
            cmake_path, sources_for(directory, recursive)
        )
        if new_text is not None:
            drifted.append(rel_dir)
            if not args.check:
                cmake_path.write_text(new_text)

    sdk_path = REPO / SDK_CMAKE
    new_sdk = rewrite_sdk(sdk_path)
    if new_sdk is not None:
        drifted.append("lib/sdk")
        if not args.check:
            sdk_path.write_text(new_sdk)

    for rel_dir in SINGLE_TARGET_TEST_DIRS:
        cmake_path = REPO / rel_dir / "CMakeLists.txt"
        if not cmake_path.exists():
            raise SystemExit(f"missing {cmake_path}")
        new_text = rewrite_single_target_test(cmake_path)
        if new_text is not None:
            drifted.append(rel_dir)
            if not args.check:
                cmake_path.write_text(new_text)

    semantic_path = REPO / SEMANTIC_CMAKE
    new_semantic = rewrite_semantic(semantic_path)
    if new_semantic is not None:
        drifted.append("unittests/semantic")
        if not args.check:
            semantic_path.write_text(new_semantic)

    if not drifted:
        print("all component source lists already match the filesystem")
        return 0

    verb = "would update" if args.check else "updated"
    print(f"{verb} {len(drifted)} component source list(s):")
    for rel_dir in drifted:
        print(f"  {rel_dir}")
    return 1 if args.check else 0


if __name__ == "__main__":
    sys.exit(main())
