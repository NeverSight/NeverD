#!/usr/bin/env python3
"""Execute BMI logical projections without BMI or APX hardware."""

import ctypes
from pathlib import Path
import re
import subprocess
import sys
import tempfile


COMMAND_TIMEOUT = 60


def reference(instruction, a, b, width):
    result = 0
    found = False
    for bit in range(width):
        present = bool(a & (1 << bit))
        if instruction == "andn":
            output = not present and bool(b & (1 << bit))
        elif instruction == "bzhi":
            output = present and bit < (b & 255)
        elif instruction == "blsi":
            output = present and not found
        elif instruction == "blsr":
            output = present and found
        else:
            output = not found
        if output:
            result |= 1 << bit
        found |= present
    return result


def fixture(instruction, form, width):
    # RDI is source and destination; RSI holds the output pointer, RDX input B.
    setup = bytes.fromhex("31 c0 83 e8 01")
    if instruction == "andn":
        opcode, modrm, v = 0xf2, 0xfa, 7
    elif instruction == "bzhi":
        opcode, modrm, v = 0xf5, 0xff, 2
    else:
        group = {"blsr": 1, "blsmsk": 2, "blsi": 3}[instruction]
        opcode, modrm, v = 0xf3, 0xc7 | (group << 3), 7
    p1 = ((~v & 15) << 3) | (0x80 if width == 64 else 0)
    if form == "vex":
        operation = bytes([0xc4, 0xe2, p1, opcode, modrm])
    else:
        operation = bytes([0x62, 0xf2, p1 | 4,
                           0x0c if form == "apx_nf" else 8, opcode, modrm])
    # Capture all six arithmetic flags with LAHF/SETO before masking/packing.
    flag_mask = 0x8d5 if form == "apx_nf" else 0x8c1
    return (setup + operation + bytes.fromhex(
        "48 89 3e 9f 0f 90 c1 0f b6 c4 0f b6 c9 c1 e1 0b 09 c8 25")
            + flag_mask.to_bytes(4, "little") + b"\xc3")


def command(arguments):
    result = subprocess.run([str(arg) for arg in arguments], text=True,
                            capture_output=True, timeout=COMMAND_TIMEOUT)
    assert result.returncode == 0, (arguments, result.returncode,
                                   result.stdout, result.stderr)


def run(executable, compiler):
    profiles = cases = 0
    with tempfile.TemporaryDirectory(prefix="neverd-bmi-logic-pipeline-") as directory:
        root = Path(directory)
        for instruction in ("andn", "blsi", "blsmsk", "blsr", "bzhi"):
            for form in ("vex", "apx", "apx_nf"):
                for width in (32, 64):
                    mask = (1 << width) - 1
                    values = {0, 1, mask, 1 << (width - 1),
                              0xa5195ac3fe817204, 0xffffffffffff0000}
                    values.update(1 << bit for bit in range(width))
                    binary = root / f"{instruction}-{form}-{width}.bin"
                    binary.write_bytes(fixture(instruction, form, width))
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
                                library = root / f"{stem}-{optimization}.so"
                                command([compiler, "-S", "-fPIC", "-march=x86-64",
                                         "-mno-bmi", "-mno-bmi2",
                                         "-" + optimization, source, "-o", assembly])
                                assert not re.search(r"^\s*(?:andn|blsi|blsmsk|blsr|bzhi)[lq]?\s",
                                                     assembly.read_text(), re.M), assembly
                                command([compiler, "-shared", "-fPIC", assembly,
                                         "-o", library])
                                module = ctypes.CDLL(str(library))
                                function = module.start
                                function.argtypes = [ctypes.c_uint64,
                                                     ctypes.POINTER(ctypes.c_uint64),
                                                     ctypes.c_uint64]
                                function.restype = ctypes.c_uint32
                                controls = (list(range(256)) + [0xff00, 0x1234567800000008]
                                            if instruction == "bzhi" else
                                            [0, 1, mask, 0x5ac3a519fe817204]
                                            if instruction == "andn" else [0])
                                for value in sorted(values):
                                    for control in controls:
                                        output = ctypes.c_uint64(0xfedcba9876543210)
                                        flags = function(value, ctypes.byref(output), control)
                                        result = reference(instruction, value, control, width)
                                        carry = (False if instruction == "andn" else
                                                 (control & 255) >= width if instruction == "bzhi" else
                                                 (value & mask) != 0 if instruction == "blsi" else
                                                 (value & mask) == 0)
                                        wanted_flags = (0x95 if form == "apx_nf" else
                                                        carry | ((result == 0) << 6) |
                                                        ((result >> (width - 1)) << 7))
                                        assert (output.value, flags) == (result, wanted_flags), (
                                            stem, optimization, hex(value), hex(control),
                                            (output.value, flags), (result, wanted_flags))
                                        cases += 1
                                profiles += 1
    print(f"{profiles} BMI logical source profiles and {cases} value/flag cases passed")


if __name__ == "__main__":
    run(*sys.argv[1:])
