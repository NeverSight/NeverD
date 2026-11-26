"""Cross-object calling and data-access fixtures for static MIPS ELF relinking."""

from pathlib import Path


FIXTURES = Path(__file__).resolve().parent / "fixtures" / "mips_relink"
SET_THREAD_AREA = {"o32": 4283, "n32": 6246, "n64": 5242}


def build_linkage_objects(args, build, profile, run):
    pic = profile.linkage != "small-data"
    soft = profile.linkage == "pic-soft"
    tls = profile.linkage == "tls"
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
    if tls:
        # Linux MIPS uses a biased thread pointer. This freestanding single-thread
        # program uses its own initialized TLS image as the first thread's storage.
        set_thread_area = SET_THREAD_AREA[profile.abi]
        add = "daddiu" if n64 else "addiu"
        set_tp = (f"{load_address} $4, __tls_image\n    {add} $4, $4, 0x7000\n"
                  f"    addiu $2, $zero, {set_thread_area}\n    syscall\n"
                  "    beq $7, $zero, 1f\n    nop\n    addiu $4, $zero, 97\n"
                  f"    addiu $2, $zero, {profile.exit_nr}\n    syscall\n1:")
    start = ((FIXTURES / "start.s").read_text().replace("LOAD_ADDRESS", load_address)
             .replace("SET_THREAD_POINTER", set_tp).replace("EXIT_NR", str(profile.exit_nr))
             .replace("CALL_REGISTER", "jalrc $25" if profile.release == 6 else "jalr $25\n    nop"))
    run([args.mc, *target, f"-triple={profile.triple}", *(["-mattr=" + ",".join(attrs)] if attrs else []),
         "-o", build / "start.o", "-"], source=start)

    def compile_object(name, source, features):
        run([args.llc, *target, f"-mtriple={profile.triple}",
             "-relocation-model=" + ("pic" if pic else "static"),
             "-float-abi=" + ("soft" if soft else "hard"),
             *(["-mattr=" + ",".join(features)] if features else []),
             *([] if pic else ["-mgpopt"]), "-o", build / name, "-"], source=source)

    driver = (FIXTURES / "caller.ll").read_text().replace("SEED_STORAGE", "thread_local(localexec)" if tls else "")
    calc = (FIXTURES / "calc.ll").read_text()
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


def inspect_linkage(image, profile, function, text):
    """Verify the actual callable pointer and floating-point ABI in each ET_EXEC."""
    symtab = image.get_section_by_name(".symtab")
    callback, = symtab.get_symbol_by_name("callback")
    data = image.get_section(callback["st_shndx"])
    offset = callback["st_value"] - data["sh_addr"]
    size = 8 if profile.abi == "n64" else 4
    if (callback["st_info"]["type"] != "STT_OBJECT" or callback["st_size"] != size
            or not 0 <= offset <= data["sh_size"] - size):
        raise ValueError(f"{profile.name}: invalid callback storage")
    pointer = int.from_bytes(data.data()[offset:offset + size], "little" if profile.little else "big")
    expected = function["st_value"] | (profile.isa != "classic")
    if profile.isa == "mips16" and profile.linkage != "pic-soft":
        # The classic entry stub moves the two doubles into the MIPS16 calling convention.
        stub, = symtab.get_symbol_by_name("__fn_stub_calc")
        expected = stub["st_value"]
    if pointer != expected:
        raise ValueError(f"{profile.name}: callback contains {pointer:#x}, expected {expected:#x}")
    fp_abi = image.get_section_by_name(".MIPS.abiflags").data()[7]
    expected_abi = 6 if profile.abi == "o32" and (profile.release == 6 or profile.linkage == "msa") else 1
    if fp_abi != (3 if profile.linkage == "pic-soft" else expected_abi):
        raise ValueError(f"{profile.name}: unexpected floating-point ABI {fp_abi}")
    if profile.linkage == "tls":
        tls, = (s for s in image.iter_segments() if s["p_type"] == "PT_TLS")
        if tls["p_filesz"] < 8:
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


def verify_linkage_objects(folder, profile, hashes, digest):
    from elftools.elf.elffile import ELFFile
    from elftools.elf.relocation import RelocationSection

    def references(obj, sections):
        path = folder / obj
        if digest(path) != hashes[obj]:
            raise ValueError(f"{path}: SHA-256 mismatch")
        with path.open("rb") as source:
            image = ELFFile(source)
            refs = set()
            for table in image.iter_sections():
                if not isinstance(table, RelocationSection) or image.get_section(table["sh_info"]).name not in sections:
                    continue
                symbols = image.get_section(table["sh_link"])
                for rel in table.iter_relocations():
                    refs.add((rel["r_info_type"], symbols.get_symbol(rel["r_info_sym"]).name))
            return refs

    if set(hashes) != {"caller.o", "calc.o"}:
        raise ValueError(f"{profile.name}: expected both original object files")
    caller = references("caller.o", (".text", ".data", ".sdata"))
    callee = references("calc.o", (".text.calc",))
    # Primary relocation numbers are common to REL, RELA and packed MIPS64 RELA.
    required = {(18 if profile.abi == "n64" else 2, "calc")}  # R_MIPS_64 or R_MIPS_32
    if profile.linkage == "small-data":
        required.update(((7, "seed"), (7, "visits")))  # R_MIPS_GPREL16
        has_call = any(symbol == "helper" for _, symbol in callee)
    else:
        got = 9 if profile.abi == "o32" else 19  # R_MIPS_GOT16 or R_MIPS_GOT_DISP
        required.add((got, "visits"))
        if profile.linkage == "tls":
            required.update(((49, "seed"), (50, "seed")))  # R_MIPS_TLS_TPREL_HI16/LO16
        else:
            required.add((got, "seed"))
        call16 = {"classic": 11, "mips16": 103, "micromips": 142}[profile.isa]
        has_call = (call16, "helper") in callee
    if not required <= caller or not has_call:
        raise ValueError(f"{profile.name}: missing caller relocations {required - caller} or helper call {callee}")
