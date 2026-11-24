#!/usr/bin/env python3
"""Compile and execute the C and LLVM projections of linked integer ELF functions."""

import argparse
import itertools
from pathlib import Path
import subprocess
import tempfile

from elftools.elf.elffile import ELFFile
import unicorn as uc
from unicorn import ppc_const, riscv_const


PROFILES = ("riscv32", "riscv64", "powerpc", "powerpcle", "powerpc64", "powerpc64le")
DATA = 0x30000000
STACK = 0x40000000
RETURN = 0x50000000


def compiler_flags(profile):
    wide = "64" in profile
    if profile.startswith("riscv"):
        isa = ["-march=rv64imac", "-mabi=lp64"] if wide else ["-march=rv32imac", "-mabi=ilp32"]
    else:
        isa = ["-mabi=elfv2", "-mcpu=power8"] if wide else ["-mcpu=ppc"]
    return [f"--target={profile}-linux-gnu", *isa, "-ffreestanding",
            "-fno-pic", "-fno-stack-protector", "-fno-unwind-tables",
            "-fno-asynchronous-unwind-tables"]


def compile_elf(args, work, profile, source, output, entry, optimization="-O2", callees=()):
    dialect = ["-std=gnu17", "-I", work] if source.suffix == ".c" else []
    subprocess.run([args.clang, *compiler_flags(profile), optimization, *dialect, "-nostdlib",
                    "-static", f"-fuse-ld={args.ld}", f"-Wl,-e,{entry}",
                    source, *callees, "-o", output], check=True)


def machine(path, profile):
    wide = "64" in profile
    rv = profile.startswith("riscv")
    if rv:
        cpu = uc.Uc(uc.UC_ARCH_RISCV, uc.UC_MODE_RISCV64 if wide else uc.UC_MODE_RISCV32)
    else:
        mode = uc.UC_MODE_PPC64 if wide else uc.UC_MODE_PPC32
        cpu = uc.Uc(uc.UC_ARCH_PPC, uc.UC_MODE_BIG_ENDIAN | mode)
        if wide:
            cpu.ctl_set_tlb_mode(uc.UC_TLB_VIRTUAL)
        cpu.reg_write(ppc_const.UC_PPC_REG_MSR, (int(wide) << 63) | int(profile.endswith("le")))
    with path.open("rb") as file:
        elf = ELFFile(file)
        sections = [s for s in elf.iter_segments() if s["p_type"] == "PT_LOAD" and s["p_memsz"]]
        # LOAD segments may share an aligned page.
        pages = {p for s in sections for p in range(s["p_vaddr"] & ~4095,
                 s["p_vaddr"] + s["p_memsz"], 4096)}
        for page in sorted(pages):
            cpu.mem_map(page, 4096)
        for s in sections:
            if s["p_filesz"]:
                cpu.mem_write(s["p_vaddr"], s.data())
        symbols = {s.name: s["st_value"] for s in elf.get_section_by_name(".symtab").iter_symbols()
                   if s["st_info"]["type"] == "STT_FUNC"}
    for page in (DATA, STACK, RETURN):
        cpu.mem_map(page, 4096)
    return cpu, symbols


