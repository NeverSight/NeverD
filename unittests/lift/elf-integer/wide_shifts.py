#!/usr/bin/env python3
"""Check compiler-rt double-word shifts through their original C interface."""

import argparse
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory

from elftools.elf.elffile import ELFFile

from roundtrip import DATA, PROFILES, compile_elf, execute


SHIFTS = ("ashldi3", "ashrdi3", "lshrdi3")
WORD_MASK = (1 << 32) - 1
HIGH_WORDS = (0, 0x80000001, 0x7fff1234, WORD_MASK)


def caller(name, symbol):
    kind = "unsigned long long" if name == "lshrdi3" else "long long"
    high_words = ", ".join(f"{word:#x}U" for word in HIGH_WORDS)
    return f"""extern {kind} {symbol}({kind}, int);
unsigned long probe_{name}(unsigned long a, unsigned long b) {{
    static const unsigned high_words[] = {{{high_words}}};
    unsigned high = high_words[(unsigned)b >> 6];
    unsigned long long input = ((unsigned long long)high << 32) | (unsigned)a;
    unsigned long long result = {symbol}(({kind})input, b & 63);
    *(volatile unsigned long *)({DATA}UL + sizeof(unsigned long)) = result >> 32;
    return (unsigned)result;
}}
"""


def result(name, a, b):
    high = HIGH_WORDS[b >> 6]
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
    callers.write_text("".join(caller(name, "__" + name) for name in SHIFTS))
    reference = work / "called.elf"
    compile_elf(args, work, profile, files[0], reference, "probe_" + SHIFTS[0],
                callees=[*files[1:], callers])
    cases = [(a, (seed << 6) | shift) for a in (0, 0xffffffff, 0x12345678, 0x80000001)
             for seed in range(len(HIGH_WORDS)) for shift in range(64)]

    def check(binary, name):
        expected = [result(name, a, b) for a, b in cases]
        got = execute(binary, profile, name, cases, "probe_" + name)
        for case, actual, want in zip(cases, got, expected, strict=True):
            if actual != want:
                raise AssertionError((profile, name, binary, case, actual, want))

    for name in SHIFTS:
        check(reference, name)

    def rebuild(image, entries, flags=()):
        module = work / "module.ll"
        subprocess.run([args.neverd, "lift", image, "-o", module], check=True)
        callers.write_text("".join(caller(name, symbol) for name, _, symbol in entries))
        for opt in ("-O0", "-O2"):
            rebuilt = work / f"module{opt}.elf"
            compile_elf(args, work, profile, module, rebuilt, "probe_" + entries[0][0],
                        opt, [callers], flags)
            for name, _, _ in entries:
                check(rebuilt, name)
        for name, selector, symbol in entries:
            probe = work / f"probe_{name}.c"
            probe.write_text(caller(name, symbol))
            for route in ("highc", "llvmc"):
                source = work / f"{name}-{route}.c"
                opts = ["--llvm"] if route == "llvmc" else []
                subprocess.run([args.neverd, "decompile", "--func", selector, *opts, image,
                                "-o", source], check=True)
                for opt in ("-O0", "-O2"):
                    rebuilt = work / f"{name}-{route}{opt}.elf"
                    compile_elf(args, work, profile, source, rebuilt, "probe_" + name,
                                opt, [probe], flags)
                    check(rebuilt, name)

    rebuild(original, [(name, "__" + name, "__" + name) for name in SHIFTS])
    variants = [(), ("-march=rv32i",)] if profile == "riscv32" else [()]
    for flags in variants:
        if flags:
            callers.write_text("".join(caller(name, "__" + name) for name in SHIFTS))
            compile_elf(args, work, profile, files[0], reference, "probe_" + SHIFTS[0],
                        callees=[*files[1:], callers], extra_flags=flags)
            for name in SHIFTS:
                check(reference, name)
        for name, source in zip(SHIFTS, files, strict=True):
            stripped = work / f"{name}-stripped.elf"
            compile_elf(args, work, profile, source, stripped, "__" + name,
                        extra_flags=[*flags, "-Wl,--strip-all"])
            with stripped.open("rb") as stream:
                elf = ELFFile(stream)
                assert not elf.get_section_by_name(".symtab") and not elf.get_section_by_name(".dynsym")
                entry = elf["e_entry"]
            rebuild(stripped, [(name, hex(entry), f"sub_{entry:X}")], flags)
    samples = len(SHIFTS) * len(cases)
    print(f"{profile}: {samples * len(variants)} original, {samples * 6} named and "
          f"{samples * 6 * len(variants)} stripped rebuilt shifts matched", flush=True)


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
