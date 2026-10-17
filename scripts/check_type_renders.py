#!/usr/bin/env python3
"""Compile a program's local types as NeverD renders them.

`neverd types --all --asserts` renders every local type with compile-time
checks of each size and offset.  This script renders them in C and C++ with
every builtin spelling, in Rust, Go, Python, JavaScript and Lua with both
of their own, and compiles each with clang, rustc and go, or runs it with
python3, node or Lua 5.3+, tsc checking JavaScript's JSDoc too, so that a
layout NeverD gets wrong, or source that does not compile, fails.  The C header, which the workbench shows as
C/C++, compiles as C++ as well.  Without --binary it checks the fixture PDB
the unit tests use; with --binary, a program's own types, from its PDB or
its DWARF; with --declare, the types C, C++, Rust or Go files declare, laid
out for --data-model, such as a type library NeverD ships, and the
prototypes they declare too.  A toolchain that is missing is reported and
skipped.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

REPOSITORY = Path(__file__).resolve().parents[1]
FIXTURES = REPOSITORY / "unittests" / "types" / "fixtures"
SPELLINGS = ("portable", "windows", "keywords")
# Rust and Go name builtins by exact widths, or as Windows does, through
# aliases.
RUST_SPELLINGS = GO_SPELLINGS = PYTHON_SPELLINGS = JAVASCRIPT_SPELLINGS = \
    LUA_SPELLINGS = ("portable", "windows")

# Windows' own typedefs of the builtins it names, as <ntdef.h> declares them:
# prototypes of the compiler's intrinsics only match its declarations with
# these, where the rendered prelude gives exact widths instead.
WINDOWS_TYPEDEFS = """#define _NTDEF_
#define VOID void
typedef char CHAR;
typedef signed char SCHAR;
typedef unsigned char UCHAR;
typedef unsigned short WCHAR;
typedef short SHORT;
typedef unsigned short USHORT;
typedef int INT;
typedef unsigned int UINT;
typedef long LONG;
typedef unsigned long ULONG;
typedef long long LONGLONG;
typedef unsigned long long ULONGLONG;
typedef float FLOAT;
typedef double DOUBLE;
"""

# Per data model: the MSVC target C and C++ compile for, the GNU target the
# portable spelling also compiles for, and Go's GOARCH.
TARGETS = {
    "LLP64": ("x86_64-pc-windows-msvc", "x86_64-linux-gnu", "amd64"),
    "ILP32": ("i686-pc-windows-msvc", "i686-linux-gnu", "386"),
    "LP64": ("x86_64-linux-gnu", "x86_64-linux-gnu", "amd64"),
}


def run(command, **options):
    return subprocess.run(command, capture_output=True, text=True, **options)


def diagnostics(result):
    """The compiler's diagnostics, or empty when it compiled cleanly."""
    text = (result.stdout + result.stderr).strip()
    if result.returncode == 0 and "warning" not in text:
        return ""
    return text or f"exit status {result.returncode}"


