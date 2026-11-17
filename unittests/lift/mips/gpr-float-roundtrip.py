#!/usr/bin/env python3
"""Preserve o32 GPR-carried floating payloads through typed source emission.

Compile an owned C kernel, declare its actual signature, rebuild both source
routes at O0/O2, and compare original and rebuilt machine code. Includes
classic, microMIPS and MIPS16, FR=0/1, both byte orders, NaN payloads and signed
zero. Requires the pinned Unicorn bindings, Clang and GNU MIPS as/ld; --gcc
adds an independent source compiler. Generated products have a scratch owner.
"""

import argparse
import json
import random
from pathlib import Path

from unicorn import UC_HOOK_CODE
from unicorn import mips_const as mips

import roundtrip
from mips_validation_artifacts import MIPSValidationArtifacts, digest


KERNEL = """typedef unsigned int u32;
u32 kernel(u32 mask, float value) {
  union { float f; u32 bits; } v = { .f = value };
  return mask ^ v.bits;
}
"""
DECLARATION = "unsigned int kernel(unsigned int mask, float value);\n"


def observe(path, little, mode, fr, cases, leading=False, split_double=False):
    model = (mips.UC_CPU_MIPS32_MICROMIPS32R2_GENERIC if mode == "micro"
             else mips.UC_CPU_MIPS32_74KF)
    uc, entry = roundtrip.emulator(path, 32, little, "kernel", cpu_model=model)
    uc.reg_write(mips.UC_MIPS_REG_CP0_STATUS,
                 (uc.reg_read(mips.UC_MIPS_REG_CP0_STATUS) & ~0x04000000)
                 | (0x24000000 if fr else 0x20000000))
    # Execute a classic instruction after returning, rather than treating a
    # canonical PC as proof of the caller's ISA. The incoming link is classic.
    uc.mem_write(roundtrip.STOP,
                 (0x24030051).to_bytes(4, "little" if little else "big"))
    returned = []
    uc.hook_add(UC_HOOK_CODE, lambda _uc, pc, size, _:
                returned.append(size) if pc == roundtrip.STOP else None)
    saved_gprs = {r: 0xa5010000 + r for r in (*range(16, 24), 30)}
    saved_fprs = {r: (0x3ff0000000000000 if fr else 0x3f800000) + r
                  for r in range(20, 32, 2 if fr else 1)}
    result = []
    for mask, bits in cases:
        returned.clear()
        for r in range(1, 32):
            uc.reg_write(getattr(mips, f"UC_MIPS_REG_{r}"), saved_gprs.get(r, 0))
        for r in range(32):
            uc.reg_write(getattr(mips, f"UC_MIPS_REG_F{r}"), saved_fprs.get(r, 0))
        uc.reg_write(mips.UC_MIPS_REG_SP, roundtrip.STACK)
        uc.reg_write(mips.UC_MIPS_REG_RA, roundtrip.STOP)
        if leading:
            if mode == "mips16":
                uc.reg_write(mips.UC_MIPS_REG_A0, bits)
            else:
                uc.reg_write(mips.UC_MIPS_REG_F12, bits)
            uc.reg_write(mips.UC_MIPS_REG_A1, mask)
        else:
            uc.reg_write(mips.UC_MIPS_REG_A0, mask)
            uc.reg_write(mips.UC_MIPS_REG_A1, bits & 0xffffffff)
            if split_double:
                uc.reg_write(mips.UC_MIPS_REG_A2, bits >> 32)
        uc.reg_write(mips.UC_MIPS_REG_FCSR, 0)
        uc.emu_start(entry, roundtrip.STOP + 4, count=10000)
        assert returned == [4] and uc.reg_read(mips.UC_MIPS_REG_V1) == 0x51
        assert uc.reg_read(mips.UC_MIPS_REG_SP) == roundtrip.STACK
        assert uc.reg_read(mips.UC_MIPS_REG_RA) == roundtrip.STOP
        assert uc.reg_read(mips.UC_MIPS_REG_FCSR) == 0
        for r, value in saved_gprs.items():
            assert uc.reg_read(getattr(mips, f"UC_MIPS_REG_{r}")) == value
        for r, value in saved_fprs.items():
            assert uc.reg_read(getattr(mips, f"UC_MIPS_REG_F{r}")) == value
        value = uc.reg_read(mips.UC_MIPS_REG_V0) & 0xffffffff
        expected = mask ^ bits
        if split_double:
            high = bits >> 32
            expected = mask ^ (bits & 0xffffffff) ^ ((high << 7 | high >> 25) & 0xffffffff)
        assert value == expected, (path, hex(mask), hex(bits), hex(value))
        result.append(value)
    return result


