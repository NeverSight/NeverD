#!/usr/bin/env python3
"""Run real Zicntr and AltiVec ELF callees against a native caller."""

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


COUNTERS = ("rdcycle", "rdtime", "rdinstret")
VMX_TYPES = {"vaddubm": ("unsigned char", 1), "vadduhm": ("unsigned short", 2),
             "vadduwm": ("unsigned int", 4)}


def reference_words(clang, work, profile):
    # Assemble an independent spelling of each instruction with zero register
    # fields. The oracle can then recognize any destination in the rebuilt ELF.
    if profile.startswith("riscv"):
        target = "riscv32"  # also assemble the RV32-only high halves
        flags = ["-march=rv32i_zicsr_zicntr"]
        names = [name + suffix for name in COUNTERS for suffix in ("", "h")]
        operands = "zero"
    else:
        target, flags, names, operands = profile, ["-maltivec"], VMX_TYPES, "0,0,0"
    source, obj = work / "reference.S", work / "reference.o"
    source.write_text(".text\n" + "".join(f"{name} {operands}\n" for name in names))
    subprocess.run([clang, *compiler_flags(target), *flags, "-c", source, "-o", obj], check=True)
    with obj.open("rb") as stream:
        elf = ELFFile(stream)
        data = elf.get_section_by_name(".text").data()
        order = "little" if elf.little_endian else "big"
    if len(data) != 4 * len(names):
        raise RuntimeError("unexpected length for reference instruction words")
    return {name: int.from_bytes(data[i * 4:i * 4 + 4], order) for i, name in enumerate(names)}


def vector_cases(lane_bytes, order, mixed):
    if lane_bytes == 1:
        # 257 permutes all 65,536 input pairs across the sixteen lanes.
        for block in range(0, 1 << 16, 16):
            pairs = [((block + lane) * 257) & 0xffff for lane in range(16)]
            left, right = bytes(p >> 8 for p in pairs), bytes(p & 255 for p in pairs)
            yield left + right, bytes((x + y) & 255 for x, y in zip(left, right))
        return

    bits = 8 * lane_bytes
    mask = (1 << bits) - 1
    edges = sorted({((1 << bit) + delta) & mask for bit in range(bits) for delta in (-1, 0, 1)}
                   | {mask, 0x55aa, 0xaa55})
    pairs = [(edges[(i + j) % len(edges)], y) for i in range(len(edges)) for j, y in enumerate(edges)]
    lanes = 16 // lane_bytes
    def pack(values):
        return b"".join(v.to_bytes(lane_bytes, order) for v in values)

    for block in range(0, len(pairs), lanes):
        # Vary both inputs across adjacent lanes, including the last partial batch.
        batch = [pairs[(block + i) % len(pairs)] for i in range(lanes)]
        left = pack(x for x, _ in batch)
        right = pack(y for _, y in batch)
        total = pack((x + y) & mask for x, y in batch)
        if mixed:
            # The intermediate is eight halfwords. Reinterpret its bytes as
            # four words before the second add, using the guest's memory order.
            third = bytes((block + 19 * i + 1) & 255 for i in range(16))
            total = b"".join(((int.from_bytes(total[i:i+4], order) + int.from_bytes(third[i:i+4], order))
                               & 0xffffffff).to_bytes(4, order) for i in range(0, 16, 4))
            yield left + right + third, total
        else:
            yield left + right, total


def check_vector(binary, profile, lane_bytes, mixed):
    cpu, symbols = machine(binary, profile, power8=True)
    # The 64-bit POWER8 also implements the 32-bit AltiVec ABI with MSR.SF clear.
    msr = cpu.reg_read(ppc_const.UC_PPC_REG_MSR)
    cpu.reg_write(ppc_const.UC_PPC_REG_MSR, msr | (1 << 25))
    start = symbols["check"]
    with binary.open("rb") as stream:
        elf = ELFFile(stream)
        order = "little" if elf.little_endian else "big"
        opd = elf.get_section_by_name(".opd")
        if opd and opd["sh_addr"] <= start < opd["sh_addr"] + opd["sh_size"]:
            toc = int.from_bytes(cpu.mem_read(start + 8, 8), order)
            start = int.from_bytes(cpu.mem_read(start, 8), order)
            cpu.reg_write(ppc_const.UC_PPC_REG_2, toc)
    for block, (inputs, expect) in enumerate(vector_cases(lane_bytes, order, mixed)):
        output = DATA + len(inputs)
        cpu.mem_write(DATA, inputs + bytes(x ^ 255 for x in expect))
        cpu.reg_write(ppc_const.UC_PPC_REG_1, STACK + 2048)
        cpu.reg_write(ppc_const.UC_PPC_REG_3, DATA)
        cpu.reg_write(ppc_const.UC_PPC_REG_4, output)
        cpu.reg_write(ppc_const.UC_PPC_REG_12, start)
        cpu.reg_write(ppc_const.UC_PPC_REG_LR, RETURN)
        cpu.emu_start(start, RETURN, count=1000)
        if cpu.reg_read(ppc_const.UC_PPC_REG_PC) != RETURN or cpu.mem_read(output, 16) != expect:
            raise RuntimeError(f"{profile} {binary.name}: wrong vector result for pair block {block}")


