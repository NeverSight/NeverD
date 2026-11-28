"""Cross-object calling and data-access fixtures for static MIPS ELF relinking."""

from pathlib import Path


FIXTURES = Path(__file__).resolve().parent / "fixtures" / "mips_relink"
CALLBACK_COUNT = 4096
SET_THREAD_AREA = {"o32": 4283, "n32": 6246, "n64": 5242}


def linkage_startup(profile, entry, set_thread_pointer=""):
    return ((FIXTURES / "start.s").read_text()
            .replace("LOAD_ADDRESS", "dla" if profile.abi == "n64" else "la")
            .replace("SET_THREAD_POINTER", set_thread_pointer).replace("ENTRY_FUNCTION", entry)
            .replace("EXIT_NR", str(profile.exit_nr))
            .replace("CALL_REGISTER", "jalrc $25" if profile.release == 6 else "jalr $25\n    nop"))


def build_linkage_objects(args, build, profile, run):
    pic = profile.linkage not in ("small-data", "relocation-bounds", "branch-slots")
    soft = profile.soft_float
    n64 = profile.abi == "n64"
    target = [f"-mcpu={profile.cpu}", f"-target-abi={profile.abi}", "-filetype=obj"]
    attrs = ([] if pic else ["+noabicalls"]) + (["+soft-float"] if soft else [])
    if profile.linkage == "dsp":
        attrs.append("+dsp")
    elif profile.linkage == "msa":
        # P5600 uses NaN2008, while QEMU's 64-bit Loongson uses legacy NaNs.
        attrs.extend(("+msa", "+fp64", "+nan2008" if profile.abi == "o32" else "-nan2008"))
    load_address = "dla" if n64 else "la"
    set_tp = ""
    if profile.tls_model and not profile.tls_blocks:
        # Linux MIPS uses a biased thread pointer. This freestanding single-thread
        # program uses its own initialized TLS image as the first thread's storage.
        set_thread_area = SET_THREAD_AREA[profile.abi]
        add = "daddiu" if n64 else "addiu"
        set_tp = (f"{load_address} $4, __tls_image\n    {add} $4, $4, 0x7000\n"
                  f"    addiu $2, $zero, {set_thread_area}\n    syscall\n"
                  "    beq $7, $zero, 1f\n    nop\n    addiu $4, $zero, 97\n"
                  f"    addiu $2, $zero, {profile.exit_nr}\n    syscall\n1:")
    entry = {"large-segments": "check_large_segments", "callback-table": "check_callback_table"}.get(
        profile.linkage, "run_calls")
    start = linkage_startup(profile, entry, set_tp)
    if profile.tls_blocks:
        start += ((FIXTURES / "tls_blocks.s").read_text()
                  .replace("ADD_POINTER", "daddiu" if n64 else "addiu")
                  .replace("SET_THREAD_AREA_NR", str(SET_THREAD_AREA[profile.abi]))
                  .replace("RETURN_TO_CALLER", "jrc $31" if profile.release == 6 else "jr $31\n    nop"))
    run([args.mc, *target, f"-triple={profile.triple}", *(["-mattr=" + ",".join(attrs)] if attrs else []),
         "-o", build / "start.o", "-"], source=start)

    def compile_object(name, source, features):
        run([args.llc, *target, f"-mtriple={profile.triple}",
             "-relocation-model=" + ("pic" if pic else "static"),
             "-float-abi=" + ("soft" if soft else "hard"),
             *(["-mattr=" + ",".join(features)] if features else []),
             *([] if pic else ["-mgpopt"]), "-o", build / name, "-"], source=source)

    if profile.linkage == "branch-slots":
        compact = profile.release == 6
        driver = (FIXTURES / "branch_slots.ll").read_text()
        # With a delay slot, adds of 1 and 4 run even on the taken paths.
        compile_object("caller.o", driver.replace("ZERO_INCREMENT", "12" if compact else "13")
                       .replace("ONE_INCREMENT", "3" if compact else "7"), attrs)
        source = (FIXTURES / "branch_slots.s").read_text()
        features = attrs + (["+micromips"] if profile.isa == "micromips" else [])
        for obj, base in (("calc.o", 8), ("updated.o", 31)):
            run([args.mc, *target, f"-triple={profile.triple}", "-mattr=" + ",".join(features),
                 f"-defsym=COMPACT={int(compact)}", f"-defsym=BASE={base}",
                 "-o", build / obj, "-"], source=source)
        return ["start.o", "caller.o", "calc.o"]
    if profile.linkage == "relocation-bounds":
        driver = (FIXTURES / "relocation_bounds.ll").read_text()
        before, after = ((0x12347fff7fff7fff, 0x12347fff7fff8001) if n64 else (0x12347fff, 0x12348001))
        compile_object("caller.o", driver.replace("WORD", "i64" if n64 else "i32")
                       .replace("BEFORE", str(before)).replace("AFTER", str(after)), attrs)
        source = (FIXTURES / "relocation_bounds.s").read_text()
        features = attrs + ([] if profile.isa == "classic" else ["+" + profile.isa])
        variants = (("calc.o", "carry_anchor-1", "gp_low", "gp_high"),
                    ("updated.o", "carry_anchor+1", "gp_low", "gp_high"),
                    ("below.o", "carry_anchor+1", "gp_low-1", "gp_high"),
                    ("above.o", "carry_anchor+1", "gp_low", "gp_high+1"))
        for obj, value, low, high in variants:
            run([args.mc, *target, f"-triple={profile.triple}", "-mattr=" + ",".join(features),
                 f"-defsym=MIPS16={int(profile.isa == 'mips16')}", f"-defsym=N64={int(n64)}",
                 f"-defsym=R6={int(profile.release == 6)}",
                 "-o", build / obj, "-"], source=source.replace("CARRY_VALUE", value)
                 .replace("GP_LOW", low).replace("GP_HIGH", high))
        return ["start.o", "caller.o", "calc.o"]
    if profile.linkage == "init-arrays":
        compile_object("caller.o", (FIXTURES / "init_arrays.ll").read_text(), attrs)
        calc = (FIXTURES / "init_arrays_calc.ll").read_text()
        compile_object("calc.o", calc.replace("RESULT", "41"), attrs)
        compile_object("updated.o", calc.replace("RESULT", "73"), attrs)
        return ["start.o", "caller.o", "calc.o"]
    if profile.linkage == "far-branches":
        compile_object("caller.o", (FIXTURES / "branch_caller.ll").read_text(), attrs)
        branches = (FIXTURES / "far_branches.s").read_text()
        for obj, first, second in (("calc.o", "left", "right"), ("updated.o", "right", "left")):
            source = branches.replace("TAKEN_ARM", first).replace("FALLTHROUGH_ARM", second)
            run([args.mc, *target, f"-triple={profile.triple}", "-mattr=+mips16,+soft-float",
                 "-o", build / obj, "-"], source=source)
        # calc.o also supplies the gap before the two branch targets in caller.o.
        return ["start.o", "calc.o", "caller.o"]
    storage = f"thread_local({profile.tls_model})" if profile.tls_model else ""
    if profile.tls_blocks:
        driver = (FIXTURES / "tls_blocks.ll").read_text().replace("TLS_STORAGE", storage)
        calc = ((FIXTURES / "tls_blocks_calc.ll").read_text().replace("TLS_STORAGE", storage)
                .replace("POINTER_INT", "i64" if n64 else "i32"))
        compile_object("caller.o", driver, attrs)
        features = attrs + ([] if profile.isa == "classic" else ["+" + profile.isa])
        compile_object("calc.o", calc.replace("ADJUST", "1"), features)
        compile_object("updated.o", calc.replace("ADJUST", "7"), features)
        return ["start.o", "caller.o", "calc.o"]
    driver = (FIXTURES / "caller.ll").read_text().replace("SEED_STORAGE", storage)
    if profile.linkage == "large-segments":
        driver += "\n" + (FIXTURES / "large_segments.ll").read_text()
    elif profile.linkage == "callback-table":
        template = (FIXTURES / "callback_table.ll").read_text()
        driver += "\n" + template.replace("COUNT", str(CALLBACK_COUNT)).replace(
            "ENTRIES", ", ".join(["ptr @calc"] * CALLBACK_COUNT))
    calc = (FIXTURES / "calc.ll").read_text().replace("SEED_STORAGE", storage)
    fp_sum = profile.linkage == "pic-hard" and profile.isa != "mips16"
    # Independent full-width results for the two argument lists in caller.ll.
    # After the helper returns, the adjustment increases from three to eleven.
    expected = ((76564513759300293, 8070767403129135745) if fp_sum else
                (-9153562922536531258, 8073019202942820996))
    for index, value in enumerate(expected, 1):
        driver = driver.replace(f"OLD_{index}", str(value)).replace(f"NEW_{index}", str(value + 8))
    x_bits = ("  %fp_sum = fadd double %x, %y\n  %xbits = bitcast double %fp_sum to i64" if fp_sum
              else "  %xbits = bitcast double %x to i64")
    declarations, operations = "", "  %result = add i64 %mixed, ADJUST"
    if profile.linkage in ("dsp", "msa"):
        adjustment = (FIXTURES / f"{profile.linkage}.ll").read_text()
        declarations, operations = adjustment.split("; body\n")
    calc = (calc.replace("X_BITS", x_bits).replace("ASE_DECLARATIONS", declarations.rstrip())
            .replace("ADJUST_RESULT", operations.rstrip()))
    compile_object("caller.o", driver, attrs)
    callee_attrs = attrs + ([] if profile.isa == "classic" else ["+" + profile.isa])
    if not pic and profile.isa == "micromips" and profile.release < 6:
        # JALS has a two-byte delay slot and cannot be exchanged for JALX.
        # Use the compiler's interworking register call for an external callee.
        callee_attrs.append("+long-calls")
    compile_object("calc.o", calc.replace("ADJUST", "3"), callee_attrs)
    compile_object("updated.o", calc.replace("ADJUST", "11"), callee_attrs)
    return ["start.o", "caller.o", "calc.o"]