class Checker:
    def __init__(self, arguments, work):
        self.arguments = arguments
        self.work = work
        self.failures = 0
        self.skipped = []

    def neverd(self, *options):
        command = [self.arguments.neverd, "types"]
        if self.arguments.declare:
            command += ["--data-model", self.arguments.data_model]
            for path in self.arguments.declare:
                command += ["--declare", path]
        else:
            command.append(self.arguments.binary)
            if self.arguments.pdb:
                command += ["--pdb", self.arguments.pdb]
        result = run(command + list(options))
        if result.returncode:
            sys.exit(f"neverd types failed: {result.stderr.strip()}")
        return result.stdout

    def render(self, name, *options):
        path = self.work / name
        self.neverd("--all", "--asserts", "-o", str(path), *options)
        return path

    def report(self, what, problems):
        if problems:
            self.failures += 1
            lines = problems.splitlines()
            print(f"FAIL {what}")
            for line in lines[:20]:
                print(f"     {line}")
            if len(lines) > 20:
                print(f"     ... {len(lines) - 20} more lines")
        else:
            print(f"ok   {what}")

    def skip(self, what, why):
        self.skipped.append(what)
        print(f"skip {what}: {why}")

    def c_family(self, model):
        clang = shutil.which(self.arguments.clang)
        if not clang:
            self.skip("C and C++", f"{self.arguments.clang} not found")
            return
        msvc, gnu, _ = TARGETS[model]
        cxx = clang + "++" if shutil.which(clang + "++") else clang
        for spelling in SPELLINGS:
            for language, suffix in (("c", ".h"), ("cpp", ".hpp")):
                header = self.render(f"types-{spelling}{suffix}", "--lang",
                                     language, "--spelling", spelling)
                # The C header is C's and C++'s; the C++ one is C++'s.
                units = [("c++", "-std=c++20", header.stem + "-cpp.cpp")]
                if language == "c":
                    units.insert(0, ("c", "-std=c11", header.stem + ".c"))
                targets = [msvc] + ([gnu] if spelling == "portable" else [])
                for unit_language, standard, name in units:
                    unit = self.work / name
                    unit.write_text(f'#include "{header.name}"\n',
                                    encoding="utf-8")
                    driver = clang if unit_language == "c" else cxx
                    for target in targets:
                        result = run([driver, f"--target={target}",
                                      "-fsyntax-only", standard, "-x",
                                      unit_language, str(unit)], cwd=self.work)
                        what = language if unit_language == "c" or \
                            language == "cpp" else "c as c++"
                        self.report(f"{what} {spelling} {target}",
                                    diagnostics(result))

    def prototypes(self, model):
        """The prototypes, with Windows' typedefs, for the MSVC target."""
        clang = shutil.which(self.arguments.clang)
        if not clang:
            self.skip("prototypes", f"{self.arguments.clang} not found")
            return
        header = self.render("prototypes.h", "--lang", "c", "--spelling",
                             "windows", "--prototypes")
        unit = self.work / "prototypes.c"
        unit.write_text(WINDOWS_TYPEDEFS + f'#include "{header.name}"\n',
                        encoding="utf-8")
        result = run([clang, f"--target={TARGETS[model][0]}", "-fsyntax-only",
                      "-std=c11", "-x", "c", str(unit)], cwd=self.work)
        self.report(f"c prototypes {TARGETS[model][0]}", diagnostics(result))

    def rust(self, model):
        rustc = shutil.which("rustc")
        if not rustc:
            self.skip("Rust", "rustc not found")
            return
        if model == "ILP32":
            self.skip("Rust", "32-bit layouts need a 32-bit Rust target")
            return
        for spelling in RUST_SPELLINGS:
            source = self.render(f"types-{spelling}.rs", "--lang", "rust",
                                 "--spelling", spelling)
            result = run([rustc, "--crate-type=lib", "--edition=2021",
                          "--emit=metadata", "-o",
                          str(self.work / f"types-{spelling}.rmeta"),
                          str(source)])
            self.report(f"rust {spelling}", diagnostics(result))

    def go(self, model):
        go = shutil.which("go")
        if not go:
            self.skip("Go", "go not found")
            return
        environment = dict(os.environ, GOOS="windows",
                           GOARCH=TARGETS[model][2], GOFLAGS="-mod=mod")
        for spelling in GO_SPELLINGS:
            module = self.work / f"go-{spelling}"
            module.mkdir()
            self.render(f"go-{spelling}/types.go", "--lang", "go",
                        "--spelling", spelling)
            (module / "go.mod").write_text("module types\n\ngo 1.22\n",
                                           encoding="utf-8")
            result = run([go, "vet", "./..."], cwd=module, env=environment)
            self.report(f"go {spelling} {TARGETS[model][2]}",
                        diagnostics(result))

    def python(self, model):
        """Python's ctypes check each size and offset as the module runs."""
        python = shutil.which("python3")
        if not python:
            self.skip("Python", "python3 not found")
            return
        if model == "ILP32":
            self.skip("Python", "32-bit layouts need a 32-bit Python")
            return
        for spelling in PYTHON_SPELLINGS:
            source = self.render(f"types-{spelling}.py", "--lang", "python",
                                 "--spelling", spelling)
            result = run([python, "-I", "-W", "error", str(source)])
            self.report(f"python {spelling}", diagnostics(result))

    def javascript(self):
        """Node runs the module, whose checks read and write every accessor
        within its record, and tsc checks its JSDoc as TypeScript does."""
        node = shutil.which("node")
        if not node:
            self.skip("JavaScript", "node not found")
            return
        tsc = shutil.which(self.arguments.tsc)
        if not tsc:
            self.skip("TypeScript", f"{self.arguments.tsc} not found")
        for spelling in JAVASCRIPT_SPELLINGS:
            # .mjs: Node reads the file as the module it is.
            module = self.render(f"types-{spelling}.mjs", "--lang",
                                 "javascript", "--spelling", spelling)
            self.report(f"javascript {spelling}",
                        diagnostics(run([node, str(module)])))
            if tsc:
                result = run([tsc, "--noEmit", "--allowJs", "--checkJs",
                              "--strict", "--target", "es2022", "--module",
                              "es2022", str(module)])
                self.report(f"typescript {spelling}", diagnostics(result))

    def lua(self):
        """Lua 5.3 or later runs the module, whose checks read and write
        every accessor within its record."""
        lua = next(filter(None, (shutil.which(name)
                                 for name in self.arguments.lua)), None)
        if not lua:
            self.skip("Lua", f"none of {', '.join(self.arguments.lua)} found")
            return
        for spelling in LUA_SPELLINGS:
            module = self.render(f"types-{spelling}.lua", "--lang", "lua",
                                 "--spelling", spelling)
            self.report(f"lua {spelling}",
                        diagnostics(run([lua, str(module)])))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", required=True,
                        help="the neverd command line tool")
    parser.add_argument("--binary", default=str(FIXTURES / "local_types.exe"),
                        help="the program whose types to check")
    parser.add_argument("--pdb", default=None,
                        help="its PDB, when the program does not find it")
    parser.add_argument("--declare", action="append", default=[],
                        help="check the types this file declares instead of a "
                             "program's; repeat for more")
    parser.add_argument("--data-model", default="LLP64",
                        choices=sorted(TARGETS),
                        help="the data model --declare files lay out for")
    parser.add_argument("--clang", default="clang",
                        help="the clang driver C and C++ compile with")
    parser.add_argument("--tsc", default="tsc",
                        help="the TypeScript compiler JavaScript's JSDoc "
                             "checks with")
    parser.add_argument("--lua", action="append", default=None,
                        help="a Lua 5.3+ interpreter to run Lua with; repeat "
                             "for others to try (default lua5.4, lua5.3, "
                             "lua)")
    parser.add_argument("--keep", default=None,
                        help="keep the rendered sources in this directory")
    arguments = parser.parse_args()
    if arguments.lua is None:
        arguments.lua = ["lua5.4", "lua5.3", "lua"]
    if arguments.pdb is None and arguments.binary == str(
            FIXTURES / "local_types.exe"):
        arguments.pdb = str(FIXTURES / "local_types.pdb")

    with tempfile.TemporaryDirectory(prefix="neverd-types-") as scratch:
        work = Path(arguments.keep or scratch)
        work.mkdir(parents=True, exist_ok=True)
        checker = Checker(arguments, work)
        if arguments.declare:
            model = arguments.data_model
            names = checker.neverd().splitlines()
            types = sum(line.startswith("type ") for line in names)
            prototypes = sum(line.startswith("prototype ") for line in names)
            if not types:
                sys.exit("the declarations declare no types to check")
            checker.report(f"{types} types and {prototypes} prototypes read",
                           "")
        else:
            listing = json.loads(checker.neverd("--json"))
            model = listing.get("dataModel", "LLP64")
            if listing.get("source") not in ("pdb", "dwarf") or \
                    not listing.get("types"):
                sys.exit(f"{arguments.binary} has no local types to check")
            stats = listing.get("stats", {})
            mismatches = stats.get("nestingMismatches", [])
            prototypes = 0
            checker.report(f"{len(listing['types'])} types read from "
                           f"{listing['source']}, nesting holds",
                           "\n".join(f"{name} does not lay out as listed"
                                     for name in mismatches))
            # What DWARF describes that local types leave out, with why.
            for entry in stats.get("skipped", []):
                print(f"left out {entry}")
        checker.c_family(model)
        if prototypes:
            checker.prototypes(model)
        checker.rust(model)
        checker.go(model)
        checker.python(model)
        checker.javascript()
        checker.lua()
    if checker.failures:
        sys.exit(f"{checker.failures} check(s) failed")
    print("every check passed" +
          (f"; skipped {', '.join(checker.skipped)}" if checker.skipped else ""))


if __name__ == "__main__":
    main()
