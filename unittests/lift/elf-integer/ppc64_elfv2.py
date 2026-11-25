#!/usr/bin/env python3
"""Execute ELFv2 global and local entries across native TOCs and PLT bindings."""

import argparse
from pathlib import Path
import subprocess
import tempfile

from elftools.elf.elffile import ELFFile
from elftools.elf.enums import ENUM_RELOC_TYPE_PPC64
import unicorn
from unicorn import ppc_const as reg

from ppc64_elfv1 import llvmc_image_storage
from ppc64_elfv1_plt import tocsave_object
from roundtrip import DATA, RETURN, STACK, machine, map_image


VALUES = (0, 1, 17, 0x8000000000000001, 0xffffffffffffffff)
PROVIDER = 0x28000000
BIAS = 0x03000000
POISON = 0xdeada000
MASK = (1 << 64) - 1


def execute(path, target, name, provider, lazy):
    with path.open("rb") as stream:
        elf = ELFFile(stream)
        bias = BIAS if elf["e_type"] == "ET_DYN" else 0
        symbols = elf.get_section_by_name(".symtab") or elf.get_section_by_name(".dynsym")
        symbol = symbols.get_symbol_by_name(name)[0]
        encoding = symbol["st_other"]["local"]
        entry = bias + symbol["st_value"]
        offset = (1 << encoding) if encoding >= 2 else 0
    order = "little" if target.endswith("le") else "big"

    def put(cpu, addr, value):
        cpu.mem_write(addr, value.to_bytes(8, order))

    def relocate(cpu, elf, base, exports):
        table = elf.get_section_by_name(".rela.dyn")
        for r in table.iter_relocations() if table else ():
            if r["r_info_type"] == ENUM_RELOC_TYPE_PPC64["R_PPC64_RELATIVE"]:
                value = base + r["r_addend"]
            elif r["r_info_type"] == ENUM_RELOC_TYPE_PPC64["R_PPC64_ADDR64"]:
                sym = elf.get_section(table["sh_link"]).get_symbol(r["r_info_sym"])
                value = (exports[sym.name] if sym["st_shndx"] == "SHN_UNDEF"
                         else base + sym["st_value"]) + r["r_addend"]
            else:
                raise ValueError(f"unexpected fixture dynamic relocation {r.entry}")
            put(cpu, base + r["r_offset"], value)

    count = 0
    for local in ((False, True) if offset else (False,)):
        cpu, _ = machine(path, target, bias)
        # Execute the linked prologue once to obtain the caller's native TOC
        # for a local entry. Pass a different r12 at that entry.
        toc = POISON
        if offset:
            cpu.reg_write(reg.UC_PPC_REG_12, entry)
            cpu.reg_write(reg.UC_PPC_REG_2, POISON)
            cpu.emu_start(entry, entry + offset, count=32)
            toc = cpu.reg_read(reg.UC_PPC_REG_2)
            if cpu.reg_read(reg.UC_PPC_REG_PC) != entry + offset or toc == POISON:
                raise RuntimeError("global entry did not establish its local TOC")

        bound = set()
        if provider:
            with provider.open("rb") as stream:
                elf = ELFFile(stream)
                map_image(cpu, elf, PROVIDER)
                exports = {s.name: PROVIDER + s["st_value"]
                           for s in elf.get_section_by_name(".dynsym").iter_symbols()
                           if s["st_info"]["type"] == "STT_FUNC" and s["st_shndx"] != "SHN_UNDEF"}
                relocate(cpu, elf, PROVIDER, exports)
            with path.open("rb") as stream:
                elf = ELFFile(stream)
                relocate(cpu, elf, bias, exports)
                dynamic = elf.get_section_by_name(".dynamic")
                tags = {t.entry.d_tag: t.entry.d_val for t in dynamic.iter_tags()} if dynamic else {}
                table = elf.get_section_by_name(".rela.plt")
                syms = elf.get_section(table["sh_link"]) if table else None
                slots = []
                for r in table.iter_relocations() if table else ():
                    if r["r_info_type"] != ENUM_RELOC_TYPE_PPC64["R_PPC64_JMP_SLOT"]:
                        raise ValueError("unexpected PLT relocation")
                    slots.append((bias + r["r_offset"], exports[syms.get_symbol(r["r_info_sym"]).name]))
                if slots and lazy:
                    glink = bias + next(tags[k] for k in ("DT_PPC64_GLINK", "DT_LOPROC", 0x70000000) if k in tags)
                    plt = bias + tags["DT_PLTGOT"]
            for i, (slot, address) in enumerate(slots):
                put(cpu, slot, glink + 32 + 4 * i if lazy else address)
            if lazy:
                put(cpu, plt, DATA)
                put(cpu, plt + 8, DATA + 8)

                def resolve(uc, address, size, data):
                    i = uc.reg_read(reg.UC_PPC_REG_0)
                    if i >= len(slots) or i in bound or uc.reg_read(reg.UC_PPC_REG_11) != DATA + 8:
                        raise RuntimeError("incorrect ELFv2 lazy resolver state")
                    bound.add(i)
                    slot, callee = slots[i]
                    put(uc, slot, callee)
                    uc.reg_write(reg.UC_PPC_REG_12, callee)
                    uc.reg_write(reg.UC_PPC_REG_PC, callee)

                cpu.hook_add(unicorn.UC_HOOK_CODE, resolve, begin=DATA, end=DATA)

            def check_global(uc, address, size, data):
                if uc.reg_read(reg.UC_PPC_REG_12) != address:
                    raise RuntimeError("PLT did not supply the provider's global entry in r12")

            for dest in set(exports.values()):
                cpu.hook_add(unicorn.UC_HOOK_CODE, check_global, begin=dest, end=dest)

        for x in VALUES:
            sp = STACK + 2048
            for r, v in ((reg.UC_PPC_REG_1, sp), (reg.UC_PPC_REG_2, toc if local else POISON),
                         (reg.UC_PPC_REG_12, POISON if local or encoding < 2 else entry),
                         (reg.UC_PPC_REG_3, x), (reg.UC_PPC_REG_LR, RETURN)):
                cpu.reg_write(r, v)
            if name == "callback_cross":
                cpu.reg_write(reg.UC_PPC_REG_3, exports["remote_apply"])
                cpu.reg_write(reg.UC_PPC_REG_4, x)
            cpu.emu_start(entry + (offset if local else 0), RETURN, count=2000)
            expected = {"cross": 3 * (x + 3) + 93 + 37,
                        "callback_cross": 3 * (x + 3) + 93 + 37,
                        "leaf_cross": 3 * (x + 3) + 93 + 37,
                        "twice": 93 - (3 * (x + 1) + 93 + 2) + 37,
                        "local_cross": x + 93 + 37, "global_cross": x + 93 + 37,
                        "other_toc": x + 93, "notoc_cross": 3 * (x + 1) + 93 + 19}[name] & MASK
            actual = cpu.reg_read(reg.UC_PPC_REG_3)
            if (actual != expected or cpu.reg_read(reg.UC_PPC_REG_PC) != RETURN or
                    cpu.reg_read(reg.UC_PPC_REG_1) != sp or
                    ((local or not encoding) and cpu.reg_read(reg.UC_PPC_REG_2) != toc)):
                raise RuntimeError(f"{path.name} {name}({x}) local={local} lazy={lazy}: "
                                   f"r3={actual}, expected {expected}, r2={cpu.reg_read(reg.UC_PPC_REG_2):#x}")
            count += 1
        if lazy and len(bound) != (2 if name == "twice" else 1):
            raise RuntimeError("incorrect number of first-call PLT bindings")
    return count