def verify_large_segments(image, profile, symtab):
    for name, size, storage in (("payload", 16 << 20, "SHT_PROGBITS"),
                                ("arena", 64 << 20, "SHT_NOBITS")):
        symbols = symtab.get_symbol_by_name(name) or []
        if len(symbols) != 1 or not isinstance(symbols[0]["st_shndx"], int):
            raise ValueError(f"{profile.name}: expected one defined {name} object")
        symbol = symbols[0]
        section = image.get_section(symbol["st_shndx"])
        begin = symbol["st_value"]
        end = begin + size
        if (symbol["st_info"]["type"] != "STT_OBJECT" or symbol["st_size"] != size
                or section["sh_type"] != storage
                or not section["sh_addr"] <= begin < end <= section["sh_addr"] + section["sh_size"]):
            raise ValueError(f"{profile.name}: incorrect storage for {name}")
        loads = [ph for ph in image.iter_segments() if ph["p_type"] == "PT_LOAD"
                 and ph["p_vaddr"] <= begin and end <= ph["p_vaddr"] + ph["p_memsz"]]
        if len(loads) != 1:
            raise ValueError(f"{profile.name}: expected one load segment for {name}")
        segment = loads[0]
        file_end = segment["p_vaddr"] + segment["p_filesz"]
        if (not segment["p_flags"] & 4
                or storage == "SHT_PROGBITS" and (end > file_end or section["sh_flags"] & 1)
                or storage == "SHT_NOBITS" and (begin < file_end or not segment["p_flags"] & 2)):
            raise ValueError(f"{profile.name}: incorrect file extent or permissions for {name}")


