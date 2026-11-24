#!/usr/bin/env python3
"""Compare linked ELFv1 functions with their C and LLVM rebuilds in Unicorn."""

import argparse
from pathlib import Path
import re
import subprocess
import tempfile

from elftools.elf.elffile import ELFFile
from elftools.elf.enums import ENUM_RELOC_TYPE_PPC64
from unicorn import ppc_const

from roundtrip import DATA, RETURN, STACK, machine


def compile_elf(clang, linker, target, sources, entry, output, optimize,
                relocated=False, retain_relocations=False):
    dialect = ["-std=gnu17"] if sources[0].suffix == ".c" else []
    placement = ["-Wl,-Ttext-segment=0x18000000"] if relocated else []
    if retain_relocations:
        placement.append("-Wl,--emit-relocs")
    subprocess.run(
        [clang, f"--target={target}-linux-gnu", "-mabi=elfv1", "-mcpu=power8",
         "-ffreestanding", "-fPIC", "-fno-stack-protector", "-nostdlib", "-static",
         optimize, *dialect, f"-fuse-ld={linker}", f"-Wl,-e,{entry}", *placement,
         *sources, "-o", output],
        check=True,
    )


def llvmc_image_storage(source, original):
    # The public single-function LLVM C view declares its original image as
    # external storage. Supply the fixture's data at the new link address and
    # let GNU ld relocate its function pointer to the rebuilt descriptor.
    decl = re.search(r"/\* neverd\.image: (0x[0-9A-Fa-f]+) \(original loaded image storage\) \*/\n"
                     r"extern uint8_t (\w+)\[(\d+)\];", source.read_text())
    if not decl:
        return []
    base, name, size = int(decl[1], 16), decl[2], int(decl[3])
    with original.open("rb") as stream:
        elf = ELFFile(stream)
        data = elf.get_section_by_name(".data")
        relocs = elf.get_section_by_name(".rela.data")
        symbols = elf.get_section(relocs["sh_link"])
        offset = data["sh_addr"] - base
        if not 0 <= offset <= size - data["sh_size"]:
            raise ValueError("LLVM C image declaration does not cover the fixture data")
        lines = [".data", ".balign 8", f".globl {name}", f"{name}:",
                 f".space {offset}", ".Lfixture_data:",
                 ".byte " + ", ".join(map(str, data.data())), f".org {name} + {size}"]
        for reloc in relocs.iter_relocations():
            symbol = symbols.get_symbol(reloc["r_info_sym"])
            if (reloc["r_info_type"] != ENUM_RELOC_TYPE_PPC64["R_PPC64_ADDR64"]
                    or symbol["st_info"]["type"] != "STT_FUNC"):
                raise ValueError("unexpected fixture data relocation")
            field = reloc["r_offset"] - data["sh_addr"]
            lines.extend([f".globl {symbol.name}", f".reloc .Lfixture_data + {field}, "
                          f"R_PPC64_ADDR64, {symbol.name} + {reloc['r_addend']}"])
    storage = source.with_suffix(".image.s")
    storage.write_text("\n".join(lines) + "\n")
    return [storage]