def check_counter(binary, profile, name, words, operation="wide"):
    # Supply one clock observation per executed CSR read. For RV32 the first
    # three span a rollover, so Clang's high/low/high loop must sample again.
    cpu, symbols = machine(binary, profile)
    wide = profile == "riscv64"
    samples = ([0x123456789abcdef0] if wide else
               [0x12fffffffe, 0x12ffffffff, 0x1300000000,
                0x1300000001, 0x1300000002, 0x1300000003])
    high = name + "h"
    expected_reads = [name] if wide else [high, name, high] * 2
    expected = samples[0] if wide else samples[4]
    if operation == "discard":
        samples, expected_reads, expected = [0x123456789abcdef0], [name], None
    elif operation in ("elapsed", "mixed"):
        samples = [0x12fffffffe, 0x1300000003]
        expected_reads = [name, "rdinstret" if operation == "mixed" else name]
        expected = samples[1] - samples[0]
    elif operation == "high":
        samples, expected_reads = [0x89abcdef01234567], [high]
        expected = samples[0] >> 32
    names = {word: mnemonic for mnemonic, word in words.items()}
    observed = []

    def counter_read(cpu, addr, size, _):
        if size != 4:
            return
        word = int.from_bytes(cpu.mem_read(addr, size), "little")
        mnemonic = names.get(word & ~(31 << 7))
        if mnemonic is None:
            return
        if len(observed) >= len(samples) or mnemonic != expected_reads[len(observed)]:
            raise RuntimeError(f"{profile} {binary.name}: unexpected {mnemonic} after {observed}")
        value = samples[len(observed)]
        observed.append(mnemonic)
        rd = (word >> 7) & 31
        if rd:
            cpu.reg_write(riscv_const.UC_RISCV_REG_X0 + rd, value >> 32 if mnemonic.endswith("h") else value)
        cpu.reg_write(riscv_const.UC_RISCV_REG_PC, addr + size)

    cpu.hook_add(uc.UC_HOOK_CODE, counter_read)
    cpu.reg_write(riscv_const.UC_RISCV_REG_RA, RETURN)
    cpu.reg_write(riscv_const.UC_RISCV_REG_SP, STACK + 2048)
    cpu.emu_start(symbols["probe"], RETURN, count=1000)
    value = cpu.reg_read(riscv_const.UC_RISCV_REG_A0)
    if not wide and operation == "wide":
        value |= cpu.reg_read(riscv_const.UC_RISCV_REG_A1) << 32
    if (cpu.reg_read(riscv_const.UC_RISCV_REG_PC) != RETURN or observed != expected_reads or
            (expected is not None and value != expected)):
        raise RuntimeError(f"{profile} {binary.name}: counter {value:#x}, observations {observed}")


def project(args, work, original, route):
    source = work / ("probe.ll" if route == "llvm" else "probe.c")
    cli = ["lift"] if route == "llvm" else ["decompile", "--func", "probe"]
    if route == "llvmc":
        cli.append("--llvm")
    subprocess.run([args.neverd, *cli, original, "-o", source], check=True)
    return source


def native_counter_reader(name, rv32):
    if name == "rdcycle":
        return "static unsigned long long sample(void) { return __builtin_readcyclecounter(); }\n"
    def read(mnemonic, result):
        return f'__asm__ volatile ("{mnemonic} %0" : "=r"({result}));'

    if not rv32:
        return f"static unsigned long long sample(void) {{ unsigned long low; {read(name, 'low')} return low; }}\n"
    return f"""static unsigned long long sample(void) {{
  unsigned high, low, again;
  do {{
    {read(name + 'h', 'high')}
    {read(name, 'low')}
    {read(name + 'h', 'again')}
  }} while (high != again);
  return ((unsigned long long)high << 32) | low;
}}
"""



def counter_leaf(path, body):
    path.write_text(".text\n.globl probe\n.type probe,@function\nprobe:\n" +
                    body + "\nret\n.size probe,.-probe\n")