def inspect_linkage(image, profile, function, text):
    """Verify the actual callable pointer and floating-point ABI in each ET_EXEC."""
    symtab = image.get_section_by_name(".symtab")
    if profile.c_pic:
        width = image.elfclass // 8
        order = "little" if profile.little else "big"
        got = image.get_section_by_name(".got")
        if not image["e_flags"] & 4 or got is None or got["sh_size"] % width:  # EF_MIPS_CPIC
            raise ValueError(f"{profile.name}: missing MIPS PIC calling convention or invalid GOT")
        data = got.data()
        entries = {int.from_bytes(data[i:i + width], order) for i in range(0, len(data), width)}
        # The exit shim is classic for MIPS16 and matches the caller on microMIPS.
        for name, compressed in (("calc", profile.isa != "classic"),
                                 ("finish", profile.isa == "micromips"), ("weights", False)):
            symbols = symtab.get_symbol_by_name(name) or []
            if len(symbols) != 1 or (symbols[0]["st_value"] | compressed) not in entries:
                raise ValueError(f"{profile.name}: missing linked GOT address for {name}")
        return
    if profile.linkage == "init-arrays":
        width = image.elfclass // 8
        order = "little" if profile.little else "big"
        for name, hooks in (("preinit_array", ("preinit",)),
                            ("init_array", ("first", "calc", "last")),
                            ("fini_array", ("finish_last", "finish_first"))):
            section = image.get_section_by_name("." + name)
            expected = bytearray()
            for hook in hooks:
                symbols = symtab.get_symbol_by_name(hook) or []
                if len(symbols) != 1 or symbols[0]["st_info"]["type"] != "STT_FUNC":
                    raise ValueError(f"{profile.name}: expected one function named {hook}")
                expected.extend(symbols[0]["st_value"].to_bytes(width, order))
            if (section is None or section["sh_type"] != "SHT_" + name.upper()
                    or section["sh_addr"] % width or section.data() != expected):
                raise ValueError(f"{profile.name}: incorrect {name} function pointers")
        return
    if profile.linkage == "large-segments":
        verify_large_segments(image, profile, symtab)
    if profile.linkage == "relocation-bounds":
        gp, = symtab.get_symbol_by_name("_gp")
        first, = symtab.get_symbol_by_name("gp_low")
        last, = symtab.get_symbol_by_name("gp_high")
        if (first["st_value"] - gp["st_value"], last["st_value"] - gp["st_value"]) != (-32768, 32767):
            raise ValueError(f"{profile.name}: GP-relative bytes moved off the signed 16-bit endpoints")
    if profile.linkage == "callback-table":
        table, = symtab.get_symbol_by_name("callback_table")
        width = image.elfclass // 8
        payload = function["st_value"].to_bytes(width, "little" if profile.little else "big") * CALLBACK_COUNT
        section = image.get_section(table["st_shndx"])
        offset = table["st_value"] - section["sh_addr"]
        if (table["st_info"]["type"] != "STT_OBJECT" or table["st_size"] != len(payload)
                or section["sh_type"] != "SHT_PROGBITS" or offset < 0
                or section.data()[offset:offset + len(payload)] != payload):
            raise ValueError(f"{profile.name}: incorrect linked callback table")
    callback, = symtab.get_symbol_by_name("callback")
    data = image.get_section(callback["st_shndx"])
    offset = callback["st_value"] - data["sh_addr"]
    size = 8 if profile.abi == "n64" else 4
    if (callback["st_info"]["type"] != "STT_OBJECT" or callback["st_size"] != size
            or not 0 <= offset <= data["sh_size"] - size):
        raise ValueError(f"{profile.name}: invalid callback storage")
    pointer = int.from_bytes(data.data()[offset:offset + size], "little" if profile.little else "big")
    expected = function["st_value"] | (profile.isa != "classic")
    if profile.isa == "mips16" and not profile.soft_float:
        # The classic entry stub moves the two doubles into the MIPS16 calling convention.
        stub, = symtab.get_symbol_by_name("__fn_stub_calc")
        expected = stub["st_value"]
    if pointer != expected:
        raise ValueError(f"{profile.name}: callback contains {pointer:#x}, expected {expected:#x}")
    fp_abi = image.get_section_by_name(".MIPS.abiflags").data()[7]
    expected_abi = 6 if profile.abi == "o32" and (profile.release == 6 or profile.linkage == "msa") else 1
    if fp_abi != (3 if profile.soft_float else expected_abi):
        raise ValueError(f"{profile.name}: unexpected floating-point ABI {fp_abi}")
    if profile.linkage == "far-branches":
        if function["st_size"] != 8:
            raise ValueError(f"{profile.name}: expected two extended branch instructions")
        for name in ("left", "right"):
            target, = symtab.get_symbol_by_name(name)
            distance = target["st_value"] - function["st_value"]
            if (distance - 8 if text.name == ".text" else -distance) <= 2048:
                raise ValueError(f"{profile.name}: {name} is within short branch range or in the wrong direction")
    if profile.tls_model:
        tls, = (s for s in image.iter_segments() if s["p_type"] == "PT_TLS")
        if profile.tls_blocks:
            seed, = symtab.get_symbol_by_name("seed")
            zeroes, = symtab.get_symbol_by_name("zeroes")
            tdata = image.get_section(seed["st_shndx"])
            tbss = image.get_section(zeroes["st_shndx"])
            # LLD can put the alignment gap inside .tbss. TLS symbols are
            # offsets from the template, including that zero-filled gap.
            if ((tls["p_filesz"], tls["p_memsz"], tls["p_align"]) != (4, 515, 256)
                    or seed["st_info"]["type"] != "STT_TLS" or zeroes["st_info"]["type"] != "STT_TLS"
                    or (seed["st_value"], seed["st_size"], tdata.name) != (0, 4, ".tdata")
                    or (zeroes["st_value"], zeroes["st_size"], tbss.name) != (256, 259, ".tbss")
                    or tbss["sh_type"] != "SHT_NOBITS" or tbss["sh_addralign"] != 256
                    or not tls["p_vaddr"] + 4 <= tbss["sh_addr"] <= tls["p_vaddr"] + 256
                    or tbss["sh_addr"] + tbss["sh_size"] != tls["p_vaddr"] + 515):
                raise ValueError(f"{profile.name}: incorrect .tdata/.tbss TLS template")
        elif tls["p_filesz"] < 8:
            raise ValueError(f"{profile.name}: missing initialized TLS storage")
    if profile.linkage in ("dsp", "msa"):
        flags = image.get_section_by_name(".MIPS.abiflags").data()
        ases = int.from_bytes(flags[12:16], "little" if profile.little else "big")
        mask = 1 if profile.linkage == "dsp" else 0x200
        if not ases & mask:
            raise ValueError(f"{profile.name}: missing ASE flag {mask:#x}")
        offset = function["st_value"] - text["sh_addr"]
        code = text.data()[offset:offset + function["st_size"]]
        order = "little" if profile.little else "big"
        words = (int.from_bytes(code[i:i + 4], order) for i in range(0, len(code), 4))
        # Check the rebuilt function actually contains ADDU.QB or ADDV[I].W.
        if profile.linkage == "dsp":
            found = any((word & 0xfc0007ff) == 0x7c000010 for word in words)
        else:
            found = any((word & 0xffe0003f) in (0x78400006, 0x7840000e) for word in words)
        if not found:
            raise ValueError(f"{profile.name}: calc lost its {profile.linkage} arithmetic")


