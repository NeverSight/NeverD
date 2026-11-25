#!/usr/bin/env python3
"""Compile and execute the C and LLVM projections of linked integer ELF functions."""

import argparse
from ctypes import c_int32
import itertools
from pathlib import Path
import subprocess
import tempfile

from elftools.elf.elffile import ELFFile
import unicorn as uc
from unicorn import ppc_const, riscv_const

from image_storage import llvmc_image_storage

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


def compile_elf(args, work, profile, source, output, entry, optimization="-O2", callees=(),
                extra_flags=()):
    dialect = ["-std=gnu17", "-I", work] if source.suffix == ".c" else []
    subprocess.run([args.clang, *compiler_flags(profile), *extra_flags, optimization, *dialect, "-nostdlib",
                    "-static", f"-fuse-ld={args.ld}", f"-Wl,-e,{entry}",
                    source, *callees, "-o", output], check=True)


def without_sections(app):
    with app.open("rb") as stream:
        elf = ELFFile(stream)
        for field in ("e_shoff", "e_shnum", "e_shstrndx"):
            elf.header[field] = 0
        header = elf.structs.Elf_Ehdr.build(elf.header)
    raw = bytearray(app.read_bytes())
    raw[:len(header)] = header
    result = app.with_name(app.stem + "-no-section-headers.so")
    result.write_bytes(raw)
    return result


def map_image(cpu, elf, bias=0):
    segments = [s for s in elf.iter_segments() if s["p_type"] == "PT_LOAD" and s["p_memsz"]]
    # LOAD segments may share an aligned page.
    pages = {p for s in segments for p in range(s["p_vaddr"] & ~4095,
             s["p_vaddr"] + s["p_memsz"], 4096)}
    for page in sorted(pages):
        cpu.mem_map(bias + page, 4096)
    for s in segments:
        if s["p_filesz"]:
            cpu.mem_write(bias + s["p_vaddr"], s.data())


def machine(path, profile, bias=0, power8=False):
    wide = "64" in profile
    rv = profile.startswith("riscv")
    if rv:
        cpu = uc.Uc(uc.UC_ARCH_RISCV, uc.UC_MODE_RISCV64 if wide else uc.UC_MODE_RISCV32)
    else:
        # The POWER8 model also runs 32-bit user code with MSR.SF clear.
        mode = uc.UC_MODE_PPC64 if wide or power8 else uc.UC_MODE_PPC32
        cpu = uc.Uc(uc.UC_ARCH_PPC, uc.UC_MODE_BIG_ENDIAN | mode)
        if wide or power8:
            cpu.ctl_set_tlb_mode(uc.UC_TLB_VIRTUAL)
        # POWER8 can use FP, VMX and VSX to optimize a recompiled scalar C expression.
        facilities = (1 << 13) | (1 << 25) | (1 << 23) if wide else 0
        cpu.reg_write(ppc_const.UC_PPC_REG_MSR,
                      (int(wide) << 63) | facilities | int(profile.endswith("le")))
    with path.open("rb") as file:
        elf = ELFFile(file)
        map_image(cpu, elf, bias)
        symtab = elf.get_section_by_name(".symtab") or elf.get_section_by_name(".dynsym")
        symbols = {s.name: bias + s["st_value"] for s in symtab.iter_symbols()
                   if s["st_info"]["type"] == "STT_FUNC"}
    for page in (DATA, STACK, RETURN):
        cpu.mem_map(page, 4096)
    return cpu, symbols


def execute(path, profile, function, cases, symbol=None):
    cpu, symbols = machine(path, profile, power8=function == "population_counts")
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
    symbol = symbol or function
    if symbol not in symbols:
        raise RuntimeError(f"{profile}: {path.name} has no definition of {symbol}")
    start = symbols[symbol]
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
        result = cpu.reg_read(first) & ((1 << (size * 8)) - 1)
        results.append((result, int.from_bytes(cpu.mem_read(DATA + size, size), order)))
    return results


