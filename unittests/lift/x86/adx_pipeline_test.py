#!/usr/bin/env python3
"""Execute independent CF/OF chains through LLVM, HighC and LLVMC."""

import ctypes
from pathlib import Path
import random
import re
import subprocess
import sys
import tempfile


COMMAND_TIMEOUT = 60


def operation(instruction, form, width, r, v, b):
    modrm = 0xc0 | ((r & 7) << 3) | b
    if form == "legacy":
        return bytes([0x66 if instruction == "adcx" else 0xf3] + ([0x48] if width == 64 else []) + [0x0f, 0x38, 0xf6, modrm])
    if form == "apx":
        v = 0
    return bytes([0x62, 0xf4, ((~v & 15) << 3) | (0x80 if width == 64 else 0) | 4 | (1 if instruction == "adcx" else 2),
                  (0 if v & 16 else 8) | (0x10 if form == "ndd" else 0), 0x66, modrm])


def fixture(instruction, form, width, cf, of):
    # RDI, RDX and RCX supply input words; RSI points to the two-word result.
    setup = bytes.fromhex("b8 ff ff ff 7f 83 c0 01" if of else "b8 00 00 00 00 83 e8 00") + bytes([0xf9 if cf else 0xf8])
    if instruction == "chains":
        body = b"".join(operation(op, form, width, r, r, 2)
                        for op, r in (("adcx", 7), ("adox", 1), ("adcx", 7), ("adox", 1)))
    else:
        body = operation(instruction, form, width, 7, 7, 2)
    output = bytes.fromhex("48 89 3e 48 89 4e 08")
    return setup + body + output + bytes.fromhex("9f 0f 90 c1 0f b6 c4 0f b6 c9 c1 e1 0b 09 c8 25 d5 08 00 00 c3")


def reference(instruction, width, a, b, c, cf, of):
    mask = (1 << width) - 1
    flags = (0x894 if of else 0x44) | cf
    def add(left, carry):
        value = (left & mask) + (b & mask) + carry
        return value & mask, value >> width
    if instruction == "chains":
        for _ in range(2):
            a, cf = add(a, cf)
            c, of = add(c, of)
    elif instruction == "adcx":
        a, cf = add(a, cf)
    else:
        a, of = add(a, of)
    return (a, c), (flags & ~0x801) | cf | (of << 11)


def command(arguments):
    result = subprocess.run([str(x) for x in arguments], text=True, capture_output=True, timeout=COMMAND_TIMEOUT)
    assert result.returncode == 0, (arguments, result.returncode, result.stdout, result.stderr)


def run(executable, compiler):
    profiles = cases = 0
    with tempfile.TemporaryDirectory(prefix="neverd-adx-pipeline-") as directory:
        root = Path(directory)
        for instruction in ("adcx", "adox", "chains"):
            for form in ("legacy", "apx", "ndd"):
                for width in (32, 64):
                    mask = (1 << width) - 1
                    values = sorted({0, 1, mask, mask - 1, 1 << (width - 1), (1 << (width - 1)) - 1, 0xfedcba9876543210})
                    rng = random.Random(0x57cc93)
                    triples = [(a, b, c) for a in values for b in values for c in values]
                    triples += [(rng.getrandbits(64), rng.getrandbits(64), rng.getrandbits(64)) for _ in range(256)]
                    for cf, of in ((0, 0), (0, 1), (1, 0), (1, 1)):
                        binary = root / f"{instruction}-{form}-{width}-{cf}-{of}.bin"
                        binary.write_bytes(fixture(instruction, form, width, cf, of))
                        answers = [reference(instruction, width, a, b, c, cf, of) for a, b, c in triples]
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
                                    command([compiler, "-S", "-fPIC", "-march=x86-64", "-mno-adx", "-" + optimization, source, "-o", assembly])
                                    assert not re.search(r"^\s*(?:adcx|adox)[lq]?\s|\{(?:nf|evex)\}", assembly.read_text(), re.M), assembly
                                    command([compiler, "-shared", "-fPIC", assembly, "-o", library])
                                    module = ctypes.CDLL(str(library))
                                    function = module.start
                                    function.argtypes = [ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.c_uint64, ctypes.c_uint64]
                                    function.restype = ctypes.c_uint32
                                    for (a, b, c), wanted in zip(triples, answers):
                                        output = (ctypes.c_uint64 * 2)(0xdeadbeef, 0x12345678)
                                        flags = function(a, output, b, c)
                                        assert (tuple(output), flags) == wanted, (stem, optimization, hex(a), hex(b), hex(c), tuple(output), flags, wanted)
                                        cases += 1
                                    profiles += 1
    print(f"{profiles} ADX source profiles and {cases} value/flag cases passed using baseline host instructions")


if __name__ == "__main__":
    run(*sys.argv[1:])
