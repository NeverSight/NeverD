#!/usr/bin/env python3
"""Execute typed MIPS16 calls and classic entry stubs after C reconstruction.

An owned C callee receives a leading float and returns its bits XOR an integer.
Classic linker entry stubs are recorded as expected source refusals until their
prototype propagation is qualified. The caller takes integer payloads, so its
entry does not hide an FP transport
mistake. Rebuild each side separately against the original other side; check
both byte orders, FR=0/1, HighC/LLVMC and O0/O2 with Clang and optional GCC.
"""

import argparse
import importlib.util
import json
import random
from pathlib import Path

import roundtrip
from mips_validation_artifacts import MIPSValidationArtifacts, digest

spec = importlib.util.spec_from_file_location(
    "gpr_float", Path(__file__).with_name("gpr-float-roundtrip.py"))
gpr_float = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gpr_float)

PRELUDE = """typedef unsigned int u32;
#if defined(__clang__)
#define M16 __attribute__((mips16,nomicromips,noinline))
#else
#define M16 __attribute__((mips16,noinline))
#endif
M16 u32 fp_callee(float value, u32 mask);
"""
CALLEE = PRELUDE + """M16 u32 fp_callee(float value, u32 mask) {
  union { float f; u32 bits; } v = { .f = value };
  return mask ^ v.bits;
}
"""
CALLER = PRELUDE + """u32 kernel(u32 mask, u32 bits) {
  union { float f; u32 bits; } v = { .bits = bits };
  return fp_callee(v.f, mask);
}
"""

DOUBLE_CALLEE = PRELUDE.replace("float value", "double value") + """
M16 u32 fp_callee(double value, u32 mask) {
  union { double f; unsigned long long bits; } v = { .f = value };
  u32 high = (u32)(v.bits >> 32);
  return mask ^ (u32)v.bits ^ (high << 7 | high >> 25);
}
"""
DOUBLE_CALLER = PRELUDE.replace("float value", "double value") + """
u32 kernel(u32 mask, u32 low, u32 high) {
  union { double f; unsigned long long bits; } v = {
    .bits = (unsigned long long)high << 32 | low
  };
  return fp_callee(v.f, mask);
}
"""


