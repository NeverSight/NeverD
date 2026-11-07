#!/usr/bin/env python3
"""Link and execute the PE32 callback object emitted by the frame tests.

This records generated callback ABI execution, not rewritten-input evidence.
LLVM short import libraries preserve the linker's default SafeSEH checks.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

if __package__:
    from .check_windows_registration_eh import image_digest, run_image
else:
    from check_windows_registration_eh import image_digest, run_image


BANNER = "neverd-registration-frame: filters=16 failures=0"
KERNEL32 = """LIBRARY kernel32.dll
EXPORTS
_RaiseException@16
_ExitProcess@4
"""
MSVCRT = """LIBRARY msvcrt.dll
EXPORTS
_except_handler3
printf
"""


def undecorate_kernel32_imports(library: Path,
                               additional_imports: tuple[bytes, ...] = ()) -> None:
    """Use the original COFF UNDECORATE import kind, also understood by lld 14.

    Older llvm-lib accepts EXPORTAS in a DEF but silently ignores its mapping.
    Changing only TypeInfo keeps every archive offset and symbol index intact.
    """
    data = bytearray(library.read_bytes())
    if data[:8] != b"!<arch>\n":
        raise ValueError("kernel32 import library is not a COFF archive")
    required = {b"_RaiseException@16", b"_ExitProcess@4", *additional_imports}
    seen = set()
    offset = 8
    while offset < len(data):
        if offset + 60 > len(data) or data[offset + 58:offset + 60] != b"`\n":
            raise ValueError("kernel32 import archive has an invalid member header")
        size = int(data[offset + 48:offset + 58])
        if size < 0:
            raise ValueError("kernel32 import archive has a negative member size")
        begin, end = offset + 60, offset + 60 + size
        if end > len(data):
            raise ValueError("kernel32 import archive has a truncated member")
        if size >= 20 and data[begin:begin + 4] == b"\x00\x00\xff\xff":
            _, _, version, machine, _, payload, _, kind = struct.unpack_from(
                "<HHHHIIHH", data, begin)
            names = bytes(data[begin + 20:end]).split(b"\0")
            if version != 0 or machine != 0x14c or payload != size - 20 or \
                    len(names) != 3 or names[1] != b"kernel32.dll" or \
                    names[0] not in required or names[0] in seen or \
                    kind & 3 or kind >> 2 not in (1, 3):
                raise ValueError("kernel32 short import has an unexpected contract")
            struct.pack_into("<H", data, begin + 18, 3 << 2)
            seen.add(names[0])
        offset = end + (size & 1)
    if seen != required or offset != len(data):
        raise ValueError("kernel32 stdcall short imports are incomplete")
    library.write_bytes(data)


def build_and_observe(source: Path, output: Path, linker: str,
                      launcher: list[str], environment: dict[str, str],
                      timeout: float) -> dict:
    report = {"schema": 1, "evidence": "generated-x86-callback-abi",
              "passed": False, "commands": []}
    try:
        data = source.read_bytes()
        if len(data) < 20 or struct.unpack_from("<H", data)[0] != 0x14C:
            raise ValueError("callback probe is not an i386 COFF object")
        report["object_sha256"] = hashlib.sha256(data).hexdigest()
        output.mkdir(parents=True, exist_ok=True)
        for name, definition in (("kernel32", KERNEL32), ("msvcrt", MSVCRT)):
            (output / f"{name}.def").write_text(definition, encoding="utf-8")
            command = [linker, "/lib", f"/def:{output / (name + '.def')}",
                       "/machine:x86", f"/out:{output / (name + '.lib')}"]
            report["commands"].append(command)
        image = output / "frame-runtime.exe"
        report["commands"].append(
            [linker, "/entry:mainCRTStartup", "/nodefaultlib", "/machine:x86",
             "/subsystem:console", f"/out:{image}", str(source.resolve()),
             str(output / "kernel32.lib"), str(output / "msvcrt.lib")])
        report["link_steps"] = []
        for index, command in enumerate(report["commands"]):
            result = subprocess.run(command, capture_output=True, text=True,
                                    errors="replace", timeout=timeout,
                                    check=False)
            report["link_steps"].append({"exit_code": result.returncode,
                                         "stdout": result.stdout,
                                         "stderr": result.stderr})
            if result.returncode:
                raise ValueError("callback probe import generation or link failed")
            if index == 0:
                undecorate_kernel32_imports(output / "kernel32.lib")
        report["image_sha256"] = image_digest(image)
        report["runtime"] = run_image(image, launcher, environment, timeout)
        runtime = report["runtime"]
        if runtime.get("exit_code") != 0 or \
                runtime.get("stdout", "").strip() != BANNER:
            raise ValueError("generated callback frame runtime probe failed")
        report["passed"] = True
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        report["error"] = str(error)
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--object", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--linker", default="lld-link")
    parser.add_argument("--wine")
    parser.add_argument("--wine-prefix", type=Path)
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be positive and finite")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    launcher = []
    error = None
    linker = shutil.which(args.linker)
    if not linker:
        error = "lld-link is unavailable; callback probe was not linked"
    if os.name != "nt":
        wine = args.wine or shutil.which("wine") or shutil.which("wine64-stable")
        if not wine:
            error = "Wine is unavailable; callback ABI execution is missing"
        else:
            launcher = [wine]
            prefix = (args.wine_prefix or output / "wine-prefix").resolve()
            prefix.mkdir(parents=True, exist_ok=True)
            environment.update(WINEPREFIX=str(prefix), WINEDEBUG="-all")
    if error:
        report = {"schema": 1, "evidence": "generated-x86-callback-abi",
                  "passed": False, "error": error}
    else:
        report = build_and_observe(args.object, output, linker, launcher,
                                   environment, args.timeout)
    destination = output / "frame-runtime.json"
    destination.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"{'PASS' if report['passed'] else 'FAIL'} callback ABI: {destination}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