def verify_callback_relocations(image, profile):
    name = ".rodata.callbacks"
    width = image.elfclass // 8
    table = image.get_section_by_name((".rel" if profile.abi == "o32" else ".rela") + name)
    if table is None or table.num_relocations() != CALLBACK_COUNT:
        raise ValueError(f"{profile.name}: incomplete callback relocation table")
    storage = image.get_section(table["sh_info"])
    if storage.name != name or storage.data() != bytes(CALLBACK_COUNT * width):
        raise ValueError(f"{profile.name}: incorrect callback relocation storage or addends")
    symbol = table.get_relocation(0)["r_info_sym"]
    if image.get_section(table["sh_link"]).get_symbol(symbol).name != "calc":
        raise ValueError(f"{profile.name}: callback relocations do not refer to calc")
    kind = 18 if width == 8 else 2  # R_MIPS_64 or R_MIPS_32
    for index, rel in enumerate(table.iter_relocations()):
        if (rel["r_offset"] != index * width or rel["r_info_type"] != kind or rel["r_info_sym"] != symbol
                or any(rel.entry.get(field, 0) for field in ("r_addend", "r_info_type2", "r_info_type3", "r_info_ssym"))):
            raise ValueError(f"{profile.name}: incorrect callback relocation at index {index}")


def verify_linkage_objects(folder, profile, hashes, digest):
    from elftools.elf.elffile import ELFFile
    from elftools.elf.relocation import RelocationSection

    def references(obj, sections):
        path = folder / obj
        if digest(path) != hashes[obj]:
            raise ValueError(f"{path}: SHA-256 mismatch")
        with path.open("rb") as source:
            image = ELFFile(source)
            if profile.c_pic and not image["e_flags"] & 2:  # EF_MIPS_PIC
                raise ValueError(f"{path}: Clang did not produce a PIC object")
            if profile.linkage == "callback-table" and obj == "caller.o":
                verify_callback_relocations(image, profile)
            refs = set()
            packed = set()
            for table in image.iter_sections():
                if not isinstance(table, RelocationSection) or image.get_section(table["sh_info"]).name not in sections:
                    continue
                symbols = image.get_section(table["sh_link"])
                for rel in table.iter_relocations():
                    name = symbols.get_symbol(rel["r_info_sym"]).name
                    refs.add((rel["r_info_type"], name))
                    if rel.entry.get("r_info_type2") or rel.entry.get("r_info_type3"):
                        packed.add((name, rel["r_info_type"], rel["r_info_type2"], rel["r_info_type3"],
                                    rel["r_info_ssym"], rel.entry.get("r_addend")))
            return refs, packed

    if profile.c_pic:
        if set(hashes) != {"caller.o", "calc.o", "updated.o"}:
            raise ValueError(f"{profile.name}: expected the PIC caller and both callees")
        caller, _ = references("caller.o", (".text.start",))
        # CALL16 and GOT16 (o32) or GOT_DISP (n32/n64) for each encoding.
        call, got = {"classic": (11, 9 if profile.abi == "o32" else 19),
                     "micromips": (142, 138 if profile.abi == "o32" else 145),
                     "mips16": (103, 102)}[profile.isa]
        if not {(call, "calc"), (call, "finish")} <= caller:
            raise ValueError(f"{profile.name}: missing PIC C call relocations")
        for obj in ("calc.o", "updated.o"):
            callee, _ = references(obj, (".text.calc",))
            if (got, "weights") not in callee:
                raise ValueError(f"{profile.name}: {obj} is missing the weights GOT relocation")
        return
    if profile.linkage in ("relocation-bounds", "branch-slots"):
        if set(hashes) != {"caller.o", "calc.o", "updated.o"}:
            raise ValueError(f"{profile.name}: expected the indirect caller and both callees")
        caller, _ = references("caller.o", (".rodata.callback",))
        pointer = 18 if profile.abi == "n64" else 2  # R_MIPS_64 or R_MIPS_32
        if caller != {(pointer, "calc")}:
            raise ValueError(f"{profile.name}: missing function pointer relocation")
        if profile.linkage == "branch-slots":
            for obj in ("calc.o", "updated.o"):
                refs, _ = references(obj, (".text.calc",))
                if refs:
                    raise ValueError(f"{profile.name}: unexpected relocation in the local branch body {refs}")
            return
        gp, hi, lo = {"classic": (7, 5, 6), "micromips": (136, 134, 135), "mips16": (101, 104, 105)}[profile.isa]
        kinds = [hi, lo]
        if profile.abi == "n64":
            kinds.extend((29, 28) if profile.isa == "classic" else (152, 151))  # HIGHEST, HIGHER
        expected = {(gp, "gp_low"), (gp, "gp_high")} | {(kind, "carry_anchor") for kind in kinds}
        for obj in ("calc.o", "updated.o"):
            actual, _ = references(obj, (".text.calc",))
            if actual != expected:
                raise ValueError(f"{profile.name}: unexpected {obj} relocations {actual ^ expected}")
        return
    if set(hashes) != {"caller.o", "calc.o"}:
        raise ValueError(f"{profile.name}: expected both original object files")
    if profile.linkage == "init-arrays":
        pointer = 18 if profile.abi == "n64" else 2  # R_MIPS_64 or R_MIPS_32
        caller, _ = references("caller.o", (".preinit_array", ".init_array.101", ".init_array.300",
                                            ".fini_array.101", ".fini_array.200"))
        callee, _ = references("calc.o", (".init_array.200",))
        hooks = {"preinit", "first", "last", "finish_first", "finish_last"}
        if caller != {(pointer, hook) for hook in hooks} or callee != {(pointer, "calc")}:
            raise ValueError(f"{profile.name}: missing typed constructor or destructor relocations")
        return
    caller, caller_packed = references("caller.o", (".text", ".data", ".sdata"))
    callee, callee_packed = references("calc.o", (".text.calc",))
    if profile.abi == "n64" and profile.linkage in ("pic-hard", "pic-soft"):
        # Each PIC function computes GP from its entry with GPREL16 / SUB / HI16 or LO16.
        gp = {"classic": (7, 24, 5, 6), "micromips": (136, 150, 134, 135)}
        for packed, names, isa in ((caller_packed, ("helper", "run_calls"), "classic"),
                                   (callee_packed, ("calc",), profile.isa)):
            gprel, sub, hi, lo = gp[isa]
            needed = {(name, gprel, sub, half, 0, 0) for name in names for half in (hi, lo)}
            if not needed <= packed:
                raise ValueError(f"{profile.name}: missing packed GP relocations {needed - packed}")
    # Primary relocation numbers are common to REL, RELA and packed MIPS64 RELA.
    required = {(18 if profile.abi == "n64" else 2, "calc")}  # R_MIPS_64 or R_MIPS_32
    if profile.linkage == "far-branches":
        branches = {(113, name) for name in ("left", "right")}  # R_MIPS16_PC16_S1
        if not required <= caller or callee != branches:
            raise ValueError(f"{profile.name}: incorrect callback or external MIPS16 branches")
        return
    if profile.tls_model == "initialexec":
        codes = ({"classic": 46, "micromips": 166, "mips16": 110}[profile.isa],)
    elif profile.tls_model:
        high = {"classic": 49, "micromips": 169, "mips16": 111}[profile.isa]
        codes = (high, high + 1)
    if profile.tls_blocks:
        required.add((11, "set_block"))  # R_MIPS_CALL16 in the classic driver
        accesses = {(code, symbol) for code in codes for symbol in ("seed", "zeroes")}
        if not required <= caller or not accesses <= callee:
            raise ValueError(f"{profile.name}: missing caller {required - caller}, callee {accesses - callee}")
        return
    if profile.linkage == "small-data":
        required.update(((7, "seed"), (7, "visits")))  # R_MIPS_GPREL16
        has_call = any(symbol == "helper" for _, symbol in callee)
    else:
        got = 9 if profile.abi == "o32" else 19  # R_MIPS_GOT16 or R_MIPS_GOT_DISP
        required.add((got, "visits"))
        if profile.linkage == "tls-ie":
            required.add((46, "seed"))  # R_MIPS_TLS_GOTTPREL
        elif profile.linkage == "tls":
            required.update(((49, "seed"), (50, "seed")))  # R_MIPS_TLS_TPREL_HI16/LO16
        else:
            required.add((got, "seed"))
        call16 = {"classic": 11, "mips16": 103, "micromips": 142}[profile.isa]
        has_call = (call16, "helper") in callee
    if profile.tls_model:
        seed_refs = {(code, "seed") for code in codes}
    elif profile.linkage == "small-data":
        gprel = {"classic": 7, "micromips": 136, "mips16": 101}[profile.isa]
        seed_refs = {(gprel, "seed")}
    else:
        got = {"classic": 9 if profile.abi == "o32" else 19,
               "micromips": 138 if profile.abi == "o32" else 145, "mips16": 102}[profile.isa]
        seed_refs = {(got, "seed")}
    if not required <= caller or not seed_refs <= callee or not has_call:
        raise ValueError(f"{profile.name}: missing caller {required - caller}, "
                         f"callee {seed_refs - callee}, helper call present={has_call}")
