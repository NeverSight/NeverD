#!/usr/bin/env python3
"""Run real cycle-counter and AltiVec ELF callees against a native caller."""

import argparse
import copy
from pathlib import Path
import subprocess
import tempfile

from elftools.elf.constants import E_FLAGS
from elftools.elf.elffile import ELFFile
import unicorn as uc
from unicorn import ppc_const, riscv_const

from roundtrip import DATA, PROFILES, RETURN, STACK, compile_elf, compiler_flags, machine


def check_vector(binary, profile):
    cpu, symbols = machine(binary, profile, power8=True)
    # The 64-bit POWER8 also implements the 32-bit AltiVec ABI with MSR.SF clear.
    msr = cpu.reg_read(ppc_const.UC_PPC_REG_MSR)
    cpu.reg_write(ppc_const.UC_PPC_REG_MSR, msr | (1 << 25))
    start = symbols["check"]
    with binary.open("rb") as stream:
        elf = ELFFile(stream)
        opd = elf.get_section_by_name(".opd")
        if opd and opd["sh_addr"] <= start < opd["sh_addr"] + opd["sh_size"]:
            order = "little" if elf.little_endian else "big"
            toc = int.from_bytes(cpu.mem_read(start + 8, 8), order)
            start = int.from_bytes(cpu.mem_read(start, 8), order)
            cpu.reg_write(ppc_const.UC_PPC_REG_2, toc)
    # 257 permutes the 16-bit pair space. Adjacent lanes have distinct values
    # and exercise both wrapped and nonwrapped sums.
    for block in range(0, 1 << 16, 16):
        pairs = [((block + lane) * 257) & 0xffff for lane in range(16)]
        left = bytes(pair >> 8 for pair in pairs)
        right = bytes(pair & 255 for pair in pairs)
        expect = bytes((x + y) & 255 for x, y in zip(left, right))
        cpu.mem_write(DATA, left + right + bytes(x ^ 255 for x in expect))
        cpu.reg_write(ppc_const.UC_PPC_REG_1, STACK + 2048)
        cpu.reg_write(ppc_const.UC_PPC_REG_3, DATA)
        cpu.reg_write(ppc_const.UC_PPC_REG_4, DATA + 32)
        cpu.reg_write(ppc_const.UC_PPC_REG_12, start)
        cpu.reg_write(ppc_const.UC_PPC_REG_LR, RETURN)
        cpu.emu_start(start, RETURN, count=1000)
        if cpu.reg_read(ppc_const.UC_PPC_REG_PC) != RETURN or cpu.mem_read(DATA + 32, 16) != expect:
            raise RuntimeError(f"{profile} {binary.name}: wrong vector result for pair block {block}")


def check_counter(binary, profile, operation="wide"):
    # Supply one clock observation per executed CSR read. For RV32 the first
    # three span a rollover, so Clang's high/low/high loop must sample again.
    cpu, symbols = machine(binary, profile)
    wide = profile == "riscv64"
    samples = ([0x123456789abcdef0] if wide else
               [0x12fffffffe, 0x12ffffffff, 0x1300000000,
                0x1300000001, 0x1300000002, 0x1300000003])
    expected_csrs = [0xc00] if wide else [0xc80, 0xc00, 0xc80] * 2
    expected = samples[0] if wide else samples[4]
    if operation == "discard":
        samples, expected_csrs, expected = [0x123456789abcdef0], [0xc00], None
    elif operation == "elapsed":
        samples, expected_csrs = [0x12fffffffe, 0x1300000003], [0xc00, 0xc00]
        expected = samples[1] - samples[0]
    elif operation == "high":
        samples, expected_csrs = [0x89abcdef01234567], [0xc80]
        expected = samples[0] >> 32
    observed = []

    def counter_read(cpu, addr, size, _):
        if size != 4:
            return
        word = int.from_bytes(cpu.mem_read(addr, size), "little")
        if word & 0xfffff07f not in (0xc0002073, 0xc8002073):
            return
        csr = word >> 20
        if len(observed) >= len(samples) or csr != expected_csrs[len(observed)]:
            raise RuntimeError(f"{profile} {binary.name}: unexpected CSR read {csr:#x} after {observed}")
        value = samples[len(observed)]
        observed.append(csr)
        rd = (word >> 7) & 31
        if rd:
            cpu.reg_write(riscv_const.UC_RISCV_REG_X0 + rd, value >> 32 if csr == 0xc80 else value)
        cpu.reg_write(riscv_const.UC_RISCV_REG_PC, addr + size)

    cpu.hook_add(uc.UC_HOOK_CODE, counter_read)
    cpu.reg_write(riscv_const.UC_RISCV_REG_RA, RETURN)
    cpu.reg_write(riscv_const.UC_RISCV_REG_SP, STACK + 2048)
    cpu.emu_start(symbols["probe"], RETURN, count=1000)
    value = cpu.reg_read(riscv_const.UC_RISCV_REG_A0)
    if not wide and operation == "wide":
        value |= cpu.reg_read(riscv_const.UC_RISCV_REG_A1) << 32
    if (cpu.reg_read(riscv_const.UC_RISCV_REG_PC) != RETURN or observed != expected_csrs or
            (expected is not None and value != expected)):
        raise RuntimeError(f"{profile} {binary.name}: counter {value:#x}, observations {observed}")


