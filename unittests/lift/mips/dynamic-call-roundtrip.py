#!/usr/bin/env python3
"""Compare untyped changing MIPS callbacks with both regenerated C routes.

Requires cross-target Clang/lld, pyelftools and repository-pinned Unicorn.
No debug declarations or retained relocations describe the dynamic target.
"""

import argparse
import hashlib
import io
import random
import re
import subprocess
from itertools import product
from pathlib import Path

from elftools.elf.elffile import ELFFile
from roundtrip import STACK, STOP, emulator, invoke, map_image, mips
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_TLB_VIRTUAL, UcError
from mips_validation_artifacts import MIPSValidationArtifacts


def symbols(path):
    elf = ELFFile(io.BytesIO(path.read_bytes()))
    return {s.name: s for s in elf.get_section_by_name(".symtab").iter_symbols()
            if s.name}


def rebuilt_storage(source, rebuilt_symbols, address, width, fallback):
    # GNU linkers may alias a user's object with a section-boundary symbol.
    # Use the C emitter's original-image storage annotations, not a preferred
    # spelling of that symbol, to mutate the corresponding rebuilt object.
    owners = set()
    for match in re.finditer(
            r"/\* neverd\.image: 0x([0-9a-fA-F]+)(?: \.\. 0x([0-9a-fA-F]+))?[^\n]*\*/\n([^\n]+)",
            source):
        base = int(match[1], 16)
        declaration = match[3].split("=", 1)[0]
        for name in re.findall(r"[A-Za-z_][A-Za-z_0-9]*", declaration):
            symbol = rebuilt_symbols.get(name)
            if symbol is None or symbol["st_shndx"] in ("SHN_UNDEF", "SHN_ABS"):
                continue
            extent = int(match[2], 16) - base if match[2] else symbol["st_size"]
            if base <= address and address + width <= base + min(extent, symbol["st_size"]):
                owners.add(symbol["st_value"] + address - base)
    if len(owners) > 1:
        raise AssertionError(f"ambiguous rebuilt storage for {address:#x}: {owners}")
    return next(iter(owners)) if owners else fallback


