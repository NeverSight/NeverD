#!/usr/bin/env python3
"""Generate the Windows type libraries NeverD ships.

The kernel's PDB names its records but no typedefs and no prototypes, and a
program's imports name routines no debug information of its own declares, so
their parameters would show as no reader knows them.  This reads the headers
that declare them, from mingw-w64, whose DDK headers come from the ReactOS
DDK and are in the public domain and whose SDK headers are in the public
domain or under the Zope Public License, and in user mode from phnt, whose
native API headers are under the MIT License, and renders the types and
prototypes they declare as the C NeverD reads back:

1. clang preprocesses what the target's code includes: a driver's
   <ntifs.h>, which brings <ntddk.h> and <wdm.h>, in kernel mode, for
   Windows 10 2004 (NTDDI_WIN10_VB); in user mode, for Windows 11 24H2
   (NTDDI_WIN11_GE), the version mingw-w64's SDK headers describe, a
   program's <windows.h>, the native API ntdll exports as phnt declares it
   in place of <winternl.h>, and the headers of the libraries programs most
   often call.  What phnt takes from Microsoft's SDK that mingw-w64 spells
   otherwise or lacks comes from scripts/windows_types (PHNT_SHIMS), which
   also stands in for phnt's copy of Microsoft's <ntintsafe.h>, whose
   helpers the native API needs none of.
2. The lines of the compiler's own headers (intrinsics, <stddef.h>) and of
   the C runtime's are dropped: the library describes the system's API, and
   C++ would see the runtime's names twice.  So are the few SDK headers
   under the LGPL, which mingw-w64 takes from Wine.  #pragma pack arguments
   that name macros take the macros' values, which -E leaves unexpanded.
3. `neverd types --data-model --keep-going --all --prototypes` reads what
   remains and renders it, with builtins spelled as Windows names them.
   Declarations the reader refuses, such as vector types, are reported; the
   script fails when one is not of a kind expected.  Where headers are left
   out, the headers are read once more with them, which must leave out no
   declaration but what the reader is expected to refuse: what the headers
   left out declare, and what names it, is left out with them, and nothing
   else.

The output starts with where it came from.  Run it again when the headers or
the reader change, and check the result with scripts/check_type_renders.py.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import datetime
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import textwrap

REPOSITORY = Path(__file__).resolve().parents[1]

HEADERS = REPOSITORY / "lib" / "types" / "headers"

# Windows 10 2004 (19041), as the system the kernel's libraries describe.
VERSION = ["-DNTDDI_VERSION=0x0A000008", "-D_WIN32_WINNT=0x0A00"]

# Windows 11 24H2 (26100), the version mingw-w64's SDK headers describe:
# phnt declares what an older SDK lacks, which mingw-w64's headers declare
# whatever the version, and the native API of that version.
USER_VERSION = ["-DNTDDI_VERSION=0x0A000010", "-D_WIN32_WINNT=0x0A00",
                "-DWINVER=0x0A00", "-DPHNT_MODE=PHNT_MODE_USER",
                "-DPHNT_VERSION=PHNT_WINDOWS_11_24H2", "-DWIN32_NO_STATUS"]

# What phnt takes from Microsoft's SDK: the macros mingw-w64 spells otherwise
# or lacks (phnt_prelude.h, included first), the types it lacks
# (phnt_sdk_types.h) and the headers it names otherwise.
PHNT_SHIMS = REPOSITORY / "scripts" / "windows_types"

# Headers whose declarations are the C runtime's, not the kernel's.
KERNEL_RUNTIME = re.compile(
    r"/(?:corecrt[^/]*|crtdefs|_mingw[^/]*|stddef|stdarg|stdlib|stdio|string|"
    r"wchar|ctype|errno|limits|malloc|mm_malloc|vadefs|excpt|intrin|setjmp|"
    r"x86intrin|swprintf|sec_api/[^/]*|psdk_inc/[^/]*)\.(?:h|inl)$")

# The C runtime's routines, which the C library tables describe, and
# winpthreads' POSIX ones.  The runtime's types stay where the system's
# records hold them, as IP_ADAPTER_INFO holds a time_t (<corecrt.h>), and so
# do Winsock's, which psdk_inc declares.  <stdint.h>, which phnt includes,
# is the runtime's: its int_fast16_t is mingw-w64's short, the Microsoft
# runtime's int.
USER_RUNTIME = re.compile(
    r"/(?:corecrt_[^/]*|crtdefs|_mingw[^/]*|stddef|stdarg|stdint|stdlib|"
    r"stdio|string|wchar|ctype|errno|limits|malloc|mm_malloc|vadefs|excpt|"
    r"intrin|setjmp|x86intrin|swprintf|time|sys/timeb|pthread_[^/]*|"
    r"sec_api/[^/]*|sec_api/sys/[^/]*|psdk_inc/(?!_socket_types|_fd_types|"
    r"_ip_types|_wsadata|_xmitfile|_ip_mreq1|_wsa_errnos)[^/]*)\.(?:h|inl)$")

# The SDK headers mingw-w64 takes from Wine under the LGPL 2.1 or later
# (the Debian package's copyright file lists them): COM interfaces of MSXML,
# the property system, object arrays and query conditions, and the GUIDs of
# known folders and device properties.  The library holds the public domain
# and Zope licensed declarations alone, as the kernel's does.
USER_LEFT_OUT = re.compile(
    r"/(?:msxml|propsys|objectarray|structuredquerycondition|knownfolders|"
    r"devpkey)\.h$")


@dataclass(frozen=True)
class Target:
    triple: str
    model: str
    # The lines of the unit clang preprocesses, the definitions and the
    # include directories under --mingw.
    unit: list[str]
    defines: list[str]
    directories: list[str]
    runtime: re.Pattern
    left_out: re.Pattern | None
    describes: str
    output: Path
    # Whether it reads phnt's native API (--phnt).
    phnt: bool = False


# The headers a program includes for the libraries programs most often call,
# after <windows.h> and the native API (USER_UNIT): WinINet, the shell,
# process and module enumeration, debugging help, CNG, the IP helper, device
# setup, trust verification, user environments, terminal services, LAN
# Manager, security descriptors and policies, common controls, URL monikers
# and the event log.  WinHTTP is left out: it cannot be included with
# WinINet.
USER_HEADERS = (
    "wininet", "shellapi", "shlobj", "shlwapi", "psapi", "tlhelp32",
    "dbghelp", "bcrypt", "ncrypt", "iphlpapi", "setupapi", "wintrust",
    "softpub", "userenv", "wtsapi32", "lm", "sddl", "aclapi", "ntsecapi",
    "commctrl", "urlmon", "winevt", "evntrace")

# The phnt headers that <phnt.h> leaves for a program to include: the Zw
# routines, WinStations, the session manager, win32k's GDI calls, service
# tags, the user manager and SMBIOS.  Not <ntd3dkmt.h> and <ntafd.h>, which
# need the DDK's <dxmini.h> and <tdi.h>, nor phnt's copies of Microsoft's
# <ntintsafe.h> and <ntstrsafe.h>.
PHNT_HEADERS = ("ntzwapi", "winsta", "ntsmss", "ntgdi", "subprocesstag",
                "usermgr", "smbios")

# Routines phnt declares DECLSPEC_NORETURN that return to their caller:
# NtRaiseHardError returns the user's response, LdrShutdownProcess and
# LdrShutdownThread end the process or thread only from a loader callout
# (RtlExitUserProcess calls the first and goes on), and RtlAssert returns
# when the assertion is ignored.  The libraries declare them returning.
RETURNING = ("NtRaiseHardError", "LdrShutdownProcess", "LdrShutdownThread",
             "RtlAssert")

# Winsock first, which <windows.h> would otherwise include as the old
# <winsock.h>; <windows.h> whole, before phnt's <phnt_windows.h> asks for
# the lean one; then phnt's native API, before the headers that declare
# UNICODE_STRING themselves where no native API has (<ntsecapi.h>).
# <phnt_windows.h> asks what follows for Unicode, for the GUIDs defined and
# for COM's macros, which would leave out the ANSI routines WinINet names
# without a suffix, the shell's automation interfaces and shlwapi's
# IStream_Read: the library keeps what the headers declare without them.
USER_UNIT = (["#include <winsock2.h>", "#include <ws2tcpip.h>",
              "#include <windows.h>", "#include <phnt_windows.h>",
              "#undef UNICODE", "#undef INITGUID", "#undef COBJMACROS",
              "#include <phnt_sdk_types.h>", "#include <phnt.h>"] +
             [f"#include <{name}.h>" for name in PHNT_HEADERS] +
             [f"#include <{name}.h>" for name in USER_HEADERS])


def kernel(triple, model, arch, output):
    """What a driver for the architecture includes: <ntifs.h>, which
    brings <ntddk.h> and <wdm.h>, in kernel mode."""
    return Target(
        triple, model, ["#include <ntifs.h>"], ["-D_KERNEL_MODE"] + VERSION,
        ["ddk", ""], KERNEL_RUNTIME, None,
        f"kernel's types and prototypes for {arch} ({model}), as a driver\n"
        "// declares them with <ntifs.h>", HEADERS / output)


def user(triple, model, arch, output):
    """What a program for the architecture includes (USER_UNIT)."""
    return Target(
        triple, model, USER_UNIT, USER_VERSION, [""], USER_RUNTIME,
        USER_LEFT_OUT,
        f"user mode's types and prototypes for {arch} ({model}), as a\n"
        "// program declares them with <windows.h>, phnt's native API and "
        "the\n// headers of the libraries programs most often call",
        HEADERS / output, phnt=True)


# mingw-w64's DDK declares no ARM64 kernel ("Unknown Architecture").
TARGETS = {
    "kernel-x64": kernel("x86_64-w64-mingw32", "LLP64", "x64",
                         "WindowsKernelX64.h"),
    "user-x64": user("x86_64-w64-mingw32", "LLP64", "x64", "WindowsUserX64.h"),
    "kernel-x86": kernel("i686-w64-mingw32", "ILP32", "x86",
                         "WindowsKernelX86.h"),
    "user-x86": user("i686-w64-mingw32", "ILP32", "x86", "WindowsUserX86.h"),
    "user-arm64": user("aarch64-w64-mingw32", "LLP64", "ARM64",
                       "WindowsUserARM64.h"),
}

# What the reader may refuse: the compiler's vector and tile types, and what
# names them.
EXPECTED = re.compile(
    r"makes a type local types do not hold|"
    r"'(?:__m\d+\w*|__v\d+\w*|__bf16|_?_?tile\w*|__mmask\d+)' names no type")

NAMES_NO_TYPE = re.compile(r"'(\w+)' names no type")


def run(command, **options):
    result = subprocess.run(command, capture_output=True, text=True, **options)
    if result.returncode:
        sys.exit(f"{command[0]} failed:\n{result.stderr.strip()}")
    return result.stdout


def macros(clang, target, include, unit):
    """The object-like macros the headers define, by name."""
    found = {}
    for line in run([clang, f"--target={target.triple}", "-E", "-dM",
                     *include, *target.defines, str(unit)]).splitlines():
        match = re.match(r"#define (\w+) (.+)$", line)
        if match:
            found[match.group(1)] = match.group(2).strip()
    return found


def system_lines(text, defined, dropped):
    """The preprocessed lines of the Windows headers \\p dropped does not
    match, without line markers, #pragma pack arguments expanded."""
    kept, keep = [], True
    for line in text.splitlines():
        marker = re.match(r'# \d+ "([^"]+)"', line)
        if marker:
            path = marker.group(1)
            keep = not ("/lib/clang/" in path or dropped(path) or
                        path.startswith("<"))
            continue
        if not keep:
            continue
        pack = re.match(r"\s*#pragma pack\(([^)]*)\)", line)
        if pack:
            parts = [part.strip() for part in pack.group(1).split(",")]
            line = "#pragma pack(" + ",".join(defined.get(part, part)
                                             for part in parts) + ")"
        kept.append(line)
    return "\n".join(kept) + "\n"


