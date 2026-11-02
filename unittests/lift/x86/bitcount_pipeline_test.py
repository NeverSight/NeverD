#!/usr/bin/env python3
"""Execute bit-count projections with a baseline x86-64 target compiler."""

import ctypes
from pathlib import Path
import re
import subprocess
import sys
import tempfile


COMMAND_TIMEOUT = 60


def reference(instruction, value, width):
    value &= (1 << width) - 1
    count = 0
    for position in range(width):
        bit = width - 1 - position if instruction == "lzcnt" else position
        present = bool(value & (1 << bit))
        if instruction == "popcnt":
            count += present
        elif present:
            break
        else:
            count += 1
    return count


def fixture(instruction, form, width):
    # The aliased source/destination is RDI, and RSI points at the result.
    # SUB initializes CF/PF/AF/SF=1 and OF/ZF=0 for the NF checks.
    setup = bytes.fromhex("31 c0 83 e8 01")
    opcode = {"tzcnt": 0xbc, "lzcnt": 0xbd, "popcnt": 0xb8}[instruction]
    if form in ("legacy", "rex2"):
        operation = bytearray([0xf3])
        if width == 16:
            operation.append(0x66)
        if form == "rex2":
            operation.extend((0xd5, 0x88 if width == 64 else 0x80))
        else:
            if width == 64:
                operation.append(0x48)
            operation.append(0x0f)
        operation.extend((opcode, 0xff))
    else:
        opcode = {"tzcnt": 0xf4, "lzcnt": 0xf5, "popcnt": 0x88}[instruction]
        operation = bytes([0x62, 0xf4,
                           0xfc if width == 64 else 0x7d if width == 16 else 0x7c,
                           0x0c if form == "apx_nf" else 8, opcode, 0xff])
    # Store the full GPR to observe 16-bit preservation/32-bit zero extension.
    # LAHF and SETO snapshot the six status flags before packing/masking them.
    # PUSHFQ would also sample unmodeled machine state with native assembly.
    flag_mask = 0x8d5 if instruction == "popcnt" or form == "apx_nf" else 0x41
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
    with tempfile.TemporaryDirectory(prefix="neverd-bitcount-pipeline-") as directory:
        root = Path(directory)
        for instruction in ("tzcnt", "lzcnt", "popcnt"):
            for form in ("legacy", "rex2", "apx", "apx_nf"):
                for width in (16, 32, 64):
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
                                         "-mno-bmi", "-mno-lzcnt", "-mno-popcnt",
                                         "-" + optimization, source, "-o", assembly])
                                assert not re.search(r"^\s*(?:tzcnt|lzcnt|popcnt)[wlq]?\s",
                                                     assembly.read_text(), re.M), assembly
                                command([compiler, "-shared", "-fPIC", assembly,
                                         "-o", library])
                                module = ctypes.CDLL(str(library))
                                function = module.start
                                function.argtypes = [ctypes.c_uint64,
                                                     ctypes.POINTER(ctypes.c_uint64)]
                                function.restype = ctypes.c_uint32
                                for value in sorted(values):
                                    output = ctypes.c_uint64(0xfedcba9876543210)
                                    flags = function(value, ctypes.byref(output))
                                    count = reference(instruction, value, width)
                                    result = (value & ~mask) | count if width == 16 else count
                                    wanted_flags = (0x95 if form == "apx_nf" else
                                                    (count == 0) << 6)
                                    if form != "apx_nf" and instruction != "popcnt":
                                        wanted_flags |= (value & mask) == 0
                                    assert (output.value, flags) == (result, wanted_flags), (
                                        stem, optimization, hex(value),
                                        (output.value, flags), (result, wanted_flags))
                                    cases += 1
                                profiles += 1
    print(f"{profiles} bit-count source profiles and {cases} value/flag cases passed")


if __name__ == "__main__":
    run(*sys.argv[1:])