def project(args, work, original, route):
    source = work / ("probe.ll" if route == "llvm" else "probe.c")
    cli = ["lift"] if route == "llvm" else ["decompile", "--func", "probe"]
    if route == "llvmc":
        cli.append("--llvm")
    subprocess.run([args.neverd, *cli, original, "-o", source], check=True)
    return source


def check_profile(args, work, profile, elfv1=False, unused_vector=False, double_abi=False):
    rv = profile.startswith("riscv")
    if rv:
        extensions = "ifd" if double_abi else "i"
        flags = [f"-march=rv{profile[-2:]}{extensions}_zicsr_zicntr"]
        if double_abi:
            flags.append("-mabi=ilp32d" if profile == "riscv32" else "-mabi=lp64d")
    else:
        flags = ["-mcpu=power8", "-maltivec", "-mno-vsx"]
    if elfv1:
        args = copy.copy(args)
        args.ld = args.elfv1_ld
        flags.append("-mabi=elfv1")
    target = profile + (" ELFv1" if elfv1 else "")
    if double_abi:
        target += " ILP32D" if profile == "riscv32" else " LP64D"
    if unused_vector:
        target += " unused v2"
    types = "typedef unsigned char Bytes __attribute__((vector_size(16)));\n"
    leaf = work / "leaf.c"
    caller = work / "caller.c"
    obj = work / "caller.o"
    if rv:
        leaf.write_text("unsigned long long probe(void) { return __builtin_readcyclecounter(); }\n")
        caller.write_text(
            "extern unsigned long long probe(void);\n"
            "void check(void) {\n"
            "  unsigned ok = 1;\n"
            "  for (unsigned i = 0; i < 64; ++i) {\n"
            "    unsigned long long before = __builtin_readcyclecounter();\n"
            "    unsigned long long value = probe();\n"
            "    unsigned long long after = __builtin_readcyclecounter();\n"
            "    ok &= (value - before <= after - before) && (after != before);\n"
            "  }\n"
            "  register unsigned long status __asm__(\"a0\") = !ok;\n"
            "  register unsigned long exit_nr __asm__(\"a7\") = 93;\n"
            "  __asm__ volatile (\"ecall\" : : \"r\"(status), \"r\"(exit_nr) : \"memory\");\n"
            "  __builtin_unreachable();\n}\n")
    else:
        params = "Bytes unused, Bytes a, Bytes b" if unused_vector else "Bytes a, Bytes b"
        values = "(Bytes){0}, in[0], in[1]" if unused_vector else "in[0], in[1]"
        leaf.write_text(types + f"Bytes probe({params}) {{ return a + b; }}\n")
        caller.write_text(types + f"extern Bytes probe({params});\n"
                          f"void check(const Bytes *in, Bytes *out) {{ *out = probe({values}); }}\n")
    subprocess.run([args.clang, *compiler_flags(profile), *flags, "-O2", "-c", caller, "-o", obj],
                   check=True)

    def execute(binary):
        if rv:
            check_counter(binary, profile)
            qemu = "qemu-" + profile
            subprocess.run([args.qemu_dir / qemu if args.qemu_dir else qemu, binary],
                           check=True, timeout=30)
        else:
            check_vector(binary, profile)

    original = work / "original.elf"
    linked = work / "linked.elf"
    compile_elf(args, work, profile, leaf, original, "probe", extra_flags=flags)
    compile_elf(args, work, profile, leaf, linked, "check", callees=[obj], extra_flags=flags)
    with original.open("rb") as stream:
        elf = ELFFile(stream)
        if rv:
            expected = E_FLAGS.EF_RISCV_FLOAT_ABI_DOUBLE if double_abi else E_FLAGS.EF_RISCV_FLOAT_ABI_SOFT
            if elf["e_flags"] & E_FLAGS.EF_RISCV_FLOAT_ABI != expected:
                raise RuntimeError(f"{target}: compiler fixture has the wrong float ABI")
        else:
            text = elf.get_section_by_name(".text").data()
            order = "little" if elf.little_endian else "big"
            if not any(int.from_bytes(text[i:i+4], order) & 0xfc0007ff == 0x10000000
                       for i in range(0, len(text), 4)):
                raise RuntimeError(f"{target}: compiler fixture contains no vaddubm")
    execute(linked)
    print(f"{target} original executed", flush=True)
    for route in ("highc", "llvmc", "llvm"):
        output = project(args, work, original, route)
        for optimization in ("-O0", "-O2"):
            rebuilt = work / f"{route}{optimization}.elf"
            compile_elf(args, work, profile, output, rebuilt, "check", optimization, [obj], flags)
            execute(rebuilt)
            print(f"{target} {route} {optimization} executed", flush=True)

    if rv:
        # A discarded read still samples. Two reads in one subtraction must
        # keep their execution order even when the result of each is inlined.
        operations = {"discard": "rdcycle zero", "elapsed": "rdcycle t0\nrdcycle a0\nsub a0,a0,t0"}
        if profile == "riscv32":
            operations["high"] = "rdcycleh a0"
        for name, body in operations.items():
            leaf = work / (name + ".S")
            leaf.write_text(".text\n.globl probe\n.type probe,@function\nprobe:\n" +
                            body + "\nret\n.size probe,.-probe\n")
            compile_elf(args, work, profile, leaf, original, "probe", extra_flags=flags)
            check_counter(original, profile, name)
            for route in ("highc", "llvmc", "llvm"):
                output = project(args, work, original, route)
                binary = work / f"{name}-{route}.elf"
                compile_elf(args, work, profile, output, binary, "probe", extra_flags=flags)
                check_counter(binary, profile, name)
                print(f"{target} {route} {name} executed", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for tool in ("neverd", "clang", "ld"):
        parser.add_argument("--" + tool, required=True, type=Path)
    parser.add_argument("--qemu-dir", type=Path, help="directory containing RISC-V QEMU user-mode binaries")
    parser.add_argument("--elfv1-ld", type=Path, help="also exercise PPC64 ELFv1 with this GNU linker")
    parser.add_argument("--profile", choices=PROFILES, action="append")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="neverd-cycle-vmx-") as directory:
        work = Path(directory)
        (work / "string.h").write_text("#include <stddef.h>\nvoid *memcpy(void *, const void *, size_t);\n")
        for profile in args.profile or PROFILES:
            abis = (False, True) if args.elfv1_ld and profile.startswith("powerpc64") else (False,)
            for elfv1 in abis:
                check_profile(args, work, profile, elfv1)
                if profile.startswith("riscv"):
                    check_profile(args, work, profile, double_abi=True)
                else:
                    check_profile(args, work, profile, elfv1, unused_vector=True)


if __name__ == "__main__":
    main()