def count_word_divides(path, function):
    with path.open("rb") as file:
        elf = ELFFile(file)
        symbol = elf.get_section_by_name(".symtab").get_symbol_by_name(function)[0]
        section = elf.get_section(symbol["st_shndx"])
        start = symbol["st_value"] - section["sh_addr"]
        code = section.data()[start:start + symbol["st_size"]]
        order = "little" if elf.little_endian else "big"
        # divw without OE or Rc. Ignore only the three register selectors.
        return sum((int.from_bytes(code[i:i + 4], order) & 0xfc0007ff) == 0x7c0003d6
                   for i in range(0, len(code), 4))


def check_double_word_sum(args, work, profile):
    # Keep the caller's native ABI in a separate object across all rebuilds.
    # The two source routes select just the leaf. The LLVM module has no caller.
    leaf = work / "sum_words.c"
    leaf.write_text("unsigned long long sum_words(unsigned long long a, "
                    "unsigned long long b) { return a + b; }\n")
    inputs = [(0, 0), (0xffffffff, 1), (0x0123456789abcdef, 0x76543210fedcba98),
              (0xffffffffffffffff, 2), (0x100000002, 0x300000004)]
    rows = ",\n".join(f"  {{ {a:#x}ULL, {b:#x}ULL, {(a + b) % (1 << 64):#x}ULL }}"
                      for a, b in inputs)
    caller = work / "check_sum.c"
    caller.write_text("typedef unsigned long long U64;\nextern U64 sum_words(U64, U64);\n"
                      f"static const U64 cases[][3] = {{\n{rows}\n}};\n"
                      "unsigned check_sum(unsigned n) { const U64 *p = cases[n];\n"
                      "  return sum_words(p[0], p[1]) == p[2]; }\n")
    obj = work / "check_sum.o"
    subprocess.run([args.clang, *compiler_flags(profile), "-O2", "-c", caller, "-o", obj],
                   check=True)
    cases = [(i, 0) for i in range(len(inputs))]

    def verify(binary):
        if any(value != 1 for value, _ in execute(binary, profile, "check_sum", cases)):
            raise RuntimeError(f"{profile} {binary.name}: incorrect 64-bit sum at caller")

    original = work / f"{profile}-sum.elf"
    linked = work / f"{profile}-check.elf"
    for original_opt in ("-O0", "-O2"):
        compile_elf(args, work, profile, leaf, original, "sum_words", original_opt)
        compile_elf(args, work, profile, leaf, linked, "check_sum", original_opt, [obj])
        verify(linked)
        for route in ("highc", "llvmc", "llvm"):
            source = work / ("sum.ll" if route == "llvm" else "sum.c")
            if route == "llvm":
                command = ["lift", "-o", source, original]
            else:
                flags = ["--llvm"] if route == "llvmc" else []
                command = ["decompile", "--func", "sum_words", *flags, "-o", source, linked]
            subprocess.run([args.neverd, *command], check=True)
            for opt in ("-O0", "-O2"):
                binary = work / f"{profile}-sum{original_opt}-{route}{opt}.elf"
                compile_elf(args, work, profile, source, binary, "check_sum", opt, [obj])
                verify(binary)
                print(f"{binary.name}: {len(cases)} 64-bit sums", flush=True)