def read(neverd, model, source):
    """The names of the types \\p source declares and the problems the
    reader meets, as `neverd types --json` lists them."""
    listed = json.loads(run([neverd, "types", "--data-model", model,
                             "--declare", str(source), "--keep-going",
                             "--json"]))
    return set(listed["types"]), [problem["message"]
                                  for problem in listed["problems"]]


def check(neverd, target, text, defined, work):
    """Reads the headers, those left out too, and exits when the reader
    refuses a declaration it should not.  Returns the source to render."""
    runtime = target.runtime.search
    source = work / "system.h"
    if not target.left_out:
        source.write_text(system_lines(text, defined, runtime),
                          encoding="utf-8")
        _, problems = read(neverd, target.model, source)
        unexpected = [problem for problem in problems
                      if not EXPECTED.search(problem)]
        print(f"{len(problems)} declarations refused, "
              f"{len(problems) - len(unexpected)} of them vector types")
    else:
        whole = work / "whole.h"
        whole.write_text(system_lines(text, defined, runtime),
                         encoding="utf-8")
        every, problems = read(neverd, target.model, whole)
        unexpected = [problem for problem in problems
                      if not EXPECTED.search(problem)]
        source.write_text(system_lines(
            text, defined,
            lambda path: runtime(path) or target.left_out.search(path)),
            encoding="utf-8")
        kept, after = read(neverd, target.model, source)
        gone = every - kept
        left_out = [problem for problem in after
                    if not EXPECTED.search(problem)]
        unexpected += [problem for problem in left_out
                       if not ((match := NAMES_NO_TYPE.search(problem)) and
                               match.group(1) in gone)]
        print(f"{len(problems)} declarations refused, "
              f"{len(problems) - sum(not EXPECTED.search(problem) for problem in problems)} "
              f"of them vector types; {len(gone)} types left out with their "
              f"headers, {len(left_out)} declarations naming them")
    if unexpected:
        print("\n".join(unexpected[:40]))
        sys.exit(f"{len(unexpected)} declarations refused unexpectedly")
    return source