def execute(path, target, name, args):
    cpu, symbols = machine(path, target)
    order = "little" if target.endswith("le") else "big"
    registers = ppc_const
    sp = STACK + 2048

    def write_word(address, value):
        cpu.mem_write(address, value.to_bytes(8, order))

    if name == "callback" or (name == "through_global" and args[0]):
        # This separately allocated descriptor supplies a different TOC and
        # a nonzero environment. Its callee reads both before returning.
        code, toc, environment = DATA + 0x100, DATA + 0x200, DATA + 0x300
        for i, value in enumerate((code, toc, environment)):
            write_word(DATA + 8 * i, value)
        write_word(toc, 73)
        write_word(environment, 19)
        # ld r4,0(r2); ld r5,0(r11); add r3,r3,r4; add r3,r3,r5; blr.
        instructions = (0xE8820000, 0xE8AB0000, 0x7C632214, 0x7C632A14, 0x4E800020)
        cpu.mem_write(code, b"".join(i.to_bytes(4, order) for i in instructions))
        args = (DATA, args[-1])

    descriptor = symbols[name]
    code, toc, environment = (
        int.from_bytes(cpu.mem_read(descriptor + offset, 8), order)
        for offset in (0, 8, 16)
    )
    for register, value in ((registers.UC_PPC_REG_1, sp),
                            (registers.UC_PPC_REG_2, toc),
                            (registers.UC_PPC_REG_11, environment),
                            (registers.UC_PPC_REG_LR, RETURN)):
        cpu.reg_write(register, value)
    for i, value in enumerate(args):
        if i < 8:
            cpu.reg_write(registers.UC_PPC_REG_3 + i, value)
        else:
            write_word(sp + 112 + 8 * (i - 8), value)
    cpu.emu_start(code, RETURN, count=2000)
    if cpu.reg_read(registers.UC_PPC_REG_PC) != RETURN or cpu.reg_read(registers.UC_PPC_REG_1) != sp:
        raise RuntimeError(f"{path.name} {name} failed to restore the caller's PC and stack")
    result = cpu.reg_read(registers.UC_PPC_REG_3)
    if name == "counter_address":
        return result == symbols["load_counter"]
    return result


def cases(name):
    mask = (1 << 64) - 1
    for x in (0, 1, 17, 0x8000000000000001, mask):
        if name == "ninth":
            args = (x, 2, 3, 4, 5, 6, 7, 8, ((x << 1) ^ 0xF0E1D2C3) & mask)
            expected = sum(args)
        elif name == "counter_address":
            yield (), True
            return
        elif name == "through_global":
            yield (0, x), (x + 38) & mask
            yield (1, x), (x + 73 + 19 + 1) & mask
            continue
        else:
            args = (x,)
            expected = x + {"load_counter": 37, "add_counter": 37,
                            "after_call": 74, "callback": 73 + 19 + 37}[name]
        yield args, expected & mask


def check(path, target, name):
    for args, expected in cases(name):
        result = execute(path, target, name, args)
        if result != expected:
            raise RuntimeError(f"{path.name} {name}{args} returned {result}, expected {expected}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", required=True, type=Path)
    parser.add_argument("--clang", required=True, type=Path)
    parser.add_argument("--ld", required=True, type=Path, help="GNU ld with elf64ppc and elf64lppc")
    args = parser.parse_args()
    fixture = Path(__file__).with_name("ppc64-elfv1.c")
    names = ("load_counter", "add_counter", "ninth", "after_call", "callback",
             "counter_address", "through_global")
    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory)
        # A single-function unit can reference load_counter through a direct
        # call, its address, or the embedded slot initializer.
        support = work / "callee.c"
        support.write_text("unsigned long load_counter(unsigned long x) { return x + 37; }\n")
        for target in ("powerpc64", "powerpc64le"):
            original = work / f"{target}.elf"
            compile_elf(args.clang, args.ld, target, [fixture], names[0], original, "-O2",
                        retain_relocations=True)
            for name in names:
                check(original, target, name)
            module = work / f"{target}.ll"
            subprocess.run([args.neverd, "lift", "-o", module, original], check=True)
            for opt in ("-O0", "-O2"):
                rebuilt = work / f"{target}-llvm{opt}.elf"
                compile_elf(args.clang, args.ld, target, [module], names[0], rebuilt, opt,
                            relocated=True)
                for name in names:
                    check(rebuilt, target, name)
            for name in names:
                for route, switches in (("highc", []), ("llvmc", ["--llvm"])):
                    source = work / f"{target}-{name}-{route}.c"
                    subprocess.run([args.neverd, "decompile", "--func", name, *switches,
                                    "-o", source, original], check=True)
                    sources = [source] if name == "load_counter" else [source, support]
                    if route == "llvmc":
                        sources.extend(llvmc_image_storage(source, original))
                    for opt in ("-O0", "-O2"):
                        rebuilt = source.with_suffix(".elf")
                        compile_elf(args.clang, args.ld, target, sources, name, rebuilt, opt,
                                    relocated=True)
                        check(rebuilt, target, name)
            count = sum(1 for name in names for _ in cases(name))
            print(f"{target} ELFv1: {count} original and {6 * count} rebuilt results matched", flush=True)


if __name__ == "__main__":
    main()
