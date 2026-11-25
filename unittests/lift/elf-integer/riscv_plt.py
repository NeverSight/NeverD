#!/usr/bin/env python3
"""Execute RISC-V PLT callers before and after recompilation with two DSOs."""

import argparse
import itertools
import re
from pathlib import Path
import subprocess
import tempfile

from elftools.elf.elffile import ELFFile
import unicorn
from unicorn import riscv_const as reg

from roundtrip import DATA, RETURN, STACK, compiler_flags, map_image, without_sections


def execute(path, providers, lazy, tail, rebuilt=False):
    with path.open("rb") as stream:
        elf = ELFFile(stream)
        size = elf.elfclass // 8
        mask = (1 << elf.elfclass) - 1
        mode = unicorn.UC_MODE_RISCV32 if size == 4 else unicorn.UC_MODE_RISCV64
        cpu = unicorn.Uc(unicorn.UC_ARCH_RISCV, mode)
        bias = (0x07000000 if rebuilt else 0x05000000) if elf["e_type"] == "ET_DYN" else 0
        map_image(cpu, elf, bias)
        start = bias + elf["e_entry"]
        dynamic = next(s for s in elf.iter_segments() if s["p_type"] == "PT_DYNAMIC")
        got = bias + next(t.entry.d_val for t in dynamic.iter_tags("DT_PLTGOT"))
        tables = dynamic.get_relocation_tables()
        slots = []
        for r in tables["JMPREL"].iter_relocations():
            if r["r_info_type"] != 5:  # R_RISCV_JUMP_SLOT
                raise ValueError(f"unexpected PLT relocation {r.entry}")
            slots.append((bias + r["r_offset"], dynamic.get_symbol(r["r_info_sym"]).name))

    def put(address, value):
        cpu.mem_write(address, value.to_bytes(size, "little"))

    exports = {}
    for index, provider in enumerate(providers):
        with provider.open("rb") as stream:
            elf = ELFFile(stream)
            base = 0x10000000 * (index + 1) + (0x800000 if rebuilt else 0)
            map_image(cpu, elf, base)
            for sym in elf.get_section_by_name(".dynsym").iter_symbols():
                if sym["st_info"]["type"] == "STT_FUNC" and sym["st_shndx"] != "SHN_UNDEF":
                    exports[sym.name] = base + sym["st_value"]
    expected_imports = {"remote_second"} if tail else {"remote_first", "remote_second"}
    if {name for _, name in slots} != expected_imports:
        raise ValueError(f"unexpected import bindings {slots}")
    for page in (DATA, STACK, RETURN):
        cpu.mem_map(page, 4096)
    resolved = []
    for slot, name in slots:
        if lazy:
            put(slot, bias + int.from_bytes(cpu.mem_read(slot, size), "little"))
        else:
            put(slot, exports[name])
    if lazy:
        resolver = exports["fixture_resolver"]
        put(got, resolver)
        put(got + size, DATA + size)

        def resolve(uc, address, length, data):
            index, remainder = divmod(uc.reg_read(reg.UC_RISCV_REG_T1), size)
            if (remainder or index >= len(slots) or index in resolved or
                    uc.reg_read(reg.UC_RISCV_REG_T0) != DATA + size):
                raise ValueError("unexpected RISC-V PLT resolver arguments")
            resolved.append(index)
            slot, name = slots[index]
            put(slot, exports[name])
            uc.reg_write(reg.UC_RISCV_REG_T3, exports[name])

        cpu.hook_add(unicorn.UC_HOOK_CODE, resolve, begin=resolver, end=resolver)

    values = (0, 1, 0x123456789abcdef0 & mask, mask // 2, mask - 2, mask)
    saved = {reg.UC_RISCV_REG_X0 + n: (0xfedcba9876543200 + n) & mask
             for n in (3, 4, 8, 9, *range(18, 28))}
    for x in values:
        sp = STACK + 2048
        for register, value in saved.items():
            cpu.reg_write(register, value)
        cpu.reg_write(reg.UC_RISCV_REG_SP, sp)
        cpu.reg_write(reg.UC_RISCV_REG_RA, RETURN)
        cpu.reg_write(reg.UC_RISCV_REG_A0, x)
        cpu.emu_start(start, RETURN, count=300)
        expected = (93 - (x + 3) if tail else 93 - (3 * (x + 3) + 17 + 2) + 5) & mask
        if (cpu.reg_read(reg.UC_RISCV_REG_A0) != expected or
                cpu.reg_read(reg.UC_RISCV_REG_SP) != sp or
                cpu.reg_read(reg.UC_RISCV_REG_PC) != RETURN):
            raise ValueError(f"{path.name}: incorrect result or return at {x:#x}")
        if any(cpu.reg_read(register) != value for register, value in saved.items()):
            raise ValueError(f"{path.name}: clobbered a preserved register at {x:#x}")
    if lazy and len(resolved) != len(slots):
        raise ValueError("the PLT did not resolve every import")
    return len(values)


def qualify(args, work, profile):
    cc = [args.clang, *compiler_flags(profile), "-fPIC", "-std=gnu17"]
    ld = [args.ld, "-m", "elf32lriscv" if profile == "riscv32" else "elf64lriscv"]
    source = Path(__file__).with_name("riscv-plt.c")
    obj = work / "unit.o"
    providers = []
    for name in ("first", "second"):
        subprocess.run([*cc, "-O2", "-D" + name.upper() + "_PROVIDER",
                        "-c", source, "-o", obj], check=True)
        provider = work / f"lib{name}.so"
        subprocess.run([*ld, "-shared", "-soname", provider.name, obj, "-o", provider], check=True)
        providers.append(provider)
    original_count = rebuilt_count = 0
    for tail in (False, True):
        caller = work / "caller.o"
        subprocess.run([*cc, "-O2", *(["-DTAIL_CALL"] if tail else []),
                        "-c", source, "-o", caller], check=True)
        for kind, binding, relax in itertools.product(
                ("exec", "dso"), ("lazy", "now"), ("--relax", "--no-relax")):
            app = work / f"{kind}-{binding}{relax}.elf"
            link = [*ld, *(["-shared"] if kind == "dso" else []), relax,
                    "-z", binding, "-z", "defs", "-E", "-e", "call_remote"]
            subprocess.run([*link, caller, *providers, "-o", app], check=True)
            for image in (app, without_sections(app)):
                original_count += execute(image, providers, binding == "lazy", tail)
                for route in ("highc", "llvmc", "llvm"):
                    if route == "llvm":
                        output = work / "caller.ll"
                        command = ["lift"]
                    else:
                        output = work / "caller.c"
                        command = ["decompile", "--func", "call_remote"]
                        if route == "llvmc":
                            command.append("--llvm")
                    subprocess.run([args.neverd, *command, "-o", output, image], check=True)
                    if route == "llvm" and not re.search(
                            r"^define[^\n]*@call_remote\(", output.read_text(), re.MULTILINE):
                        raise ValueError(f"{image.name}: whole-module lift omitted call_remote")
                    for level in ("-O0", "-O2"):
                        subprocess.run([*cc, level, "-c", output, "-o", obj], check=True)
                        rebuilt = work / "rebuilt.elf"
                        subprocess.run([*link, obj, *providers, "-o", rebuilt], check=True)
                        rebuilt_count += execute(rebuilt, providers, binding == "lazy", tail, rebuilt=True)
    print(f"{profile}: {original_count} original and {rebuilt_count} rebuilt results matched", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "clang", "ld"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    for profile in ("riscv32", "riscv64"):
        with tempfile.TemporaryDirectory() as directory:
            qualify(args, Path(directory), profile)


if __name__ == "__main__":
    main()