def generate(arguments, name, target, version):
    include = [option for directory in target.directories
               for option in ("-I", str(Path(arguments.mingw) / directory))]
    if target.phnt:
        include = ["-include", str(PHNT_SHIMS / "phnt_prelude.h"), "-I",
                   str(PHNT_SHIMS), "-I", str(arguments.phnt)] + include
    output = Path(arguments.output) if arguments.output else target.output
    with tempfile.TemporaryDirectory(prefix="neverd-windows-types-") as scratch:
        work = Path(scratch)
        unit = work / "system.c"
        unit.write_text("\n".join(target.unit) + "\n", encoding="utf-8")
        text = run([arguments.clang, f"--target={target.triple}", "-E",
                    *include, *target.defines, str(unit)])
        source = check(arguments.neverd, target, text,
                       macros(arguments.clang, target, include, unit), work)
        rendered = work / "rendered.h"
        run([arguments.neverd, "types", "--data-model", target.model,
             "--declare", str(source), "--keep-going", "--all",
             "--prototypes", "--spelling", "windows", "--no-offsets", "-o",
             str(rendered)])
        body = rendered.read_text(encoding="utf-8")
    for routine in RETURNING if target.phnt else ():
        body, count = re.subn(rf"^_Noreturn ((?:.*\W)?{routine}\()", r"\1",
                              body, flags=re.MULTILINE)
        if count != 1:
            sys.exit(f"{count} noreturn prototypes of {routine}, not one")

    if target.phnt:
        commit = run(["git", "-C", str(arguments.phnt), "rev-parse",
                      "HEAD"]).strip()
        text = (
            f"The Windows {target.describes.replace(chr(10) + '// ', ' ')}, "
            "for Windows 11 24H2.  Generated on "
            f"{datetime.date.today().isoformat()} by "
            f"scripts/generate_windows_types.py --target {name} from the SDK "
            f"headers of mingw-w64 {version} and the native API headers of "
            f"phnt at commit {commit}: changed from them, as NeverD reads and "
            "renders their declarations again.  The headers' notices are in "
            "LICENSES/mingw-w64-headers.txt and LICENSES/phnt.txt.  Do not "
            "edit; generate it again.")
        header = "".join(f"// {line}\n" for line in textwrap.wrap(
            text, 77, break_long_words=False, break_on_hyphens=False))
    else:
        header = (
            f"// The Windows {target.describes} for Windows 10 2004.  "
            "Generated\n"
            f"// on {datetime.date.today().isoformat()} by "
            f"scripts/generate_windows_types.py --target {name}\n"
            f"// from the DDK and SDK headers of mingw-w64 {version}: changed "
            "from\n"
            "// them, as NeverD reads and renders their declarations again.  "
            "The\n"
            "// headers' notices are in LICENSES/mingw-w64-headers.txt.  Do "
            "not\n// edit; generate it again.\n")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(header + body, encoding="utf-8")
    shown = (output.relative_to(REPOSITORY)
             if output.is_relative_to(REPOSITORY) else output)
    print(f"wrote {shown}: {body.splitlines()[0].lstrip('/ ')}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", required=True,
                        help="the neverd command line tool")
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--mingw", default="/usr/share/mingw-w64/include",
                        help="mingw-w64's include directory")
    parser.add_argument("--phnt", default=None,
                        help="a checkout of https://github.com/winsiderss/"
                             "phnt, whose native API the user mode libraries "
                             "declare")
    parser.add_argument("--target", action="append", choices=sorted(TARGETS),
                        help="the library to generate; repeat for more "
                             "(default: every library)")
    parser.add_argument("--output", default=None,
                        help="with one --target, where to write the library "
                             "(default: its place in lib/types/headers)")
    arguments = parser.parse_args()
    names = arguments.target or sorted(TARGETS)
    if arguments.output and len(names) != 1:
        parser.error("--output takes one --target")
    if not arguments.phnt and any(TARGETS[name].phnt for name in names):
        parser.error("the user mode libraries read phnt: give --phnt")
    version = run(["dpkg-query", "-W", "-f=${Version}", "mingw-w64-common"]
                  ).strip() if Path("/usr/bin/dpkg-query").exists() else "?"
    for name in names:
        generate(arguments, name, TARGETS[name], version)


if __name__ == "__main__":
    main()