def check_riscv_indexed_read(args, work, profile):
    fixture = Path(__file__).with_name("riscv-indexed-read.S")
    original = work / f"{profile}-table.elf"
    name = "indexed_read"
    compile_elf(args, work, profile, fixture, original, name)
    with original.open("rb") as stream:
        elf = ELFFile(stream)
        entry = elf["e_entry"]
        table = elf.get_section_by_name(".rodata")["sh_addr"]
    cases = [(i, 0) for i in range(64)]
    # The load follows the relocated table. The XOR of the very same
    # machine address as an integer still observes its original bits.
    expected = [((32 - (i & 31)) ^ (table + (i & 31)), i) for i, _ in cases]
    if execute(original, profile, name, cases) != expected:
        raise RuntimeError(f"{profile}: incorrect original indexed read")
    for image, symbol in ((original, name), (without_sections(original), f"sub_{entry:X}")):
        for route in ("highc", "llvmc", "llvm"):
            source = work / ("table.ll" if route == "llvm" else "table.c")
            if route == "llvm":
                command = ["lift"]
            else:
                command = ["decompile", "--func", hex(entry)]
                if route == "llvmc":
                    command.append("--llvm")
            subprocess.run([args.neverd, *command, image, "-o", source], check=True)
            storage = llvmc_image_storage(source, original, ".rodata") if route == "llvmc" else []
            for opt, base in (("-O0", "0x210000"), ("-O2", "0x610000")):
                binary = work / "moved-table.elf"
                compile_elf(args, work, profile, source, binary, symbol, opt, storage,
                            [f"-Wl,--image-base={base}", "-fno-strict-aliasing"])
                with binary.open("rb") as stream:
                    if any(s["p_type"] == "PT_LOAD" and
                           s["p_vaddr"] <= table < s["p_vaddr"] + s["p_memsz"]
                           for s in ELFFile(stream).iter_segments()):
                        raise RuntimeError(f"{profile}: old table address is still mapped")
                if execute(binary, profile, name, cases, symbol) != expected:
                    raise RuntimeError(f"{profile} {image.name} {route} {opt}: incorrect relocated read")
                print(f"{image.name} {route} {opt}: {len(cases)} relocated reads", flush=True)


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
            check_double_word_sum(args, work, profile)
            if profile.startswith("riscv"):
                check_riscv_indexed_read(args, work, profile)
            original = work / f"{profile}.elf"
            # Exercise the linked AUIPC/JALR call pair with its original displacement.
            original_flags = ["-mno-relax", "-Wl,--no-relax"] if profile.startswith("riscv") else []
            compile_elf(args, work, profile, fixture, original, "mix", extra_flags=original_flags)
            bits = 64 if "64" in profile else 32
            mask = (1 << bits) - 1
            values = [0, 1, 1 << (bits - 1), mask, 0x87654321]
            all_cases = list(itertools.product(values, repeat=2))
            module = work / f"{profile}.ll"
            subprocess.run([args.neverd, "lift", "-o", module, original], check=True)
            module_binaries = {}
            for optimization in ("-O0", "-O2"):
                binary = work / f"{profile}-llvm{optimization}.elf"
                compile_elf(args, work, profile, module, binary, "mix", optimization)
                module_binaries[optimization] = binary
            functions = ["mix", "product", "unsigned_quotient", "signed_quotient",
                         "branch", "access_word", "atomic_word", "call_mix"]
            if profile.startswith("powerpc"):
                functions.extend(("independent_divides", "recorded_divide", "divide_flags",
                                  "conditional_return", "population_counts", "recorded_word_product"))
            for function in functions:
                cases = all_cases
                if function in ("unsigned_quotient", "signed_quotient"):
                    cases = [(a, b) for a, b in cases if b and
                             (function != "signed_quotient" or (a, b) != (1 << (bits - 1), mask))]
                if function == "divide_flags" and bits == 64:
                    cases = [*cases, (1 << 31, mask)]
                if function == "recorded_word_product":
                    cases = [*cases, (0x10000, 0x10000), (0x80000000, 0xffffffff),
                             (0x12345678ffffffff & mask, 0xfedcba9800000002 & mask)]
                expected = []
                for a, b in cases:
                    if function == "mix":
                        expected.append((((a + b) ^ (b << 3)) & mask, a))
                    elif function == "product":
                        expected.append(((a * b) & mask, a))
                    elif function == "unsigned_quotient":
                        expected.append((a // b, a))
                    elif function == "signed_quotient":
                        left = a - (1 << bits) if a >> (bits - 1) else a
                        right = b - (1 << bits) if b >> (bits - 1) else b
                        value = abs(left) // abs(right)
                        expected.append(((-value if (left < 0) != (right < 0) else value) & mask, a))
                    elif function == "call_mix":
                        left, right = b + 1, a ^ 7
                        expected.append(((((left + right) ^ (right << 3)) + b) & mask, a))
                    elif function == "conditional_return":
                        count = (b - 1) & mask
                        taken = (a & 0xffffffff) == 0 and count != 0
                        expected.append((a if taken else (a + count) & mask, a))
                    elif function == "recorded_word_product":
                        product = (c_int32(a).value * c_int32(b).value) & mask
                        flags = 4 if product >> (bits - 1) else 2 if product else 1
                        expected.append((product ^ flags, a))
                    elif function == "population_counts":
                        words = sum(((a >> n) & 0xffffffff).bit_count() << n
                                    for n in range(0, bits, 32))
                        octets = sum(((b >> n) & 0xff).bit_count() << n
                                     for n in range(0, bits, 8))
                        expected.append(((words + octets + (a ^ b).bit_count()) & mask, a))
                    elif function == "branch":
                        expected.append(((a + b if a < b else a - b) & mask, a))
                    elif function in ("independent_divides", "recorded_divide", "divide_flags"):
                        # The original target supplies the undefined word-division
                        # bits and the CR0 recorded by this processor model.
                        continue
                    elif function == "access_word":
                        expected.append(((a + b) & mask, b))
                    else:
                        expected.append((a, (a + b) & mask))
                observed = execute(original, profile, function, cases)
                if function in ("independent_divides", "recorded_divide", "divide_flags"):
                    expected = observed
                    if function == "independent_divides" and count_word_divides(original, function) != 2:
                        raise RuntimeError(f"{profile}: original must execute both word divides")
                    if function == "divide_flags":
                        for (a, b), (flags, memory) in zip(cases, observed):
                            divisor = b & 0xffffffff
                            overflow = divisor == 0 or (a & 0xffffffff, divisor) == (0x80000000, 0xffffffff)
                            if (flags & 1) != bool((a & 1) or overflow) or memory != a:
                                raise RuntimeError(f"{profile} divwo.: incorrect SO or memory")
                elif observed != expected:
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
                        if function == "independent_divides" and count_word_divides(rebuilt, function) != 2:
                            raise RuntimeError(f"{stem.name} {optimization}: lost an independent word divide")
                        if actual != expected:
                            index = next(i for i, pair in enumerate(zip(actual, expected)) if pair[0] != pair[1])
                            raise RuntimeError(f"{stem.name} {optimization} {cases[index]}: "
                                               f"{actual[index]} != {expected[index]}")
                        print(f"{stem.name} {optimization}: {len(cases)} register/memory comparisons", flush=True)

            if profile.startswith("riscv"):
                with original.open("rb") as file:
                    symtab = ELFFile(file).get_section_by_name(".symtab")
                    caller = symtab.get_symbol_by_name("call_mix")[0]["st_value"]
                    callee = symtab.get_symbol_by_name("mix")[0]["st_value"]
                stripped = work / f"{profile}-stripped.elf"
                compile_elf(args, work, profile, fixture, stripped, "call_mix",
                            extra_flags=[*original_flags, "-Wl,--strip-all"])
                stripped_ir = stripped.with_suffix(".ll")
                subprocess.run([args.neverd, "lift", "-o", stripped_ir, stripped], check=True)
                entry = f"sub_{caller:X}"
                for optimization in ("-O0", "-O2"):
                    rebuilt = work / f"{profile}-stripped{optimization}.elf"
                    compile_elf(args, work, profile, stripped_ir, rebuilt, entry, optimization)
                    for name, address in (("call_mix", caller), ("mix", callee)):
                        expected = execute(original, profile, name, all_cases)
                        actual = execute(rebuilt, profile, name, all_cases, f"sub_{address:X}")
                        if actual != expected:
                            raise RuntimeError(f"{profile} stripped {name} {optimization}: incorrect result")
                        print(f"{profile}-stripped-{name} {optimization}: {len(all_cases)} comparisons", flush=True)


if __name__ == "__main__":
    main()
