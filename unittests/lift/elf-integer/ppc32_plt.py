#!/usr/bin/env python3
"""Execute PPC32 System V jump slots and rebuild the callers at another base."""

import argparse
from pathlib import Path
import re
import subprocess
import tempfile

from elftools.elf.constants import P_FLAGS
from elftools.elf.elffile import ELFFile
from elftools.elf.enums import ENUM_RELOC_TYPE_PPC as PPC
import unicorn
from unicorn import ppc_const as reg

from roundtrip import DATA, RETURN, STACK, machine, map_image


VALUES = (0, 1, 17, 0x80000001, 0xffffffff)
MASK = (1 << 32) - 1


def execute(path, target, name, provider, bias, lazy):
    order = "little" if target.endswith("le") else "big"
    with path.open("rb") as stream:
        if ELFFile(stream)["e_type"] == "ET_EXEC":
            bias = 0
    cpu, entries = machine(path, target, bias)
    # Keep the provider in range of the one-instruction BSS-PLT branch.
    provider_base = (entries[name] & 0xff000000) + 0x800000

    def put(address, value):
        cpu.mem_write(address, (value & MASK).to_bytes(4, order))

    def relocate(elf, base):
        table = elf.get_section_by_name(".rela.dyn")
        for r in table.iter_relocations() if table else ():
            if r["r_info_type"] == PPC["R_PPC_RELATIVE"]:
                value = base + r["r_addend"]
            elif r["r_info_type"] in (PPC["R_PPC_GLOB_DAT"], PPC["R_PPC_ADDR32"]):
                sym = elf.get_section(table["sh_link"]).get_symbol(r["r_info_sym"])
                value = (exports[sym.name] if sym["st_shndx"] == "SHN_UNDEF"
                         else base + sym["st_value"]) + r["r_addend"]
            else:
                raise ValueError(f"unexpected fixture relocation {r.entry}")
            put(base + r["r_offset"], value)

    with provider.open("rb") as stream:
        elf = ELFFile(stream)
        map_image(cpu, elf, provider_base)
        exports = {s.name: provider_base + s["st_value"]
                   for s in elf.get_section_by_name(".dynsym").iter_symbols()
                   if s["st_info"]["type"] == "STT_FUNC" and s["st_shndx"] != "SHN_UNDEF"}
        relocate(elf, provider_base)

    with path.open("rb") as stream:
        elf = ELFFile(stream)
        relocate(elf, bias)
        tags = {t.entry.d_tag: t.entry.d_val
                for t in elf.get_section_by_name(".dynamic").iter_tags()}
        got = next((tags[k] for k in ("DT_PPC_GOT", "DT_LOPROC", 0x70000000) if k in tags), None)
        table = elf.get_section_by_name(".rela.plt")
        slots = []
        if lazy and table is None:
            raise ValueError("lazy binding requires a runtime PLT")
        stride = table["sh_entsize"] if table else 0
        symbols = elf.get_section(table["sh_link"]) if table else None
        for r in table.iter_relocations() if table else ():
            if r["r_info_type"] != PPC["R_PPC_JMP_SLOT"]:
                raise ValueError("fixture lacks a PPC32 jump slot")
            callee = exports[symbols.get_symbol(r["r_info_sym"]).name]
            slot = bias + r["r_offset"]
            slots.append((slot, callee))
            if got is None:
                if lazy:
                    raise ValueError("BSS-PLT fixture uses loader-installed direct branches")
                distance = callee - slot
                if distance % 4 or not -(1 << 25) <= distance < 1 << 25:
                    raise ValueError("provider outside BSS-PLT branch range")
                put(slot, 0x48000000 | (distance & 0x03fffffc))
            elif lazy:
                # Execute the linker's original glink entry and resolver.
                put(slot, bias + int.from_bytes(cpu.mem_read(slot, 4), order))
            else:
                put(slot, callee)

    bound = set()
    if lazy:
        if not got:
            raise ValueError("lazy fixture has no resolver GOT")
        put(bias + got + 4, DATA)
        put(bias + got + 8, DATA + 8)

        def resolve(uc, address, size, data):
            offset = uc.reg_read(reg.UC_PPC_REG_11)
            index, low = divmod(offset, stride)
            if low or index >= len(slots) or index in bound or uc.reg_read(reg.UC_PPC_REG_12) != DATA + 8:
                raise ValueError("incorrect PPC32 lazy resolver arguments")
            bound.add(index)
            slot, callee = slots[index]
            put(slot, callee)
            uc.reg_write(reg.UC_PPC_REG_PC, callee)

        cpu.hook_add(unicorn.UC_HOOK_CODE, resolve, begin=DATA, end=DATA)

    for x in VALUES:
        sp = STACK + 2048
        saved = 0xded03000
        for register, value in ((reg.UC_PPC_REG_1, sp), (reg.UC_PPC_REG_30, saved),
                                (reg.UC_PPC_REG_3, x), (reg.UC_PPC_REG_LR, RETURN)):
            cpu.reg_write(register, value)
        cpu.emu_start(entries[name], RETURN, count=1000)
        expected = {"cross": 3 * (x + 3) + 93 + 37,
                    "twice": 93 - (3 * (x + 1) + 93 + 2) + 37,
                    "tail_other": 93 - (x + 1), "got_cross": 93 - (x + 1)}[name] & MASK
        if (cpu.reg_read(reg.UC_PPC_REG_3) != expected or
                cpu.reg_read(reg.UC_PPC_REG_1) != sp or cpu.reg_read(reg.UC_PPC_REG_30) != saved or
                cpu.reg_read(reg.UC_PPC_REG_PC) != RETURN):
            raise RuntimeError(f"{target} {path.name} {name}({x}) lazy={lazy}: "
                               f"r3={cpu.reg_read(reg.UC_PPC_REG_3):#x}, expected {expected:#x}")
    if lazy and len(bound) != (2 if name == "twice" else 1):
        raise RuntimeError("unexpected number of first-call bindings")
    return len(VALUES)


