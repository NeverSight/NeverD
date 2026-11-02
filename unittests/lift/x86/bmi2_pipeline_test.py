#!/usr/bin/env python3
"""Execute VEX/APX BMI2 projections and verify preserved status flags."""

import ctypes
from pathlib import Path
import random
import re
import subprocess
import sys
import tempfile


COMMAND_TIMEOUT = 60


def reference(instruction, a, b, width):
    mask = (1 << width) - 1
    a &= mask
    b &= mask
    if instruction == "mulx":
        return (a * b) & mask, (a * b) >> width
    result = packed = 0
    count = b % width
    for bit in range(width):
        if instruction in ("pdep", "pext"):
            if not (b >> bit) & 1:
                continue
            source, destination = ((packed, bit) if instruction == "pdep" else (bit, packed))
            result |= ((a >> source) & 1) << destination
            packed += 1
        else:
            source = bit - count if instruction == "shlx" else bit + count
            present = ((a >> (source % width)) & 1 if instruction == "rorx" else
                       False if source < 0 else
                       instruction == "sarx" and (a >> (width - 1)) & 1
                       if source >= width else (a >> source) & 1)
            result |= bool(present) << bit
    return result, 0


def fixture(instruction, form, width, immediate=31):
    # RDI and RDX supply inputs, RSI the two-word output pointer.
    setup = bytes.fromhex("31 c0 83 e8 01")  # All status flags defined: 0x95.
    r = 8 if instruction == "mulx" else 7
    v = 7 if instruction in ("mulx", "pdep", "pext") else 0 if instruction == "rorx" else 2
    b = 2 if instruction in ("pdep", "pext") else 7
    pp = 1 if instruction == "shlx" else 2 if instruction in ("pext", "sarx") else 3
    opcode = 0xf6 if instruction == "mulx" else 0xf5 if instruction in ("pdep", "pext") else 0xf0 if instruction == "rorx" else 0xf7
    map_id = 3 if instruction == "rorx" else 2
    p1 = ((~v & 15) << 3) | (0x80 if width == 64 else 0) | pp
    modrm = 0xc0 | ((r & 7) << 3) | b
    if form == "vex":
        operation = bytes([0xc4, (0xe0 | map_id) ^ (0x80 if r & 8 else 0), p1, opcode, modrm])
    else:
        operation = bytes([0x62, (0xf0 | map_id) ^ (0x80 if r & 8 else 0), p1 | 4, 8, opcode, modrm])
    if instruction == "rorx":
        operation += bytes([immediate])
    output = bytes.fromhex("48 89 3e")
    output += bytes.fromhex("4c 89 46 08" if instruction == "mulx" else "48 c7 46 08 00 00 00 00")
    # Sample CF/PF/AF/ZF/SF/OF without depending on unspecified machine flags.
    return setup + operation + output + bytes.fromhex("9f 0f 90 c1 0f b6 c4 0f b6 c9 c1 e1 0b 09 c8 25 d5 08 00 00 c3")


def command(arguments):
    result = subprocess.run([str(x) for x in arguments], text=True, capture_output=True, timeout=COMMAND_TIMEOUT)
    assert result.returncode == 0, (arguments, result.returncode, result.stdout, result.stderr)


def run(executable, compiler):
    has_bmi2 = bool(re.search(r"\bbmi2\b", Path("/proc/cpuinfo").read_text()))
    profiles = cases = unexecuted = 0
    with tempfile.TemporaryDirectory(prefix="neverd-bmi2-pipeline-") as directory:
        root = Path(directory)
        for instruction in ("mulx", "pdep", "pext", "rorx", "sarx", "shlx", "shrx"):
            for form in ("vex", "apx"):
                for width in (32, 64):
                    mask = (1 << width) - 1
                    values = sorted({0, 1, mask, 0xfedcba9876543210, 0xffff000000000001} | {1 << bit for bit in range(width)})
                    for immediate in ((0, 31, 64, 255) if instruction == "rorx" else (31,)):
                        binary = root / f"{instruction}-{form}-{width}-{immediate}.bin"
                        binary.write_bytes(fixture(instruction, form, width, immediate))
                        rng = random.Random(0x938249cd)
                        if instruction in ("shlx", "shrx", "sarx"):
                            pairs = [(a, b) for a in values for b in range(256)]
                            pairs += [(a, 0x1234567800000000 | b) for a in values for b in (0, 31, 32, 63, 64, 255)]
                        elif instruction == "rorx":
                            pairs = [(a, immediate) for a in values]
                        else:
                            pairs = [(a, b) for a in values for b in values]
                            pairs += [(rng.getrandbits(64), rng.getrandbits(64)) for _ in range(1024)]
                        answers = [reference(instruction, a, b, width) for a, b in pairs]
                        for no_opt in (False, True):
                            for route in ("llvm", "highc", "llvmc"):
                                stem = f"{binary.stem}-{no_opt}-{route}"
                                source = root / (stem + (".ll" if route == "llvm" else ".c"))
                                args = [executable, "lift" if route == "llvm" else "decompile", "--loader=binary", "--processor=x86_64", "--platform=sysv", "--load-base=0x1000", "-o", source, binary]
                                if no_opt:
                                    args.append("--no-opt")
                                if route == "llvmc":
                                    args.append("--llvm")
                                command(args)
                                assert not re.search(r"\b(?:asm|__asm|__asm__)\b", source.read_text()), source
                                for optimization in ("O0", "O2"):
                                    assembly = root / f"{stem}-{optimization}.s"
                                    library = root / f"{stem}-{optimization}.so"
                                    native_bits = instruction in ("pdep", "pext")
                                    command([compiler, "-S", "-fPIC", "-march=x86-64", "-mbmi2" if native_bits else "-mno-bmi2", "-" + optimization, source, "-o", assembly])
                                    text = assembly.read_text()
                                    assert not re.search(r"\{(?:nf|evex)\}|\br(?:1[6-9]|2\d|3[01])\b", text), assembly
                                    if not native_bits:
                                        assert not re.search(r"^\s*(?:mulx|rorx|sarx|shlx|shrx)[lq]?\s", text, re.M), assembly
                                    command([compiler, "-shared", "-fPIC", assembly, "-o", library])
                                    profiles += 1
                                    if native_bits and not has_bmi2:
                                        unexecuted += 1
                                        continue
                                    module = ctypes.CDLL(str(library))
                                    function = module.start
                                    function.argtypes = [ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.c_uint64]
                                    function.restype = ctypes.c_uint32
                                    for (a, b), wanted in zip(pairs, answers):
                                        output = (ctypes.c_uint64 * 2)(0xdeadbeef, 0x12345678)
                                        flags = function(a, output, b)
                                        assert (tuple(output), flags) == (wanted, 0x95), (stem, optimization, hex(a), hex(b), tuple(output), flags, wanted)
                                        cases += 1
    print(f"{profiles} BMI2 source profiles and {cases} value/flag cases passed; {unexecuted} PDEP/PEXT profiles compiled without runtime BMI2 qualification")


if __name__ == "__main__":
    run(*sys.argv[1:])