def run(args, artifacts):
    rows = []
    refusals = []
    rng = random.Random(0x4d313646)
    patterns = [0, 1, 0x80000000, 0x007fffff, 0x00800000, 0x7f7fffff,
                0x7f800000, 0xff800000, 0x7fa12345, 0xffc54321,
                0x3fc00000, 0xc0200000, 0xffffffff]
    patterns += [rng.getrandbits(32) for _ in range(19)]
    if args.double:
        patterns = [0, 1, 0x8000000000000000, 0x000fffffffffffff,
                    0x0010000000000000, 0x7fefffffffffffff,
                    0x7ff0000000000000, 0xfff0000000000000,
                    0x7ff123456789abcd, 0xfff8fedcba987654,
                    0x3ff8000000000000, 0xc004000000000000,
                    0xffffffffffffffff]
        patterns += [rng.getrandbits(64) for _ in range(19)]
    cases = [(mask, bits) for mask in (0, 0x71395217, 0x80000000, 0xffffffff)
             for bits in patterns]

    def command(argv):
        roundtrip.run_command(artifacts, argv, artifacts.work / "command.log")

    modes = args.mode or ("mips16", "classic")
    for little in (False, True):
        for mode in modes:
            for fr in (0, 1):
                profile = f'{"le" if little else "be"}-{mode}-fr{fr}'
                directory = artifacts.work / profile
                directory.mkdir()
                endian = "-EL" if little else "-EB"
                fp = "-mfp64" if fr else "-mfp32"

                def compile(compiler, source, obj, source_mode, opt):
                    assembly = obj.with_suffix(".s")
                    target = (["--target=mipsel-linux-gnu" if little
                               else "--target=mips-linux-gnu", "-fno-addrsig"]
                              if compiler == args.clang else
                              [endian, "-minterlink-compressed"])
                    command([compiler, *target, "-march=mips32r2", "-mabi=32", fp,
                             "-mhard-float", "-mno-abicalls", "-fno-pic", "-G0",
                             "-ffreestanding", "-fno-stack-protector",
                             "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
                             *(["-mips16"] if source_mode == "mips16" else []),
                             opt, "-S", source, "-o", assembly])
                    if obj.name in ("caller.o", "callee.o"):
                        artifacts.keep_source(assembly)
                    command([args.assembler, "-march=mips32r2", "-mabi=32", fp,
                             endian, assembly, "-o", obj])
                    artifacts.discard(assembly)

                def link(objects, output):
                    command([args.ld, "-m", "elf32ltsmip" if little else "elf32btsmip",
                             "-Ttext", "0x10000", "-e", "kernel", *objects, "-o", output])

                caller, callee = directory / "caller.c", directory / "callee.c"
                caller.write_text(DOUBLE_CALLER if args.double else CALLER)
                callee.write_text(DOUBLE_CALLEE if args.double else CALLEE)
                caller_obj, callee_obj = directory / "caller.o", directory / "callee.o"
                compile(args.clang, caller, caller_obj, mode, "-O2")
                compile(args.clang, callee, callee_obj, "mips16", "-O2")
                original = directory / "original.elf"
                link([caller_obj, callee_obj], original)
                expected = gpr_float.observe(original, little, mode, fr, cases,
                                             split_double=args.double)
                header = directory / "declarations.h"
                scalar = "double" if args.double else "float"
                header.write_text(f"unsigned int fp_callee({scalar} value, unsigned int mask);\n")
                command([args.neverd, "types", original, "--declare", header, "--save"])
                # This separate linker-created local symbol does not yet acquire
                # the callee's source signature. Keep the refusal explicit, rather
                # than silently skipping that compiler-produced call boundary.
                if mode == "classic":
                    for route in ("highc", "llvmc"):
                        try:
                            command([args.neverd, "decompile", original, "--func", "kernel",
                                     *(["--llvm"] if route == "llvmc" else []),
                                     "-o", directory / "refused.c"])
                        except RuntimeError as error:
                            assert "MIPS floating call ABI is not qualified" in str(error)
                            refusals.append({"profile": profile, "route": route,
                                             "reason": "unbound classic MIPS16 entry stub"})
                            artifacts.write("expected-refusals.json", refusals)
                        else:
                            raise AssertionError("classic stub admission changed: execute and qualify it")
                    artifacts.discard(original, caller_obj, callee_obj, caller, callee, header)
                    continue
                functions = [("kernel", callee_obj), ("together", None)]
                if mode == "mips16":
                    functions.append(("fp_callee", caller_obj))
                for function, other in functions:
                    for route in ("highc", "llvmc"):
                        source = directory / f"{function}-{route}.c"
                        selection = (["--func", "kernel", "--callees"]
                                     if function == "together" and route == "highc" else
                                     [] if function == "together" else ["--func", function])
                        command([args.neverd, "decompile", original, *selection,
                                 *(["--llvm"] if route == "llvmc" else []), "-o", source])
                        artifacts.keep_source(source)
                        for compiler in (args.clang, *([args.gcc] if args.gcc else [])):
                            for opt in ("-O0", "-O2"):
                                obj, rebuilt = directory / "rebuilt.o", directory / "rebuilt.elf"
                                compile(compiler, source, obj, "classic", opt)
                                link([obj, other] if other else [obj], rebuilt)
                                actual = gpr_float.observe(rebuilt, little, mode, fr, cases,
                                                           split_double=args.double)
                                assert actual == expected
                                rows.append({"profile": profile, "function": function,
                                             "route": route, "compiler": str(compiler),
                                             "optimization": opt, "cases": len(cases),
                                             "original_sha256": digest(original),
                                             "source_sha256": digest(source),
                                             "rebuilt_sha256": digest(rebuilt)})
                                artifacts.write("results.json", rows)
                                artifacts.discard(obj, rebuilt)
                        artifacts.discard(source)
                artifacts.discard(original, caller_obj, callee_obj, caller, callee, header)
    artifacts.write("summary.json", {"scalar": "double" if args.double else "float",
                                    "profiles": 4 * len(modes),
                                    "original_cases": 4 * len(modes) * len(cases),
                                    "builds": len(rows), "rebuilt_cases": len(rows) * len(cases),
                                    "expected_refusals": len(refusals)})
    print(json.dumps({"builds": len(rows), "rebuilt_cases": len(rows) * len(cases)}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--double", action="store_true",
                        help="check both words of a split double parameter")
    parser.add_argument("--mode", action="append", choices=("mips16", "classic"),
                        help="select direct compressed calls or classic entry-stub calls")
    for name in ("neverd", "clang", "assembler", "ld", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--gcc", type=Path)
    args = parser.parse_args()
    with MIPSValidationArtifacts(args.output, "mips16-floating-call-roundtrip",
                                 (args.neverd, args.clang, args.assembler, args.ld)) as artifacts:
        run(args, artifacts)
