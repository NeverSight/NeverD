#!/usr/bin/env python3
"""Locate the static libraries that hold a toolchain's unwinder.

The exception runtime is never one library.  GCC splits the unwinder into
`libgcc_eh.a` and the C++ ABI into `libstdc++.a`/`libsupc++.a`; LLVM splits the
same job across `libunwind.a` and `libc++abi.a`; Rust ships its own copy inside
`libpanic_unwind`; the Windows routines live in the CRT import libraries.  A
signature run that named one of them by hand would cover one toolchain and go
stale the first time a distribution moved a file.

So the libraries are asked for by name from the compiler that would link them.
`gcc -print-file-name=libgcc_eh.a` answers with the path the linker would use
and echoes the name back unchanged when there is no such file, which is how a
missing library is told from a present one without guessing at layouts.

Run this module directly to see what a host has:

    python3 scripts/signatures/eh_signature_inputs.py --compiler gcc
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


# Asked of a C or C++ driver with -print-file-name.  Ordered roughly by how
# much exception machinery each carries, because the first one found is the
# most useful thing to report when a host has only some of them.
GCC_STYLE_LIBRARIES: tuple[str, ...] = (
    "libgcc_eh.a",
    "libgcc.a",
    "libsupc++.a",
    "libstdc++.a",
    "libunwind.a",
    "libc++abi.a",
    "libc++.a",
    "libobjc.a",
    "libgnat.a",
    "libgnarl.a",
    "libgphobos.a",
    "libdruntime.a",
)

# compiler-rt names its builtins archive after the target it was built for, so
# it cannot be asked for by a fixed name on every host.
CLANG_RUNTIME_LIBRARIES: tuple[str, ...] = (
    "libclang_rt.builtins.a",
    "libclang_rt.builtins-x86_64.a",
    "libclang_rt.builtins-aarch64.a",
)

# An rlib is an ar archive with object members, so the signature maker reads
# one without knowing it is Rust.  These two hold the personality and the
# panic runtime it dispatches into.
RUST_LIBRARY_STEMS: tuple[str, ...] = (
    "libpanic_unwind-",
    "libstd-",
    "libunwind-",
)


@dataclass(frozen=True, slots=True)
class LibraryInput:
    """One archive to feed the signature maker, and where it came from."""

    path: Path
    origin: str

    def __str__(self) -> str:
        return f"{self.path} ({self.origin})"


def _run(command: list[str]) -> str | None:
    try:
        result = subprocess.run(
            command, check=True, capture_output=True, text=True, timeout=120
        )
    except (OSError, subprocess.SubprocessError):
        return None
    return result.stdout.strip()


def print_file_name(compiler: str, library: str) -> Path | None:
    """The path \\p compiler would link \\p library from, if it exists.

    The driver echoes the bare name back when it has no such file, so a result
    that is still just the name means "absent" rather than "in the working
    directory".
    """

    answer = _run([compiler, f"-print-file-name={library}"])
    if not answer or answer == library:
        return None
    path = Path(answer)
    return path if path.is_file() else None


def find_toolchain_libraries(compiler: str) -> list[LibraryInput]:
    if shutil.which(compiler) is None:
        return []
    found: list[LibraryInput] = []
    seen: set[Path] = set()
    for library in GCC_STYLE_LIBRARIES + CLANG_RUNTIME_LIBRARIES:
        path = print_file_name(compiler, library)
        if path is None:
            continue
        resolved = path.resolve()
        if resolved in seen:
            continue
        seen.add(resolved)
        found.append(LibraryInput(resolved, f"{compiler} -print-file-name"))
    return found


def find_rust_libraries(rustc: str = "rustc") -> list[LibraryInput]:
    if shutil.which(rustc) is None:
        return []
    sysroot = _run([rustc, "--print", "sysroot"])
    if not sysroot:
        return []
    found: list[LibraryInput] = []
    for target_dir in sorted(Path(sysroot).glob("lib/rustlib/*/lib")):
        for stem in RUST_LIBRARY_STEMS:
            for candidate in sorted(target_dir.glob(f"{stem}*.rlib")):
                found.append(LibraryInput(candidate.resolve(), "rustc sysroot"))
    return found


def find_all(compilers: list[str], include_rust: bool) -> list[LibraryInput]:
    found: list[LibraryInput] = []
    seen: set[Path] = set()
    sources = [find_toolchain_libraries(compiler) for compiler in compilers]
    if include_rust:
        sources.append(find_rust_libraries())
    for group in sources:
        for entry in group:
            if entry.path in seen:
                continue
            seen.add(entry.path)
            found.append(entry)
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--compiler",
        action="append",
        default=[],
        help="compiler driver to interrogate; repeatable (default: gcc, clang)",
    )
    parser.add_argument(
        "--no-rust",
        action="store_true",
        help="skip the rustc sysroot",
    )
    arguments = parser.parse_args()

    compilers = arguments.compiler or ["gcc", "clang", "g++", "clang++"]
    found = find_all(compilers, include_rust=not arguments.no_rust)
    for entry in found:
        print(entry)
    if not found:
        print("no exception-runtime archives found", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
