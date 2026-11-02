#!/usr/bin/env python3
"""Execute BEXTR projections without requiring BMI, TBM or APX hardware."""

import ctypes
from pathlib import Path
import re
import subprocess
import sys
import tempfile


COMMAND_TIMEOUT = 60


def reference(value, control, width):
    start = control & 255
    count = (control >> 8) & 255
    result = 0
    for bit in range(count):
        if start + bit < width and value & (1 << (start + bit)):
            result |= 1 << bit
    return result


def fixture(form, width, immediate):
    # The source/destination is RDI, the output pointer RSI, the control RDX.
    # Initialize CF=1, ZF=0, OF=0 to distinguish APX NF preservation.
    setup = bytes.fromhex("31 c0 83 e8 01")
    if form == "xop":
        operation = bytes([0x8f, 0xea, 0xf8 if width == 64 else 0x78,
                           0x10, 0xff]) + immediate.to_bytes(4, "little")
    elif form == "vex":
        operation = bytes([0xc4, 0xe2, 0xe8 if width == 64 else 0x68,
                           0xf7, 0xff])
    else:
        operation = bytes([0x62, 0xf2, 0xec if width == 64 else 0x6c,
                           0x0c if form == "apx_nf" else 8, 0xf7, 0xff])
    # Store the result, capture the defined flags, then pack them as CF/ZF/OF.
    return setup + operation + bytes.fromhex(
        "48 89 3e 0f 92 c0 0f 94 c1 41 0f 90 c0 "
        "0f b6 c0 0f b6 c9 45 0f b6 c0 "
        "d1 e1 41 c1 e0 02 09 c8 44 09 c0 c3")


def command(arguments):
    result = subprocess.run([str(arg) for arg in arguments], text=True,
                            capture_output=True, timeout=COMMAND_TIMEOUT)
    assert result.returncode == 0, (arguments, result.returncode,
                                   result.stdout, result.stderr)


def run(executable, compiler):
    profiles = cases = 0
    with tempfile.TemporaryDirectory(prefix="neverd-bextr-pipeline-") as directory:
        root = Path(directory)
        for form in ("xop", "vex", "apx", "apx_nf"):
            for width in (32, 64):
                mask = (1 << width) - 1
                values = {0, 1, mask, 1 << (width - 1), 0xa5195ac3fe817204}
                controls = {start | (count << 8)
                            for start in (0, 1, 31, 32, 63, 64, 255)
                            for count in (0, 1, 31, 32, 63, 64, 255)}
                controls.update((0xffffffff, 0xabcd0804))
                immediates = (0, 0x4000, 0x4040, 0xff3f, 0xabcd0804)
                for immediate in immediates if form == "xop" else (0,):
                    binary = root / f"{form}-{width}-{immediate}.bin"
                    binary.write_bytes(fixture(form, width, immediate))
                    for no_opt in (False, True):
                        for route in ("llvm", "highc", "llvmc"):
                            stem = f"{binary.stem}-{no_opt}-{route}"
                            source = root / (stem + (".ll" if route == "llvm" else ".c"))
                            args = [executable, "lift" if route == "llvm" else "decompile",
                                    "--loader=binary", "--processor=x86_64",
                                    "--platform=sysv", "--load-base=0x1000",
                                    "-o", source, binary]
                            if no_opt:
                                args.append("--no-opt")
                            if route == "llvmc":
                                args.append("--llvm")
                            command(args)
                            assert not re.search(r"\b(?:asm|__asm|__asm__)\b",
                                                 source.read_text()), source
                            for optimization in ("O0", "O2"):
                                assembly = root / f"{stem}-{optimization}.s"
                                library = assembly.with_suffix(".so")
                                command([compiler, "-S", "-fPIC", "-march=x86-64",
                                         "-mno-bmi", "-mno-tbm", "-" + optimization,
                                         source, "-o", assembly])
                                assert not re.search(r"^\s*bextr[lq]?\s",
                                                     assembly.read_text(), re.M), assembly
                                command([compiler, "-shared", "-fPIC", assembly,
                                         "-o", library])
                                module = ctypes.CDLL(str(library))
                                function = module.start
                                function.argtypes = [ctypes.c_uint64,
                                                     ctypes.POINTER(ctypes.c_uint64),
                                                     ctypes.c_uint64]
                                function.restype = ctypes.c_uint32
                                for value in values:
                                    for control in (immediate,) if form == "xop" else controls:
                                        output = ctypes.c_uint64(0xfedcba9876543210)
                                        flags = function(value, ctypes.byref(output), control)
                                        result = reference(value, control, width)
                                        wanted_flags = 1 if form == "apx_nf" else (result == 0) << 1
                                        assert (output.value, flags) == (result, wanted_flags), (
                                            stem, optimization, hex(value), hex(control),
                                            (output.value, flags), (result, wanted_flags))
                                        cases += 1
                                profiles += 1
    print(f"{profiles} BEXTR source profiles and {cases} value/flag cases passed")


if __name__ == "__main__":
    run(*sys.argv[1:])