def image_storage(source, reference):
    # The single-function LLVM C interface imports original image storage.
    # Supply the two data objects at their offsets and rebind .got2 pointers.
    match = re.search(r"/\* neverd\.image: (0x[\dA-Fa-f]+) \(original loaded image storage\) \*/\n"
                      r"extern uint8_t (\w+)\[(\d+)\];", source.read_text())
    if not match:
        return []
    base, name, size = int(match[1], 16), match[2], int(match[3])
    lines = [".data", ".balign 4", f".globl {name}", f"{name}:"]
    with reference.open("rb") as stream:
        elf = ELFFile(stream)
        by_address = {}
        for section in (".got2", ".data"):
            sec = elf.get_section_by_name(section)
            if sec and base <= sec["sh_addr"] < base + size:
                for i, byte in enumerate(sec.data()):
                    by_address[sec["sh_addr"] + i] = f".byte {byte}"
        table = elf.get_section_by_name(".rela.dyn")
        for r in table.iter_relocations() if table else ():
            at = r["r_offset"]
            if at in by_address:
                if r["r_info_type"] != PPC["R_PPC_RELATIVE"]:
                    raise ValueError("unexpected fixture image pointer")
                for i in range(4):
                    del by_address[at + i]
                by_address[at] = f".long {name} + {r['r_addend'] - base}"
        for address, line in sorted(by_address.items()):
            lines.extend([f".org {name} + {address - base}", line])
    lines.extend([f".org {name} + {size}", '.section .note.GNU-stack,"",@progbits'])
    storage = source.with_suffix(".S")
    storage.write_text("\n".join(lines) + "\n")
    return [storage]


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


def check_rejected(neverd, image, diagnostic):
    for route in ([], ["--llvm"]):
        result = subprocess.run([neverd, "decompile", "--func", "cross", *route, image],
                                capture_output=True, text=True)
        if not result.returncode or diagnostic not in result.stderr + result.stdout:
            raise RuntimeError(f"{image.name} should report {diagnostic!r}")


def check_misplaced_slot(neverd, app, classic):
    raw = bytearray(app.read_bytes())
    with app.open("rb") as stream:
        elf = ELFFile(stream)
        relocs = elf.get_section_by_name(".rela.plt")
        reloc = relocs.get_relocation(0)
        # A BSS entry also owns its second instruction. A secure slot must
        # fall inside the writable pointer table advertised by DT_PLTGOT.
        reloc.entry.r_offset = (reloc["r_offset"] + 4 if classic
                                else elf.get_section_by_name(".data")["sh_addr"])
        entry = elf.structs.Elf_Rela.build(reloc.entry)
        at = relocs["sh_offset"]
        raw[at:at + len(entry)] = entry
    bad = app.with_suffix(".misplaced-slot.elf")
    bad.write_bytes(raw)
    check_rejected(neverd, bad, "invalid PPC32 PLT entry extent")