def check_profile(args, work, profile, name, words, elfv1=False, unused_vector=False, double_abi=False):
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
    target = f"{profile} {name}" + (" ELFv1" if elfv1 else "")
    if double_abi:
        target += " ILP32D" if profile == "riscv32" else " LP64D"
    if unused_vector:
        target += " unused v2"
    leaf = work / "leaf.c"
    caller = work / "caller.c"
    obj = work / "caller.o"
    if rv:
        if name == "rdcycle":
            leaf.write_text("unsigned long long probe(void) { return __builtin_readcyclecounter(); }\n")
        else:
            leaf = work / "leaf.S"
            body = (f"1: {name}h a1\n{name} a0\n{name}h a2\nbne a1,a2,1b"
                    if profile == "riscv32" else f"{name} a0")
            counter_leaf(leaf, body)
        # QEMU user mode can report one host tick for several guest reads.
        # Check progress over the complete run and bound every individual read.
        start = "  unsigned long long first = sample();\n" if name != "rdtime" else ""
        progress = "  ok &= sample() != first;\n" if start else ""
        caller.write_text(
            native_counter_reader(name, profile == "riscv32") + "extern unsigned long long probe(void);\n"
            "void check(void) {\n"
            f"  unsigned ok = 1;\n{start}"
            "  for (unsigned i = 0; i < 64; ++i) {\n"
            "    unsigned long long before = sample();\n"
            "    unsigned long long value = probe();\n"
            "    unsigned long long after = sample();\n"
            "    ok &= value - before <= after - before;\n"
            "  }\n"
            f"{progress}  register unsigned long status __asm__(\"a0\") = !ok;\n"
            "  register unsigned long exit_nr __asm__(\"a7\") = 93;\n"
            "  __asm__ volatile (\"ecall\" : : \"r\"(status), \"r\"(exit_nr) : \"memory\");\n"
            "  __builtin_unreachable();\n}\n")
    else:
        mixed = name == "mixed"
        scalar, lane_bytes = VMX_TYPES["vadduhm" if mixed else name]
        types = f"typedef {scalar} Vec __attribute__((vector_size(16)));\n"
        params, values, expression = "Vec a, Vec b", "in[0], in[1]", "a + b"
        if unused_vector:
            params, values = "Vec unused, " + params, "(Vec){0}, " + values
        if mixed:
            types += "typedef unsigned int Words __attribute__((vector_size(16)));\n"
            params, values = params + ", Words c", values + ", (Words)in[2]"
            expression = "(Vec)((Words)(a + b) + c)"
        leaf.write_text(types + f"Vec probe({params}) {{ return {expression}; }}\n")
        caller.write_text(types + f"extern Vec probe({params});\n"
                          f"void check(const Vec *in, Vec *out) {{ *out = probe({values}); }}\n")
    subprocess.run([args.clang, *compiler_flags(profile), *flags, "-O2", "-c", caller, "-o", obj],
                   check=True)

    def execute(binary):
        if rv:
            check_counter(binary, profile, name, words)
            qemu = "qemu-" + profile
            subprocess.run([args.qemu_dir / qemu if args.qemu_dir else qemu, binary],
                           check=True, timeout=30)
        else:
            check_vector(binary, profile, lane_bytes, mixed)

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
            encodings = {int.from_bytes(text[i:i+4], order) & 0xfc0007ff for i in range(0, len(text), 4)}
            for mnemonic in (("vadduhm", "vadduwm") if mixed else (name,)):
                if words[mnemonic] not in encodings:
                    raise RuntimeError(f"{target}: compiler fixture contains no {mnemonic}")
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
        operations = {"discard": f"{name} zero", "elapsed": f"{name} t0\n{name} a0\nsub a0,a0,t0"}
        if profile == "riscv32":
            operations["high"] = f"{name}h a0"
        if name == "rdtime":
            operations["mixed"] = "rdtime t0\nrdinstret a0\nsub a0,a0,t0"
        for operation, body in operations.items():
            leaf = work / (operation + ".S")
            counter_leaf(leaf, body)
            compile_elf(args, work, profile, leaf, original, "probe", extra_flags=flags)
            check_counter(original, profile, name, words, operation)
            for route in ("highc", "llvmc", "llvm"):
                output = project(args, work, original, route)
                binary = work / f"{operation}-{route}.elf"
                compile_elf(args, work, profile, output, binary, "probe", extra_flags=flags)
                check_counter(binary, profile, name, words, operation)
                print(f"{target} {route} {operation} executed", flush=True)


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
            words = reference_words(args.clang, work, profile)
            if profile.startswith("riscv"):
                for name in COUNTERS:
                    for double_abi in (False, True):
                        check_profile(args, work, profile, name, words, double_abi=double_abi)
            else:
                abis = (False, True) if args.elfv1_ld and profile.startswith("powerpc64") else (False,)
                for elfv1 in abis:
                    for name in (*VMX_TYPES, "mixed"):
                        check_profile(args, work, profile, name, words, elfv1)
                    check_profile(args, work, profile, "vaddubm", words, elfv1, unused_vector=True)


if __name__ == "__main__":
    main()