def run(args, artifacts):
    source = artifacts.work / "original.c"
    header = artifacts.work / "declarations.h"
    source.write_text(KERNEL.replace("u32 mask, float value", "float value, u32 mask")
                      if args.leading else KERNEL)
    header.write_text(DECLARATION.replace("unsigned int mask, float value",
                                         "float value, unsigned int mask")
                      if args.leading else DECLARATION)
    rng = random.Random(0x47505246)
    patterns = [0, 1, 0x80000000, 0x007fffff, 0x00800000, 0x7f7fffff,
                0x7f800000, 0xff800000, 0x7fa12345, 0xffc54321,
                0x3fc00000, 0xc0200000, 0xffffffff]
    patterns += [rng.getrandbits(32) for _ in range(19)]
    cases = [(mask, bits) for mask in (0, 0x71395217, 0x80000000, 0xffffffff)
             for bits in patterns]
    rows = []

    def command(argv):
        roundtrip.run_command(artifacts, argv, artifacts.work / "command.log")

    def build(compiler, input_source, output, little, mode, fr, optimization):
        assembly = output.with_suffix(".s")
        obj = output.with_suffix(".o")
        common = ["-march=mips32r2", "-mabi=32", "-mhard-float",
                  "-mfp64" if fr else "-mfp32", "-mno-abicalls", "-fno-pic",
                  "-ffreestanding", "-fno-stack-protector", "-fno-unwind-tables",
                  "-fno-asynchronous-unwind-tables", optimization]
        if mode != "classic":
            common += ["-mips16" if mode == "mips16" else "-mmicromips"]
        target = (["--target=mipsel-linux-gnu" if little else "--target=mips-linux-gnu",
                   "-fno-addrsig"] if compiler == args.clang
                  else ["-EL" if little else "-EB"])
        command([compiler, *target, *common, "-S", input_source, "-o", assembly])
        command([args.assembler, "-march=mips32r2", "-mabi=32",
                 "-mfp64" if fr else "-mfp32", "-EL" if little else "-EB",
                 assembly, "-o", obj])
        command([args.ld, "-m", "elf32ltsmip" if little else "elf32btsmip",
                 "-Ttext", "0x10000", "-e", "kernel", obj, "-o", output])
        artifacts.discard(assembly, obj)

    for little in (False, True):
        for mode in ("classic", "micro", "mips16"):
            for fr in (0, 1):
                profile = f'{"le" if little else "be"}-{mode}-fr{fr}'
                original = artifacts.work / f"{profile}.elf"
                build(args.clang, source, original, little, mode, fr, "-O2")
                expected = observe(original, little, mode, fr, cases, args.leading)
                command([args.neverd, "types", original, "--declare", header, "--save"])
                for route in ("highc", "llvmc"):
                    output = artifacts.work / f"{profile}-{route}.c"
                    command([args.neverd, "decompile", original, "--func", "kernel",
                             *(["--llvm"] if route == "llvmc" else []), "-o", output])
                    if route == "highc":
                        assert (("kernel(float value, uint32_t mask) {" if args.leading else
                                 "kernel(uint32_t mask, float value) {") in output.read_text())
                        assert "__builtin_bit_cast(" in output.read_text()
                    artifacts.keep_source(output)
                    for compiler in (args.clang, *([args.gcc] if args.gcc else [])):
                        for optimization in ("-O0", "-O2"):
                            rebuilt = artifacts.work / "rebuilt.elf"
                            build(compiler, output, rebuilt, little, "classic", fr, optimization)
                            observed = observe(rebuilt, little, mode, fr, cases, args.leading)
                            assert observed == expected
                            rows.append({"profile": profile, "route": route,
                                         "compiler": str(compiler), "optimization": optimization,
                                         "cases": len(cases), "original_sha256": digest(original),
                                         "source_sha256": digest(output), "rebuilt_sha256": digest(rebuilt)})
                            artifacts.write("results.json", rows)
                            artifacts.discard(rebuilt)
                    artifacts.discard(output)
                artifacts.discard(original)
    artifacts.write("summary.json", {"leading": args.leading, "profiles": 12, "original_cases": 12 * len(cases),
                                    "builds": len(rows), "rebuilt_cases": len(rows) * len(cases)})
    print(json.dumps({"builds": len(rows), "rebuilt_cases": len(rows) * len(cases)}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--leading", action="store_true",
                        help="put the float before the integer, testing the MIPS16 entry transport")
    for name in ("neverd", "clang", "assembler", "ld", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--gcc", type=Path)
    args = parser.parse_args()
    with MIPSValidationArtifacts(args.output, "mips-gpr-float-roundtrip",
                                 (args.neverd, args.clang, args.assembler, args.ld)) as artifacts:
        run(args, artifacts)
