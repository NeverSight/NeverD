#!/usr/bin/env python3
"""Check compiler-rt double-word shifts through their original C interface."""

import argparse
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory

from roundtrip import DATA, PROFILES, compile_elf, execute


SHIFTS = ("ashldi3", "ashrdi3", "lshrdi3")
WORD_MASK = (1 << 32) - 1


def caller(name):
    kind = "unsigned long long" if name == "lshrdi3" else "long long"
    return f"""extern {kind} __{name}({kind}, int);
unsigned long probe_{name}(unsigned long a, unsigned long b) {{
    unsigned high = (unsigned)a ^ ((unsigned)b >> 6) * 0x9e3779b9U;
    unsigned long long input = ((unsigned long long)high << 32) | (unsigned)a;
    unsigned long long result = __{name}(({kind})input, b & 63);
    *(volatile unsigned long *)({DATA}UL + sizeof(unsigned long)) = result >> 32;
    return (unsigned)result;
}}
"""


def result(name, a, b):
    high = a ^ ((b >> 6) * 0x9e3779b9 & WORD_MASK)
    value = (high << 32) | a
    if name == "ashrdi3" and high >> 31:
        value -= 1 << 64
    count = b & 63
    shifted = value << count if name == "ashldi3" else value >> count
    return shifted & WORD_MASK, (shifted >> 32) & WORD_MASK


def check_profile(args, work, profile):
    files = [args.builtins / (name + ".c") for name in SHIFTS]
    original = work / "original.elf"
    compile_elf(args, work, profile, files[0], original, "__" + SHIFTS[0], callees=files[1:])
    callers = work / "callers.c"
    callers.write_text("".join(caller(name) for name in SHIFTS))
    reference = work / "called.elf"
    compile_elf(args, work, profile, files[0], reference, "probe_" + SHIFTS[0],
                callees=[*files[1:], callers])
    cases = [(a, (seed << 6) | shift) for a in (0, 0xffffffff, 0x12345678, 0x80000001)
             for seed in (0, 1, 7) for shift in range(64)]

    def check(binary, name):
        expected = [result(name, a, b) for a, b in cases]
        got = execute(binary, profile, name, cases, "probe_" + name)
        for case, actual, want in zip(cases, got, expected, strict=True):
            if actual != want:
                raise AssertionError((profile, name, binary, case, actual, want))

    for name in SHIFTS:
        check(reference, name)
    module = work / "module.ll"
    subprocess.run([args.neverd, "lift", original, "-o", module], check=True)
    for opt in ("-O0", "-O2"):
        rebuilt = work / f"module{opt}.elf"
        compile_elf(args, work, profile, module, rebuilt, "probe_" + SHIFTS[0], opt, [callers])
        for name in SHIFTS:
            check(rebuilt, name)
    for name in SHIFTS:
        probe = work / f"probe_{name}.c"
        probe.write_text(caller(name))
        for route in ("highc", "llvmc"):
            source = work / f"{name}-{route}.c"
            opts = ["--llvm"] if route == "llvmc" else []
            subprocess.run([args.neverd, "decompile", "--func", "__" + name, *opts, original,
                            "-o", source], check=True)
            for opt in ("-O0", "-O2"):
                rebuilt = work / f"{name}-{route}{opt}.elf"
                compile_elf(args, work, profile, source, rebuilt, "probe_" + name, opt, [probe])
                check(rebuilt, name)
    samples = len(SHIFTS) * len(cases)
    print(f"{profile}: {samples} original and {samples * 6} rebuilt shifts matched", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for tool in ("neverd", "clang", "ld"):
        parser.add_argument("--" + tool, type=Path, required=True)
    parser.add_argument("--builtins", type=Path,
                        default=Path(__file__).resolve().parents[3] /
                        "third_party/llvm-project/compiler-rt/lib/builtins")
    parser.add_argument("--profile", action="append", choices=PROFILES)
    args = parser.parse_args()
    with TemporaryDirectory(prefix="neverd-wide-shifts-") as directory:
        for profile in args.profile or PROFILES:
            work = Path(directory) / profile
            work.mkdir()
            check_profile(args, work, profile)
