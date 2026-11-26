#!/usr/bin/env python3
"""Execute PowerPC word products, recorded CR and overflow across source routes."""

import argparse
import itertools
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory

from roundtrip import DATA, compile_elf, execute, powerpc_xform_opcodes


def signed(value, bits):
    sign = 1 << (bits - 1)
    return ((value & (sign * 2 - 1)) ^ sign) - sign


def comparison(value):
    return 8 if value < 0 else 4 if value > 0 else 2


def check_original(name, bits, cases, observed):
    if name == "repeated_high":
        # Each instruction independently chooses its unspecified bits.
        if any(result & 0xffffffff or flags & ~7 for result, flags in observed):
            raise AssertionError("repeated products disagree in their defined low words")
        return
    high = name.startswith("high_")
    width = bits if name.startswith("double_") else 32
    mask = (1 << width) - 1
    for (a, b), (result, flags) in zip(cases, observed, strict=True):
        product = ((a & mask) * (b & mask) if name.startswith("high_unsigned")
                   else signed(a, width) * signed(b, width))
        overflow = not high and signed(product, width) != product
        so = int(bool(b & (1 << 31)) or overflow)
        defined = (product >> 32) if high else product
        result_mask = mask if high else (1 << bits) - 1
        cr1 = comparison(signed(defined, 32)) | so
        cr0 = ((comparison(signed(defined, bits)) | so) if name.endswith("_record")
               else (a >> 28) & 15)
        flag_mask = 0x1f if high and bits == 64 and name.endswith("_record") else 0xff
        expected_flags = (cr0 << 4) | cr1
        if (result ^ defined) & result_mask or (flags ^ expected_flags) & flag_mask:
            raise AssertionError((name, bits, hex(a), hex(b), hex(result), hex(flags),
                                  hex(defined & result_mask), hex(expected_flags), hex(flag_mask)))


def caller(name, bits):
    # Compile independently with the original ABI, including the third argument.
    return f"""extern unsigned long {name}(unsigned long, unsigned long, unsigned long *);
unsigned long probe_{name}(unsigned long a, unsigned long b) {{
  return {name}(a, b, (unsigned long *){DATA + bits // 8:#x}UL);
}}
"""


def check_profile(args, directory, profile):
    bits = 64 if "64" in profile else 32
    mask = (1 << bits) - 1
    values = sorted({v & mask for v in (0, 1, 2, 0x10000, 0x7fffffff, 0x80000000,
                                        0xffffffff, mask >> 1, 1 << (bits - 1), mask,
                                        0x12345678ffffffff, 0xfedcba9800000002)})
    cases = list(itertools.product(values, repeat=2))
    names = ["high_signed", "high_unsigned", "high_signed_record", "high_unsigned_record",
             "word_overflow", "word_overflow_record"]
    if bits == 64:
        names.extend(("double_overflow", "double_overflow_record", "repeated_high"))
    work = directory / profile
    work.mkdir()
    asm = Path(__file__).with_name("ppc-products.S")
    original = work / "original.elf"
    compile_elf(args, work, profile, asm, original, names[0])
    callers = work / "callers.c"
    callers.write_text("".join(caller(n, bits) for n in names))
    reference = work / "reference.elf"
    compile_elf(args, work, profile, asm, reference, "probe_" + names[0], callees=[callers])
    expected = {}
    for name in names:
        expected[name] = execute(reference, profile, name, cases, "probe_" + name)
        check_original(name, bits, cases, expected[name])
    pattern = powerpc_xform_opcodes(original, "repeated_high")[0] if bits == 64 else None

    def check(binary, name):
        if name == "repeated_high" and powerpc_xform_opcodes(binary, name).count(pattern) != 2:
            raise AssertionError(f"{binary} must retain both recorded word products")
        got = execute(binary, profile, name, cases, "probe_" + name)
        if got != expected[name]:
            i = next(i for i, pair in enumerate(zip(got, expected[name])) if pair[0] != pair[1])
            raise AssertionError((binary, name, cases[i], got[i], expected[name][i]))

    if bits == 64:
        check(reference, "repeated_high")
    module = work / "module.ll"
    subprocess.run([args.neverd, "lift", "-o", module, original], check=True)
    for opt in ("-O0", "-O2"):
        binary = work / f"module{opt}.elf"
        compile_elf(args, work, profile, module, binary, "probe_" + names[0], opt, [callers])
        for name in names:
            check(binary, name)

    for name in names:
        probe = work / "probe.c"
        probe.write_text(caller(name, bits))
        for route in ("highc", "llvmc"):
            source = work / f"{name}-{route}.c"
            options = ["--llvm"] if route == "llvmc" else []
            subprocess.run([args.neverd, "decompile", "--func", name, *options,
                            "-o", source, original], check=True)
            with source.open("a") as out:
                out.write(f'\n_Static_assert(sizeof({name}(0, 0, 0)) == sizeof(unsigned long), '
                          '"product result width");\n')
            for opt in ("-O0", "-O2"):
                binary = work / f"{name}-{route}{opt}.elf"
                compile_elf(args, work, profile, source, binary, "probe_" + name, opt, [probe])
                check(binary, name)
    print(f"{profile}: {len(names) * len(cases)} original and "
          f"{len(names) * len(cases) * 6} rebuilt products matched", flush=True)


if __name__ == "__main__":
    profiles = ("powerpc", "powerpcle", "powerpc64", "powerpc64le")
    parser = argparse.ArgumentParser(description=__doc__)
    for tool in ("neverd", "clang", "ld"):
        parser.add_argument("--" + tool, required=True, type=Path)
    parser.add_argument("--profile", action="append", choices=profiles)
    args = parser.parse_args()
    with TemporaryDirectory(prefix="neverd-ppc-products-") as temp:
        for profile in args.profile or profiles:
            check_profile(args, Path(temp), profile)
