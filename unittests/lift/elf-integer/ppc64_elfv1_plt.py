#!/usr/bin/env python3
"""Execute ELFv1 inter-module and multi-TOC calls before and after rebuilding."""

import argparse
from pathlib import Path
import subprocess
import tempfile

from elftools.elf.elffile import ELFFile
from elftools.elf.enums import ENUM_RELOC_TYPE_PPC64
import unicorn
from unicorn import ppc_const as reg

from image_storage import llvmc_image_storage
from roundtrip import DATA, RETURN, STACK, machine, map_image


MASK = (1 << 64) - 1
VALUES = (0, 1, 17, 0x8000000000000001, MASK)
PROVIDER_BASE = 0x28000000
CALLER_BIAS = 0x03000000


def execute(app, target, name, value, provider=None, lazy=False, static_chain=False):
    with app.open("rb") as file:
        bias = CALLER_BIAS if ELFFile(file)["e_type"] == "ET_DYN" else 0
    cpu, symbols = machine(app, target, bias)
    order = "little" if target.endswith("le") else "big"

    def word(addr):
        return int.from_bytes(cpu.mem_read(addr, 8), order)

    def put(addr, number):
        cpu.mem_write(addr, number.to_bytes(8, order))

    def relocate(elf, load_bias):
        table = elf.get_section_by_name(".rela.dyn")
        for r in table.iter_relocations() if table else ():
            if r["r_info_type"] != ENUM_RELOC_TYPE_PPC64["R_PPC64_RELATIVE"]:
                raise ValueError("unexpected fixture relocation")
            put(load_bias + r["r_offset"], load_bias + r["r_addend"])

    slots = []
    bound = set()
    if provider:
        # Load a second ELF image. The runtime binds the three words of the
        # exported descriptor after applying the provider's own base relocs.
        with provider.open("rb") as file:
            elf = ELFFile(file)
            map_image(cpu, elf, PROVIDER_BASE)
            relocate(elf, PROVIDER_BASE)
            exports = {s.name: PROVIDER_BASE + s["st_value"]
                       for s in elf.get_section_by_name(".dynsym").iter_symbols()
                       if s["st_info"]["type"] == "STT_FUNC" and s["st_shndx"] != "SHN_UNDEF"}
        with app.open("rb") as file:
            elf = ELFFile(file)
            relocate(elf, bias)
            dynamic = elf.get_section_by_name(".dynamic")
            tags = {t.entry.d_tag: t.entry.d_val for t in dynamic.iter_tags()}
            # pyelftools can spell the processor's first tag DT_LOPROC.
            glink = bias + next(tags[k] for k in ("DT_PPC64_GLINK", "DT_LOPROC", 0x70000000) if k in tags)
            table = elf.get_section_by_name(".rela.plt")
            syms = elf.get_section(table["sh_link"])
            for r in table.iter_relocations():
                if r["r_info_type"] != ENUM_RELOC_TYPE_PPC64["R_PPC64_JMP_SLOT"]:
                    raise ValueError("unexpected application jump slot")
                slots.append((bias + r["r_offset"], exports[syms.get_symbol(r["r_info_sym"]).name]))

        def bind(index):
            slot, descriptor = slots[index]
            cpu.mem_write(slot, bytes(cpu.mem_read(descriptor, 24)))
            bound.add(index)

        for index, (slot, _) in enumerate(slots):
            if lazy:
                put(slot, glink + 32 + index * 8)
                put(slot + 8, 0)
                put(slot + 16, 0)
            else:
                bind(index)
        if lazy:
            # The real GNU .glink code loads the resolver descriptor and
            # passes the relocation ordinal in r0. Implement only the dynamic
            # linker's symbol lookup; the CPU runs all application/PLT code.
            resolver = bias + tags["DT_PLTGOT"]
            put(resolver, DATA)
            put(resolver + 8, DATA + 8)
            put(resolver + 16, DATA + 16)

            def resolve(uc, address, size, user_data):
                index = uc.reg_read(reg.UC_PPC_REG_0)
                if (index >= len(slots) or index in bound or
                        uc.reg_read(reg.UC_PPC_REG_2) != DATA + 8 or
                        uc.reg_read(reg.UC_PPC_REG_11) != DATA + 16):
                    raise RuntimeError("incorrect ELFv1 lazy resolver state")
                bind(index)
                slot = slots[index][0]
                uc.reg_write(reg.UC_PPC_REG_2, word(slot + 8))
                uc.reg_write(reg.UC_PPC_REG_11, word(slot + 16))
                uc.reg_write(reg.UC_PPC_REG_PC, word(slot))

            cpu.hook_add(unicorn.UC_HOOK_CODE, resolve, begin=DATA, end=DATA)

    descriptor = symbols[name]
    toc = word(descriptor + 8)
    sp = STACK + 2048
    for r, v in ((reg.UC_PPC_REG_1, sp), (reg.UC_PPC_REG_2, toc),
                 (reg.UC_PPC_REG_11, word(descriptor + 16)), (reg.UC_PPC_REG_3, value),
                 (reg.UC_PPC_REG_LR, RETURN)):
        cpu.reg_write(r, v)
    cpu.emu_start(word(descriptor), RETURN, count=2000)
    result = cpu.reg_read(reg.UC_PPC_REG_3)
    expected = {"cross": (value + 3) * 3 + 93 + 37,
                "twice": (19 if static_chain else 0) + 93 - ((value + 1) * 3 + 93 + 2) + 37,
                "local_cross": value + 93 + 37}[name] & MASK
    if (result != expected or cpu.reg_read(reg.UC_PPC_REG_PC) != RETURN or
            cpu.reg_read(reg.UC_PPC_REG_1) != sp or cpu.reg_read(reg.UC_PPC_REG_2) != toc):
        raise RuntimeError(f"{app.name} {name}({value}): result {result}, expected {expected}; "
                           f"r2={cpu.reg_read(reg.UC_PPC_REG_2):#x}, caller TOC={toc:#x}")
    if lazy and len(bound) != {"cross": 1, "twice": 2}[name]:
        raise RuntimeError("execution did not resolve every called PLT slot")