def execute(path, profile, function, cases):
    cpu, symbols = machine(path, profile)
    rv = profile.startswith("riscv")
    size = 8 if "64" in profile else 4
    order = "little" if rv or profile.endswith("le") else "big"
    first = riscv_const.UC_RISCV_REG_A0 if rv else ppc_const.UC_PPC_REG_3
    sp = riscv_const.UC_RISCV_REG_SP if rv else ppc_const.UC_PPC_REG_1
    link = riscv_const.UC_RISCV_REG_RA if rv else ppc_const.UC_PPC_REG_LR
    pc = riscv_const.UC_RISCV_REG_PC if rv else ppc_const.UC_PPC_REG_PC
    gprs = ([riscv_const.UC_RISCV_REG_X0 + n for n in (3, 4, 8, 9, *range(18, 28))] if rv
            else [ppc_const.UC_PPC_REG_0 + n for n in range(14, 32)])
    saved = {register: 0x12340000 + register for register in gprs}
    if not rv:
        saved.update({ppc_const.UC_PPC_REG_CR0 + n: n + 8 for n in (2, 3, 4)})
    if function not in symbols:
        raise RuntimeError(f"{profile}: {path.name} has no definition of {function}")
    start = symbols[function]
    results = []
    for a, b in cases:
        cpu.mem_write(DATA + size, a.to_bytes(size, order))
        cpu.reg_write(first, DATA if function in ("access_word", "atomic_word") else a)
        cpu.reg_write(first + 1, b)
        # PowerPC permits a callee to save LR in its caller's linkage area.
        cpu.reg_write(sp, STACK + 2048)
        cpu.reg_write(link, RETURN)
        if function == "call_mix":
            for register, value in saved.items():
                cpu.reg_write(register, value)
        if not rv:
            cpu.reg_write(ppc_const.UC_PPC_REG_12, start)
        cpu.emu_start(start, RETURN, count=1000)
        if cpu.reg_read(pc) != RETURN:
            raise RuntimeError(f"{profile} {function}: did not return")
        if cpu.reg_read(sp) != STACK + 2048:
            raise RuntimeError(f"{profile} {function}: stack pointer changed")
        if function == "call_mix" and any(cpu.reg_read(r) != v for r, v in saved.items()):
            raise RuntimeError(f"{profile} {function}: callee-saved register changed")
        results.append((cpu.reg_read(first), int.from_bytes(cpu.mem_read(DATA + size, size), order)))
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for program in ("neverd", "clang", "ld"):
        parser.add_argument("--" + program, required=True, type=Path)
    parser.add_argument("--profile", action="append", choices=PROFILES)
    args = parser.parse_args()
    fixture = Path(__file__).with_name("integer.c")
    with tempfile.TemporaryDirectory(prefix="neverd-elf-integer-") as directory:
        work = Path(directory)
        (work / "string.h").write_text("#include <stddef.h>\nvoid *memcpy(void *, const void *, size_t);\n"
                                       "void *memset(void *, int, size_t);\n")
        for profile in args.profile or PROFILES:
            original = work / f"{profile}.elf"
            compile_elf(args, work, profile, fixture, original, "mix")
            bits = 64 if "64" in profile else 32
            mask = (1 << bits) - 1
            values = [0, 1, 1 << (bits - 1), mask, 0x87654321]
            cases = list(itertools.product(values, repeat=2))
            module = work / f"{profile}.ll"
            subprocess.run([args.neverd, "lift", "-o", module, original], check=True)
            module_binaries = {}
            for optimization in ("-O0", "-O2"):
                binary = work / f"{profile}-llvm{optimization}.elf"
                compile_elf(args, work, profile, module, binary, "mix", optimization)
                module_binaries[optimization] = binary
            for function in ("mix", "product", "branch", "access_word", "atomic_word", "call_mix"):
                expected = []
                for a, b in cases:
                    if function == "mix":
                        expected.append((((a + b) ^ (b << 3)) & mask, a))
                    elif function == "product":
                        expected.append(((a * b) & mask, a))
                    elif function == "call_mix":
                        left, right = b + 1, a ^ 7
                        expected.append(((((left + right) ^ (right << 3)) + b) & mask, a))
                    elif function == "branch":
                        expected.append(((a + b if a < b else a - b) & mask, a))
                    elif function == "access_word":
                        expected.append(((a + b) & mask, b))
                    else:
                        expected.append((a, (a + b) & mask))
                if execute(original, profile, function, cases) != expected:
                    raise RuntimeError(f"{profile} {function}: original disagrees with input C")
                for projection in ("highc", "llvmc", "llvm"):
                    stem = work / f"{profile}-{function}-{projection}"
                    source = stem.with_suffix(".c")
                    if projection != "llvm":
                        options = ["--llvm"] if projection == "llvmc" else []
                        subprocess.run([args.neverd, "decompile", "--func", function, *options,
                                        "-o", source, original], check=True)
                        # A void result can accidentally leave the expected bits in a0 or r3.
                        # Check the emitted declaration as well as the register the caller sees.
                        checked = work / f"{stem.name}-checked.c"
                        checked.write_text(f'#include "{source.name}"\n'
                                           f'_Static_assert(sizeof({function}(0, 0)) == '
                                           'sizeof(unsigned long), "integer result width");\n')
                    for optimization in ("-O0", "-O2"):
                        rebuilt = module_binaries[optimization]
                        if projection != "llvm":
                            rebuilt = stem.with_suffix(".elf")
                            callees = [work / f"{profile}-mix-{projection}.c"] if function == "call_mix" else []
                            compile_elf(args, work, profile, checked, rebuilt, function, optimization, callees)
                        actual = execute(rebuilt, profile, function, cases)
                        if actual != expected:
                            index = next(i for i, pair in enumerate(zip(actual, expected)) if pair[0] != pair[1])
                            raise RuntimeError(f"{stem.name} {optimization} {cases[index]}: "
                                               f"{actual[index]} != {expected[index]}")
                        print(f"{stem.name} {optimization}: {len(cases)} register/memory comparisons", flush=True)


if __name__ == "__main__":
    main()