def check_import(args, artifacts, command, profile, original_flags, fixed_flags,
                 provider, companions, callbacks, little, values, results, linker, form):
    work = artifacts.work
    name = profile + "-import" + ("" if form == "direct" else "-" + form)
    obj, shared = work / (name + ".o"), work / (name + ".so")
    fixture = "pic-guarded-import.c" if form == "guarded" else "pic-import.S"
    flags = ["-DSAVED_ENTRY"] if form == "saved" else []
    command([args.clang, *original_flags, "-O2", *flags, "-c", Path(__file__).with_name(fixture),
             "-o", obj], name + "-compile")
    command([*linker, "-shared", obj, *companions, "-o", shared], name + "-link")
    artifacts.discard(obj)
    elf = ELFFile(io.BytesIO(shared.read_bytes()))
    flags = 4 if args.linkage == "cpic" else 6
    if elf["e_type"] != "ET_DYN" or elf["e_flags"] & 6 != flags:
        raise AssertionError(f"{name} unexpected shared-object flags")
    dyn = {entry.entry.d_tag: entry.entry.d_val
           for entry in elf.get_section_by_name(".dynamic").iter_tags()}
    syms = list(elf.get_section_by_name(".dynsym").iter_symbols())
    types = ("STT_FUNC", "STT_NOTYPE") if form == "guarded" else ("STT_FUNC",)
    index = next(i for i, sym in enumerate(syms) if sym.name == "dispatch"
                 and sym["st_shndx"] == "SHN_UNDEF" and sym["st_info"]["type"] in types)
    width = elf.elfclass // 8
    slot = dyn["DT_PLTGOT"] + width * (dyn["DT_MIPS_LOCAL_GOTNO"] + index - dyn["DT_MIPS_GOTSYM"])
    bias = 0x100200000 if width == 8 else 0x200000
    order = "little" if little else "big"

    def execute(uc, entry, variant, entry_gp, reads=None):
        if width == 8:
            uc.ctl_set_tlb_mode(UC_TLB_VIRTUAL)
        address = callbacks[variant]
        uc.mem_write(bias + slot, address.to_bytes(width, order))
        observations = []
        def visit(cpu, pc, size, data):
            observations.append((cpu.reg_read(mips.UC_MIPS_REG_A0), cpu.reg_read(mips.UC_MIPS_REG_A1)))
        uc.hook_add(UC_HOOK_CODE, visit, begin=address, end=address)
        for value in values:
            result, _ = invoke(uc, entry, 64, little, [value], initial_registers={25: entry, 28: entry_gp})
            expected = ((value + 37 if variant else value ^ 37) + 1) & 0xffffffffffffffff
            expected_calls = [(value, 37)]
            if form == "guarded" and not value:
                expected, expected_calls = 11, []
            if (result != expected or observations != expected_calls or
                    uc.reg_read(mips.UC_MIPS_REG_GP) != entry_gp or
                    uc.reg_read(mips.UC_MIPS_REG_RA) != STOP or uc.reg_read(mips.UC_MIPS_REG_SP) != STACK):
                raise AssertionError(f"{name} wrong import result, arguments or saved state for {value:#x}")
            observations.clear()
            if reads is not None:
                if reads != [width] * len(expected_calls):
                    raise AssertionError(f"{name} wrong native GOT loads for {value:#x}: {reads}")
                reads.clear()

    for variant in range(2):
        uc, entry = emulator(shared, 64, little, "import_probe", original=provider, load_bias=bias)
        reads = []
        uc.hook_add(UC_HOOK_MEM_READ, lambda uc, access, addr, size, val, data: reads.append(size),
                    begin=bias + slot, end=bias + slot)
        execute(uc, entry, variant, 0x486100, reads)

    for backend in ("highc", "llvmc"):
        source = work / f"{name}-{backend}.c"
        command([args.neverd, "decompile", "--func=import_probe", *(["--llvm"] if backend == "llvmc" else []),
                 shared, "-o", source], name + "-" + backend)
        artifacts.keep_source(source)
        for optimization in (0, 2):
            build = f"{name}-{backend}-O{optimization}"
            obj = work / (build + ".o")
            command([args.clang, *fixed_flags, f"-O{optimization}", "-c", source, "-o", obj], build + "-compile")
            external = {k for k, sym in symbols(obj).items() if sym["st_shndx"] == "SHN_UNDEF"}
            if "dispatch" not in external:
                raise AssertionError(f"{build} lost the ELF function import")
            backing = []
            for symbol in external - {"dispatch"}:
                match = re.fullmatch(r"(?:unk|off)_([0-9A-Fa-f]+)", symbol)
                if not match:
                    raise AssertionError(f"{build} unexpected external {symbol}")
                backing.append(f"--defsym={symbol}={bias + int(match[1], 16)}")
            hashes = []
            for variant, address in enumerate(callbacks):
                rebuilt = work / f"{build}-{variant}.elf"
                command([*linker, "-e", "import_probe", "-Ttext=0x18000", *backing,
                         f"--defsym=dispatch={address}", obj, "-o", rebuilt], f"{build}-link-{variant}")
                uc, entry = emulator(rebuilt, 64, little, "import_probe", original=provider)
                map_image(uc, elf, set(), bias)
                execute(uc, entry, variant, symbols(rebuilt)["_gp"]["st_value"])
                hashes.append(hashlib.sha256(rebuilt.read_bytes()).hexdigest())
                artifacts.discard(rebuilt)
            artifacts.discard(obj)
            results.append({"profile": name, "backend": backend, "optimization": optimization,
                            "cases": 2 * len(values), "load_bias": bias,
                            "original_sha256": hashlib.sha256(shared.read_bytes()).hexdigest(),
                            "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                            "rebuilt_sha256": hashes})
            artifacts.write("results.json", {"complete": False, "passed": results})
    artifacts.discard(shared)


def run(args, artifacts):
    work = artifacts.work
    results = []
    artifacts.write("results.json", {"complete": False, "passed": results})

    def command(argv, name):
        log = work / (name + ".log")
        with artifacts.process(argv, log) as (process, _):
            process.wait(timeout=60)
        if process.returncode:
            raise RuntimeError(f"{name}: {log.read_text()[-4096:]}")

    resource = subprocess.check_output([args.clang, "-print-resource-dir"],
                                       text=True).strip()
    rng = random.Random(0xCA11)
    pic = args.linkage != "fixed"
    abis = ("n32", "64") if pic else ("32", "n32", "64")
    for abi, little, value_bits in product(abis, (False, True), (32, 64)):
        if abi == "32" and value_bits == 64:
            continue  # o32's split doubleword ABI is outside this carrier check.
        value_mask = (1 << value_bits) - 1
        values = [0, 1, 0x7fffffff, 0x80000000, 0xffffffff, 0x12345678,
                  1 << (value_bits - 1), value_mask]
        values += [rng.getrandbits(value_bits) for _ in range(24)]
        bits = 32 if abi == "32" else 64
        # GNU ld defaults to its configured ABI; an n32 object still uses
        # 64-bit registers but needs the distinct ELF32/n32 emulation.
        emulation = ("elf" + ("64" if abi == "64" else "32") +
                     ("l" if little else "b") + "tsmip" +
                     ("n32" if abi == "n32" else ""))
        linker = [args.ld, "-m", emulation]
        profile = f"{abi}-{'le' if little else 'be'}-i{value_bits}"
        if pic:
            profile += "-" + args.linkage
        fixture = work / (profile + ".c")
        second = ("word second_callback(word x, word y);"
                  if args.linkage == "cpic" else
                  "word second_callback(word x, word y) { return x + y; }")
        fixture.write_text(("typedef unsigned " +
                            ("long long " if value_bits == 64 else "") + "word;\n") + """
word first_callback(word x, word y) { return x ^ y; }
""" + second + """
word (*volatile target)(word, word) = first_callback;
word probe(word value) { return target(value, 37) + 1; }
""")
        artifacts.keep_source(fixture)
        arch = ("mips" if bits == 32 else "mips64") + ("el" if little else "")
        environment = {"32": "gnu", "n32": "gnuabin32", "64": "gnuabi64"}[abi]
        flags = [f"--target={arch}-unknown-linux-{environment}", f"-mabi={abi}",
                 f"-march=mips{bits}r2",
                 "-ffreestanding", "-fno-strict-aliasing", "-nostdinc",
                 "-isystem", resource + "/include"]
        obj, original = work / (profile + ".o"), work / (profile + ".elf")
        fixed_flags = [*flags, "-mno-abicalls", "-fno-pic"]
        original_flags = [*flags, "-mabicalls", "-fPIC"] if pic else fixed_flags
        command([args.clang, *original_flags, "-O2", "-c", fixture, "-o", obj], profile + "-compile")
        companions = []
        if args.linkage == "cpic":
            # The n64 C driver forces PIC with -mabicalls. Use a real CPIC
            # leaf that the runtime also exercises as the second callback.
            leaf = work / (profile + "-leaf.S")
            add = "daddu" if value_bits == 64 else "addu"
            leaf.write_text(".abicalls\n.option pic0\n.set noreorder\n.text\n"
                            ".globl second_callback\n.type second_callback,@function\n"
                            f"second_callback:\n  jr $ra\n  {add} $2,$4,$5\n"
                            ".size second_callback,.-second_callback\n")
            artifacts.keep_source(leaf)
            leaf_obj = work / (profile + "-leaf.o")
            command([args.clang, *original_flags, "-c", leaf, "-o", leaf_obj], profile + "-leaf")
            companions.append(leaf_obj)
        command([*linker, "-e", "probe", "-Ttext=0x20000", obj, *companions, "-o", original],
                profile + "-link")
        artifacts.discard(obj)
        elf = ELFFile(io.BytesIO(original.read_bytes()))
        expected_flags = {"fixed": 0, "pic": 6, "cpic": 4}[args.linkage]
        if elf["e_flags"] & 6 != expected_flags:
            raise AssertionError(f"{profile} unexpected ELF flags {elf['e_flags']:#x}")
        original_symbols = symbols(original)
        callback_bias = (1 << 32) if pic and abi == "64" else 0

        def map_callback_aliases(uc):
            if callback_bias:
                # Exercise a real 64-bit indirect target in the guest VA map.
                uc.ctl_set_tlb_mode(UC_TLB_VIRTUAL)
                map_image(uc, elf, set(), callback_bias)

        original_uc, original_entry = emulator(original, bits, little, "probe")
        map_callback_aliases(original_uc)
        slot = original_symbols["target"]["st_value"]
        gp = original_symbols["_gp"]["st_value"]
        byte_order = "little" if little else "big"
        code = original_uc.mem_read(original_entry, original_symbols["probe"]["st_size"])
        # PIC first reads the address of the live callback object from the GOT.
        # The physical base can be a temporary holding the computed GP.
        source_slot = slot
        width = 8 if abi == "64" else 4
        if pic:
            got = elf.get_section_by_name(".got")
            cells = [got["sh_addr"] + i for i in range(0, got.data_size, width)
                     if int.from_bytes(got.data()[i:i + width], byte_order) == slot]
            if len(cells) != 1:
                raise AssertionError(f"{profile} expected one GOT cell for target")
            source_slot = cells[0]
        load_opcode = 0x37 if abi == "64" else 0x23
        for offset in range(0, len(code), 4):
            word = int.from_bytes(code[offset:offset + 4], byte_order)
            displacement = word & 0xffff
            if displacement & 0x8000:
                displacement -= 0x10000
            if (word >> 26 == load_opcode and (pic or (word >> 21) & 31 == 28)
                    and gp + displacement == source_slot):
                break
        else:
            raise AssertionError(f"{profile} has no GP-relative load of target")
        callbacks = [original_symbols[name]["st_value"]
                     for name in ("first_callback", "second_callback")]
        mask = (1 << bits) - 1
        saved = {r: (0x123456789abc0000 + r) & mask
                 for r in (*range(16, 24), 30)}

        def observe(uc, entry, entry_gp, callback_slot, variant, value, use_initializer=False):
            initial = {**saved, 28: entry_gp}
            pointer = callbacks[variant]
            if not use_initializer:
                pointer += callback_bias
                uc.mem_write(callback_slot, pointer.to_bytes(width, byte_order))
            visits = []
            hook = uc.hook_add(UC_HOOK_CODE, lambda uc, pc, size, _: visits.append(pc),
                               begin=pointer, end=pointer)
            # n32/n64 carry unsigned C words sign-extended in their GPRs.
            argument = (value - (1 << 32)
                        if bits == 64 and value_bits == 32 and value & 0x80000000
                        else value)
            try:
                returned, _ = invoke(uc, entry, bits, little, [argument],
                                     initial_registers={**initial, 25: entry})
            except UcError as error:
                raise AssertionError(
                    f"{profile} entry {entry:#x}, callback {variant}, input {value:#x}: "
                    f"{error}, pc={uc.reg_read(mips.UC_MIPS_REG_PC):#x}") from error
            finally:
                uc.hook_del(hook)
            expected = ((value + 37 if variant else value ^ 37) + 1) & value_mask
            if bits == 64 and value_bits == 32 and expected & 0x80000000:
                expected |= 0xffffffff00000000
            if (returned != expected or len(visits) != 1 or
                    uc.reg_read(mips.UC_MIPS_REG_SP) != STACK or
                    uc.reg_read(mips.UC_MIPS_REG_RA) != STOP or
                    any(uc.reg_read(getattr(mips, f"UC_MIPS_REG_{r}")) != v
                        for r, v in initial.items())):
                state = {"returned": returned, "expected": expected, "visits": visits,
                         "sp": uc.reg_read(mips.UC_MIPS_REG_SP),
                         "ra": uc.reg_read(mips.UC_MIPS_REG_RA),
                         "saved": {r: uc.reg_read(getattr(mips, f"UC_MIPS_REG_{r}"))
                                   for r, v in initial.items()
                                   if uc.reg_read(getattr(mips, f"UC_MIPS_REG_{r}")) != v}}
                raise AssertionError(f"callback state mismatch: {profile}, {variant}, {value:#x}: {state}")

        entry_gp = 0x486100 if pic else gp
        reads = []
        read_hook = original_uc.hook_add(
            UC_HOOK_MEM_READ, lambda uc, access, addr, size, val, data: reads.append((addr, size)),
            begin=source_slot, end=source_slot)
        if pic:
            observe(original_uc, original_entry, entry_gp, slot, 0, values[0], use_initializer=True)
        executions = 2 * len(values) + int(pic)
        for variant, value in product(range(2), values):
            observe(original_uc, original_entry, entry_gp, slot, variant, value)
        original_uc.hook_del(read_hook)
        if reads != [(source_slot, width)] * executions:
            raise AssertionError(f"{profile} original did not read the expected slot on every call")
        # Give original-image dependencies independent storage after relinking.
        # A literal pointer to the original callback slot must stop working.
        data_bias = 0x200000 if pic else 0
        for backend in ("highc", "llvmc"):
            source = work / f"{profile}-{backend}.c"
            command([args.neverd, "decompile", "--func=probe",
                     *(["--llvm"] if backend == "llvmc" else []),
                     original, "-o", source], profile + "-" + backend + "-decompile")
            artifacts.keep_source(source)
            for optimization in (0, 2):
                name = f"{profile}-{backend}-O{optimization}"
                obj, rebuilt = work / (name + ".o"), work / (name + ".elf")
                command([args.clang, *fixed_flags, f"-O{optimization}", "-c", source,
                         "-o", obj], name + "-compile")
                bindings = []
                for symbol, record in symbols(obj).items():
                    if record["st_shndx"] != "SHN_UNDEF":
                        continue
                    if symbol in original_symbols:
                        address = original_symbols[symbol]["st_value"]
                    elif match := re.fullmatch(r"(?:unk|off)_([0-9A-Fa-f]+)", symbol):
                        address = int(match[1], 16)
                    else:
                        raise AssertionError(f"unbound source dependency: {symbol}")
                    bindings.append(f"--defsym={symbol}={address + data_bias}")
                # Keep external small-data bindings within signed GP displacement range.
                command([*linker, "-e", "probe", "-Ttext=0x18000", *bindings,
                         obj, "-o", rebuilt], name + "-link")
                uc, entry = emulator(rebuilt, bits, little, "probe", original=original)
                map_callback_aliases(uc)
                rebuilt_symbols = symbols(rebuilt)
                rebuilt_gp = rebuilt_symbols["_gp"]["st_value"]
                rebuilt_slot = rebuilt_storage(
                    source.read_text(), rebuilt_symbols, slot, width,
                    rebuilt_symbols["target"]["st_value"]
                    if "target" in rebuilt_symbols else slot + data_bias)
                if pic:
                    map_image(uc, elf, set(), data_bias)
                    uc.mem_write(source_slot + data_bias, (slot + data_bias).to_bytes(width, byte_order))
                    observe(uc, entry, rebuilt_gp, rebuilt_slot, 0, values[0], use_initializer=True)
                for variant, value in product(range(2), values):
                    observe(uc, entry, rebuilt_gp, rebuilt_slot, variant, value)
                results.append({"profile": profile, "backend": backend,
                                "optimization": optimization, "cases": executions,
                                "original_sha256": hashlib.sha256(original.read_bytes()).hexdigest(),
                                "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                                "rebuilt_sha256": hashlib.sha256(rebuilt.read_bytes()).hexdigest()})
                artifacts.write("results.json", {"complete": False, "passed": results})
                artifacts.discard(obj, rebuilt)
        if pic and value_bits == 64:
            for form in ("direct", "saved", "guarded"):
                check_import(args, artifacts, command, profile, original_flags, fixed_flags,
                             original, companions, callbacks, little, values, results, linker, form)
        artifacts.discard(original, *companions)
    artifacts.write("results.json", {"complete": True, "passed": results})
    print(f"Passed {len(results)} dynamic MIPS source groups ({sum(r['cases'] for r in results)} executions)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "clang", "ld", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--linkage", choices=("fixed", "pic", "cpic"), default="fixed")
    args = parser.parse_args()
    with MIPSValidationArtifacts(args.output, "dynamic-call-roundtrip",
                                 (args.neverd, args.clang, args.ld)) as artifacts:
        run(args, artifacts)


if __name__ == "__main__":
    main()