def tocsave_object(cc, source, output):
    # Clang's integrated assembler has no .reloc spelling for TOCSAVE.
    # Keep the RELA symbols and addends and give GNU ld their ABI type.
    subprocess.run([*cc, "-DTOCSAVE_RELOC=R_PPC64_ADDR64", "-c", source, "-o", output], check=True)
    with output.open("r+b") as stream:
        elf = ELFFile(stream)
        table = elf.get_section_by_name(".rela.text")
        markers = [(i, r) for i, r in enumerate(table.iter_relocations())
                   if r["r_info_type"] == ENUM_RELOC_TYPE_PPC64["R_PPC64_ADDR64"]]
        if len(markers) != 5:
            raise ValueError("expected two prologues and three TOCSAVE calls")
        for i, r in markers:
            info = (r["r_info_sym"] << 32) | ENUM_RELOC_TYPE_PPC64["R_PPC64_TOCSAVE"]
            stream.seek(table["sh_offset"] + i * table["sh_entsize"] + 8)
            stream.write(info.to_bytes(8, "little" if elf.little_endian else "big"))


def qualify(args, work, target):
    fixture = Path(__file__).with_name("powerpc-plt.c")
    multi = fixture.with_name("ppc64-multitoc.S")
    cc = [args.clang, f"--target={target}-linux-gnu", "-mabi=elfv1", "-mcpu=power8",
          "-ffreestanding", "-fno-stack-protector", "-fPIC", "-nostdlib"]
    link = [f"-fuse-ld={args.ld}"]
    providers = {}
    for chain in (False, True):
        provider = work / ("chain.so" if chain else "provider.so")
        implementation = ["-DSTATIC_CHAIN", fixture.with_name("ppc64-elfv1-chain.S")] if chain else []
        subprocess.run([*cc, *link, "-shared", "-DPROVIDER", "-O2", fixture,
                        *implementation, f"-Wl,-soname,{provider.name}", "-o", provider], check=True)
        providers[chain] = provider
    inputs = []
    for stem, flags in (("local", ["-DCALLER"]), ("partner", [])):
        obj = work / (stem + ".o")
        subprocess.run([*cc, *flags, "-c", multi, "-o", obj], check=True)
        inputs.append(obj)
    saved = work / "tocsave.o"
    tocsave_object(cc, fixture.with_name("ppc64-tocsave.S"), saved)
    count = 0
    cases = (("plt", ["-no-pie"], ("cross", "twice")),
             ("chain", ["-shared", "-Wl,-Bsymbolic,--plt-static-chain,--plt-thread-safe"],
              ("cross", "twice")),
             ("saved", ["-no-pie"], ("cross", "twice")),
             ("multi", ["-static", "-Wl,--no-toc-optimize"], ("local_cross",)))
    for kind, flags, names in cases:
        shared = kind != "multi"
        provider = providers[kind == "chain"] if shared else None
        app = work / (kind + ".elf")
        original = [saved if kind == "saved" else fixture, provider] if shared else inputs
        strip = ["-Wl,--strip-all"] if kind == "chain" else []
        subprocess.run([*cc, *link, *flags, *strip, "-O2", *original, f"-Wl,-e,{names[0]}",
                        "-Wl,-Ttext-segment=0x10000000", "-o", app], check=True)

        def check(path, function):
            nonlocal count
            for lazy in ((False, True) if shared else (False,)):
                for x in VALUES:
                    execute(path, target, function, x, provider, lazy, kind == "chain")
                    count += 1

        for name in names:
            check(app, name)
        module = work / (kind + ".ll")
        subprocess.run([args.neverd, "lift", "-o", module, app], check=True)
        for name in names:
            for route in ("llvm", "highc", "llvmc"):
                sources = [module]
                if route != "llvm":
                    source = work / f"{kind}-{name}-{route}.c"
                    switches = ["--llvm"] if route == "llvmc" else []
                    subprocess.run([args.neverd, "decompile", "--func", name, *switches,
                                    "-o", source, app], check=True)
                    sources = [source]
                    if route == "llvmc":
                        sources.extend(llvmc_image_storage(source, app, ".data" if shared else ".got"))
                        if "twice" in names:
                            # The original image backing can contain the other
                            # exported descriptor. Link its real native owner.
                            companion = "-DONLY_TWICE" if name == "cross" else "-DONLY_CROSS"
                            sources.extend([companion, fixture])
                    if not shared:
                        sources.append(inputs[1])
                for opt in ("-O0", "-O2"):
                    output = work / f"{kind}-{name}-{route}{opt}.elf"
                    std = ["-std=gnu17"] if route != "llvm" else []
                    subprocess.run([*cc, *link, *flags, *std, opt, *sources,
                                    *([provider] if shared else []), f"-Wl,-e,{name}",
                                    "-Wl,-Ttext-segment=0x18000000", "-o", output], check=True)
                    check(output, name)
    print(f"{target} ELFv1 linkage: {count} original and rebuilt executions matched", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("neverd", "clang", "ld"):
        parser.add_argument("--" + flag, required=True, type=Path)
    args = parser.parse_args()
    for target in ("powerpc64", "powerpc64le"):
        with tempfile.TemporaryDirectory() as scratch:
            qualify(args, Path(scratch), target)


if __name__ == "__main__":
    main()
