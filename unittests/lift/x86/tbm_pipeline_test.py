#!/usr/bin/env python3
"""Execute all TBM value/flag projections without executing TBM hardware."""

import ctypes
from pathlib import Path
import re
import subprocess
import sys
import tempfile


OPERATIONS = {
    "blcfill": (1, 1), "blci": (2, 6), "blcic": (1, 5),
    "blcmsk": (2, 1), "blcs": (1, 3), "blsfill": (1, 2),
    "blsic": (1, 6), "t1mskc": (1, 7), "tzmsk": (1, 4),
}
COMMAND_TIMEOUT = 60


def reference(operation, value, width):
    """Find the first set/clear bit independently of the lifter's +/-1."""
    mask = (1 << width) - 1
    value &= mask
    seek_one = operation in ("blsfill", "blsic", "tzmsk")
    first = next((i for i in range(width)
                  if bool((value >> i) & 1) == seek_one), width)
    below = (1 << first) - 1
    bit = 0 if first == width else 1 << first
    result = {
        "blcfill": value & ~below, "blci": mask ^ bit, "blcic": bit,
        "blcmsk": below | bit, "blcs": value | bit,
        "blsfill": value | below, "blsic": mask ^ bit,
        "t1mskc": mask ^ below, "tzmsk": below,
    }[operation]
    carry = value == (0 if seek_one else mask)
    flags = int(carry) | ((result == 0) << 1) | ((result >> (width - 1)) << 2)
    return result, flags


def fixture(operation, width):
    opcode, group = OPERATIONS[operation]
    # TBM edi/rdi, edi/rdi; store its zero-extended result through rsi.
    # Capture CF/ZF/SF/OF before packing the four bits into the return value.
    return bytes([0x8f, 0xe9, 0xc0 if width == 64 else 0x40,
                  opcode, 0xc7 | (group << 3)]) + bytes.fromhex(
        "48 89 3e 0f 92 c0 0f 94 c1 0f 98 c2 41 0f 90 c0 "
        "0f b6 c0 0f b6 c9 0f b6 d2 45 0f b6 c0 "
        "d1 e1 c1 e2 02 41 c1 e0 03 09 c8 09 d0 44 09 c0 c3")


def run_command(arguments):
    result = subprocess.run([str(arg) for arg in arguments], text=True,
                            capture_output=True, timeout=COMMAND_TIMEOUT)
    assert result.returncode == 0, (arguments, result.returncode,
                                   result.stdout, result.stderr)


def run(executable, compiler):
    profiles = cases = 0
    with tempfile.TemporaryDirectory(prefix="neverd-tbm-pipeline-") as directory:
        root = Path(directory)
        for operation in OPERATIONS:
            for width in (32, 64):
                binary = root / f"{operation}-{width}.bin"
                binary.write_bytes(fixture(operation, width))
                mask = (1 << width) - 1
                values = {0, 1, 2, mask, mask - 1, 0xa5195ac3fe817204}
                for bit in range(width):
                    values.update((1 << bit, (1 << bit) - 1, mask ^ (1 << bit)))
                for no_opt in (False, True):
                    for route in ("llvm", "highc", "llvmc"):
                        stem = f"{operation}-{width}-{no_opt}-{route}"
                        source = root / (stem + (".ll" if route == "llvm" else ".c"))
                        arguments = [executable,
                                     "lift" if route == "llvm" else "decompile",
                                     "--loader=binary", "--processor=x86_64",
                                     "--platform=sysv", "--load-base=0x1000",
                                     "-o", source, binary]
                        if no_opt:
                            arguments.append("--no-opt")
                        if route == "llvmc":
                            arguments.append("--llvm")
                        run_command(arguments)
                        text = source.read_text()
                        assert not re.search(r"\b(?:asm|__asm|__asm__)\b", text), source
                        for optimization in ("O0", "O2"):
                            assembly = root / f"{stem}-{optimization}.s"
                            library = assembly.with_suffix(".so")
                            run_command([compiler, "-S", "-fPIC", "-march=x86-64",
                                         "-mno-tbm", "-" + optimization,
                                         source, "-o", assembly])
                            # Inspect exactly the assembly that will be linked.
                            # The original input bytes are never executed.
                            forbidden = r"^\s*(?:" + "|".join(OPERATIONS) + r")[lq]?\s"
                            assert not re.search(forbidden, assembly.read_text(), re.M), assembly
                            run_command([compiler, "-shared", "-fPIC", assembly,
                                         "-o", library])
                            module = ctypes.CDLL(str(library))
                            function = module.start
                            function.argtypes = [ctypes.c_uint64,
                                                 ctypes.POINTER(ctypes.c_uint64)]
                            function.restype = ctypes.c_uint32
                            for value in sorted(values):
                                result = ctypes.c_uint64(0xfedcba9876543210)
                                flags = function(value, ctypes.byref(result))
                                wanted = reference(operation, value, width)
                                assert (result.value, flags) == wanted, (
                                    stem, optimization, hex(value),
                                    (result.value, flags), wanted)
                                cases += 1
                            profiles += 1
    print(f"{profiles} TBM source profiles and {cases} value/flag cases passed")


if __name__ == "__main__":
    run(*sys.argv[1:])