def qualify(args, work, target, originals_only=False):
    fixtures = Path(__file__).resolve().parent
    source = fixtures / "powerpc-plt.c"
    gnu = subprocess.check_output([args.ld, "--version"], text=True).startswith("GNU ")
    cc = [args.clang, f"--target={target}-linux-gnu", "-ffreestanding", "-nostdlib",
          "-fno-stack-protector", f"-fuse-ld={args.ld}"]
    provider = work / "provider.so"
    subprocess.run([*cc, "-shared", "-fPIC", "-O2", "-DPROVIDER", source,
                    "-Wl,-soname,provider.so", "-o", provider], check=True)
    (work / "string.h").write_text("#include <stddef.h>\nvoid *memcpy(void *, const void *, size_t);\n")
    cases = [("exec", False, False), ("dso", True, False), ("sectionless", True, False)]
    if gnu:
        cases.extend([("classic-exec", False, True), ("classic-dso", True, True),
                      ("classic-sectionless", True, True)])
    original_count = rebuilt_count = 0
    for kind, pic, classic in cases:
        app = work / (kind + ".elf")
        flags = ["-shared", "-fPIC", "-Wl,-Bsymbolic"] if pic else ["-no-pie", "-fno-pic"]
        if gnu:
            flags.append("-Wl,--bss-plt" if classic else "-Wl,--secure-plt")
        if not classic:
            flags.append("-msecure-plt")
        flags += ["-Wl,-u,_GLOBAL_OFFSET_TABLE_", "-I", work]
        names = ["cross", "twice"]
        extra = []
        if not pic or not classic:
            extra = [fixtures / "ppc32-plt.S"]
            if pic:
                extra.insert(0, "-DSMALL_PIC")
                names.append("got_cross")
            else:
                names.append("tail_other")
        strip = ["-Wl,--strip-all"] if kind.endswith("sectionless") else []
        layout = (["-Wl,--section-start=.got=0x50000,--section-start=.plt=0x60000"]
                  if kind == "classic-sectionless" else [])
        subprocess.run([*cc, *flags, *strip, *layout, "-O2", source, *extra, provider, "-Wl,-e,cross",
                        "-o", app], check=True)
        with app.open("rb") as stream:
            elf = ELFFile(stream)
            if (elf.get_section_by_name(".plt")["sh_type"] == "SHT_NOBITS") != classic:
                raise ValueError(f"linker selected the wrong PLT for {kind}")
            got2 = elf.get_section_by_name(".got2")
            mixed = got2 and any(s["p_type"] == "PT_LOAD" and s["p_flags"] & P_FLAGS.PF_X and
                                 s.section_in_segment(got2) for s in elf.iter_segments())
        original = without_sections(app) if strip else app

        def check(path, name, bias, writable_imports=False):
            if writable_imports:
                with path.open("rb") as stream:
                    elf = ELFFile(stream)
                    if not elf.get_section_by_name(".rela.plt"):
                        # Module IR keeps loads through mutable import cells.
                        # A native linker can bind their initial contents eagerly.
                        dyn = elf.get_section_by_name(".rela.dyn")
                        if not dyn:
                            raise ValueError("rebuilt module lacks imported function pointers")
                        syms = elf.get_section(dyn["sh_link"])
                        bound = {syms.get_symbol(r["r_info_sym"]).name for r in dyn.iter_relocations()
                                 if r["r_info_type"] in (PPC["R_PPC_ADDR32"], PPC["R_PPC_GLOB_DAT"])}
                        expected = ({"remote_apply", "remote_other"} if name == "twice" else
                                    {"remote_apply"} if name == "cross" else {"remote_other"})
                        if not expected <= bound:
                            raise ValueError(f"{name} lacks its runtime function-pointer relocation")
                        return execute(path, target, name, provider, bias, False)
            return sum(execute(path, target, name, provider, bias, lazy)
                       for lazy in ((False,) if classic else (False, True)))
        original_count += sum(check(app, name, 0x03000000) for name in names)
        if originals_only:
            continue
        if not pic:
            check_misplaced_slot(args.neverd, app, classic)
        if kind == "classic-dso" and mixed:
            check_rejected(args.neverd, without_sections(app),
                           "PPC32 PIC access needs data bounds in a mixed executable mapping")
        module = work / (kind + ".ll")
        subprocess.run([args.neverd, "lift", "-o", module, original], check=True)
        for name in names:
            for route in ("highc", "llvmc", "llvm"):
                sources = [module]
                if route != "llvm":
                    out = work / f"{kind}-{name}-{route}.c"
                    subprocess.run([args.neverd, "decompile", "--func", name,
                                    *(["--llvm"] if route == "llvmc" else []),
                                    "-o", out, original], check=True)
                    sources = ["-std=gnu17", out]
                    if route == "llvmc":
                        sources += image_storage(out, app)
                for opt in ("-O0", "-O2"):
                    rebuilt = work / f"{kind}-{name}-{route}{opt}.elf"
                    base = ([] if pic else ["-Wl,-Ttext-segment=0x18000000" if gnu
                                             else "-Wl,--image-base=0x18000000"])
                    subprocess.run([*cc, *flags, *base, opt, *sources, provider,
                                    f"-Wl,-e,{name}", "-o", rebuilt], check=True)
                    rebuilt_count += check(rebuilt, name, 0x05000000,
                                           writable_imports=route == "llvm" and pic and not classic)
    print(f"{target} PPC32 PLT: {original_count} original and {rebuilt_count} rebuilt executions matched",
          flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("neverd", "clang", "ld"):
        parser.add_argument("--" + flag, type=Path, required=True)
    args = parser.parse_args()
    for target in ("powerpc", "powerpcle"):
        with tempfile.TemporaryDirectory() as directory:
            qualify(args, Path(directory), target)


if __name__ == "__main__":
    main()