def check_mismatched_slot(neverd, app):
    raw = bytearray(app.read_bytes())
    with app.open("rb") as stream:
        elf = ELFFile(stream)
        relocs = elf.get_section_by_name(".rela.plt")
        syms = elf.get_section(relocs["sh_link"])
        for i, reloc in enumerate(relocs.iter_relocations()):
            if syms.get_symbol(reloc["r_info_sym"]).name == "remote_apply":
                reloc.entry.r_offset = elf.get_section_by_name(".data")["sh_addr"]
                entry = elf.structs.Elf_Rela.build(reloc.entry)
                pos = relocs["sh_offset"] + i * relocs["sh_entsize"]
                raw[pos:pos + len(entry)] = entry
                break
        else:
            raise ValueError("missing fixture jump slot")
    bad = app.with_name("wrong-slot.elf")
    bad.write_bytes(raw)
    for route in ([], ["--llvm"]):
        result = subprocess.run([neverd, "decompile", "--func", "cross", *route, bad],
                                capture_output=True, text=True)
        if not result.returncode or "no unique call target or TOC" not in result.stderr + result.stdout:
            raise RuntimeError("accepted or misdiagnosed a PPC64 PLT stub without its runtime slot")


def qualify(args, work, target):
    fixtures = Path(__file__).parent
    cc = [args.clang, f"--target={target}-linux-gnu", "-mcpu=power8", "-mabi=elfv2",
          "-ffreestanding", "-fno-stack-protector", "-fPIC", "-nostdlib"]
    link = [f"-fuse-ld={args.ld}"]
    source = fixtures / "powerpc-plt.c"
    provider = work / "provider.so"
    subprocess.run([*cc, *link, "-shared", "-O2", "-DPROVIDER", "-DTEST_ELFV2_LOCALENTRY", source,
                    "-Wl,-soname,provider.so", "-o", provider], check=True)
    gnu = "GNU ld" in subprocess.check_output([args.ld, "--version"], text=True)

    def base(value):
        return f"-Wl,{'-Ttext-segment' if gnu else '--image-base'}={value}"

    cases = [("exec", ["-no-pie"], ("cross", "twice", "callback_cross")),
             ("dso", ["-shared", "-Wl,-Bsymbolic"], ("cross", "twice")),
             ("sectionless", ["-shared", "-Wl,-Bsymbolic"], ("cross",)),
             ("notoc", ["-no-pie", "-Wl,--no-power10-stubs"], ("notoc_cross",)),
             ("pcrel", ["-no-pie", "-Wl,--power10-stubs"], ("notoc_cross",)),
             ("global", ["-static"], ("global_cross", "other_toc")),
             ("same", ["-static"], ("local_cross",))]
    if gnu:
        cases.append(("tocsave", ["-no-pie"], ("cross", "twice")))
        cases.append(("leaf", ["-no-pie", "-Wl,--plt-localentry,-z,now"], ("leaf_cross",)))
        cases.append(("multi", ["-static", "-Wl,--no-toc-optimize"], ("local_cross",)))
    total = 0
    for kind, flags, names in cases:
        app = work / f"{kind}.elf"
        dependent = kind in ("exec", "dso", "sectionless", "notoc", "pcrel", "leaf", "tocsave")
        support = [provider] if dependent else []
        companion = []
        if kind in ("same", "multi"):
            objects = []
            for part in ("local", "partner"):
                obj = work / f"{kind}-{part}.o"
                defs = (["-DSINGLE_TOC"] if kind == "same" else []) + (["-DCALLER"] if part == "local" else [])
                subprocess.run([*cc, "-DELF_V2", *defs, "-c", fixtures / "ppc64-multitoc.S", "-o", obj], check=True)
                objects.append(obj)
            original, companion = objects, objects[1:]
        elif kind == "global":
            original = [fixtures / "ppc64-elfv2-global.S"]
        elif kind == "tocsave":
            obj = work / "tocsave.o"
            tocsave_object([*cc, "-DELF_V2"], fixtures / "ppc64-tocsave.S", obj)
            original = [obj]
        elif kind == "leaf":
            original = ["-DTEST_ELFV2_LOCALENTRY", source]
        elif kind in ("notoc", "pcrel"):
            original = [fixtures / "ppc64-elfv2-notoc.S"]
        else:
            original = ["-DTEST_ELFV2_CALLBACK", source] if kind == "exec" else [source]
        strip = ["-Wl,--strip-all"] if kind in ("dso", "sectionless") else []
        subprocess.run([*cc, *link, *flags, *strip, "-O2", *original, *support,
                        f"-Wl,-e,{names[0]}", base("0x10000000"), "-o", app], check=True)

        data_section = ".data"
        if kind in ("same", "multi"):
            # LLD retains the .toc input. GNU ld folds it into .got.
            with app.open("rb") as stream:
                data_section = ".toc" if ELFFile(stream).get_section_by_name(".toc") else ".got"
        if kind == "exec":
            check_mismatched_slot(args.neverd, app)

        analysis_input = app
        if kind == "sectionless":
            # Keep the sectioned twin for the independent relocation oracle.
            with app.open("rb") as stream:
                elf = ELFFile(stream)
                for field in ("e_shoff", "e_shnum", "e_shstrndx"):
                    elf.header[field] = 0
                header = elf.structs.Elf_Ehdr.build(elf.header)
            raw = bytearray(app.read_bytes())
            raw[:len(header)] = header
            analysis_input = work / "no-section-headers.so"
            analysis_input.write_bytes(raw)

        def check(image, name):
            return sum(execute(image, target, name, provider if dependent else None, lazy)
                       for lazy in ((False, True) if dependent and kind != "leaf" and name != "callback_cross" else (False,)))

        # Unicorn 2.1.4 executes the POWER8 form used by the preceding case.
        # The POWER10 case lifts the real prefixed stub; execute its rebuilt
        # calls against the same provider and expected values.
        if kind != "pcrel":
            total += sum(check(app, name) for name in names)
        ir = app.with_suffix(".ll")
        subprocess.run([args.neverd, "lift", "-o", ir, analysis_input], check=True)
        def decompile(name, route):
            c = work / f"{kind}-{name}-{route}.c"
            subprocess.run([args.neverd, "decompile", "--func", name,
                            *(["--llvm"] if route == "llvmc" else []), "-o", c, analysis_input], check=True)
            if kind == "dso" and name == "cross":
                with app.open("rb") as stream:
                    elf = ELFFile(stream)
                    sym = elf.get_section_by_name(".dynsym").get_symbol_by_name(name)[0]
                    encoding = sym["st_other"]["local"]
                    if encoding < 2:
                        raise RuntimeError("fixture has no distinct local entry")
                    local = sym["st_value"] + (1 << encoding)
                alias = work / f"local-entry-{route}.c"
                subprocess.run([args.neverd, "decompile", "--func", hex(local),
                                *(["--llvm"] if route == "llvmc" else []),
                                "-o", alias, analysis_input], check=True)
                if alias.read_bytes() != c.read_bytes():
                    raise RuntimeError(f"{route} gave the local entry a different function body")
            return c

        for name in names:
            for route in ("highc", "llvmc", "llvm"):
                sources = [ir]
                if route != "llvm":
                    c = decompile(name, route)
                    sources = [c, *companion]
                    if route == "llvmc":
                        sources += llvmc_image_storage(c, app, data_section)
                    if name == "global_cross":
                        # The original private TOC is established at its global entry.
                        # Rebuild the callee too so the native linker can use its new local entry.
                        sources.append(decompile("other_toc", route))
                for opt in ("-O0", "-O2"):
                    exe = work / f"{kind}-{name}-{route}{opt}.elf"
                    subprocess.run([*cc, *link, *flags, *(["-std=gnu17"] if route != "llvm" else []),
                                    opt, *sources, *support, f"-Wl,-e,{name}", base("0x18000000"),
                                    "-o", exe], check=True)
                    total += check(exe, name)
    print(f"{target} ELFv2: {total} original and rebuilt executions matched", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("neverd", "clang", "ld"):
        parser.add_argument("--" + flag, type=Path, required=True)
    args = parser.parse_args()
    for target in ("powerpc64", "powerpc64le"):
        with tempfile.TemporaryDirectory() as work:
            qualify(args, Path(work), target)


if __name__ == "__main__":
    main()
