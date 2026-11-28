"""Execution helpers for the MIPS ELF test programs."""


def map_image(uc, image, pages, bias=0, *, enforce_permissions=False):
    if enforce_permissions:
        from elftools.elf.constants import P_FLAGS
        from unicorn import UC_PROT_EXEC, UC_PROT_READ, UC_PROT_WRITE

    for segment in image.iter_segments():
        if segment["p_type"] != "PT_LOAD" or not segment["p_memsz"]:
            continue
        begin, size = bias + segment["p_vaddr"], segment["p_memsz"]
        first, end = begin & ~4095, (begin + size + 4095) & ~4095
        for page in range(first, end, 4096):
            if page not in pages:
                uc.mem_map(page, 4096)
                pages.add(page)
        uc.mem_write(begin, segment.data())
        if enforce_permissions:
            flags = segment["p_flags"]
            access = ((UC_PROT_READ if flags & P_FLAGS.PF_R else 0) |
                      (UC_PROT_WRITE if flags & P_FLAGS.PF_W else 0) |
                      (UC_PROT_EXEC if flags & P_FLAGS.PF_X else 0))
            uc.mem_protect(first, end - first, access)


def run_micromips64(image, release, exit_nr, expected, *, instruction_limit=256,
                    set_thread_area=None, thread_pointers=None):
    from unicorn import (Uc, UC_ARCH_MIPS, UC_HOOK_INTR, UC_MODE_BIG_ENDIAN,
                         UC_MODE_MIPS64, UC_PROT_READ, UC_PROT_WRITE, UC_TLB_VIRTUAL)
    from unicorn import mips_const as mips

    mode = UC_MODE_MIPS64 | (0 if image.little_endian else UC_MODE_BIG_ENDIAN)
    engine = Uc(UC_ARCH_MIPS, mode)
    engine.ctl_set_cpu_model(mips.UC_CPU_MIPS64_MICROMIPS64R6_GENERIC if release == 6
                             else mips.UC_CPU_MIPS64_MICROMIPS64R2_GENERIC)
    engine.ctl_set_tlb_mode(UC_TLB_VIRTUAL)
    # Enable extended addressing for the n64 image linked above 4 GiB.
    status = mips.UC_MIPS_REG_CP0_STATUS
    value = engine.reg_read(status) | 0xe0
    cpr1_size = image.get_section_by_name(".MIPS.abiflags").data()[5]
    if cpr1_size:
        value |= 1 << 29  # CU1
        if release == 6 or cpr1_size >= 2:
            value |= 1 << 26  # FR=1, 64-bit floating-point registers
        else:
            value &= ~(1 << 26)
    engine.reg_write(status, value)
    map_image(engine, image, set(), enforce_permissions=True)
    # A compiled process entry can spill before calling its first callee.
    # One writable page below 2 GiB serves both n32 and n64. Leave space at
    # the aligned SP for an empty argc/argv, environment and auxiliary vector.
    stack = 0x7fff0000
    engine.mem_map(stack, 4096, UC_PROT_READ | UC_PROT_WRITE)
    engine.reg_write(mips.UC_MIPS_REG_SP, stack + 4096 - 64)
    observed = []
    expected_events = []
    if set_thread_area is not None:
        if thread_pointers is None:
            thread_pointers = (image.get_section_by_name(".tdata")["sh_addr"] + 0x7000,)
        expected_events.extend((17, set_thread_area, pointer) for pointer in thread_pointers)
    expected_events.append((17, exit_nr, expected))

    def interrupt(uc, number, _):
        call, arg = uc.reg_read(mips.UC_MIPS_REG_V0), uc.reg_read(mips.UC_MIPS_REG_A0)
        observed.append((number, call, arg))
        if number == 17 and call == set_thread_area:
            uc.reg_write(mips.UC_MIPS_REG_CP0_USERLOCAL, arg)
            uc.reg_write(mips.UC_MIPS_REG_A3, 0)
            uc.reg_write(mips.UC_MIPS_REG_V0, 0)
            return
        uc.emu_stop()

    engine.hook_add(UC_HOOK_INTR, interrupt)
    engine.emu_start(image["e_entry"], 0, count=instruction_limit)
    # The fork reports EXCP_SYSCALL as interrupt 17. Any other exception or
    # exhaustion of the instruction budget fails the program.
    if observed != expected_events:
        raise ValueError(f"microMIPS64 R{release}: expected {expected_events}, got {observed}")


def check_non_executable_entry(path, release, exit_nr, expected):
    from io import BytesIO
    from elftools.elf.constants import P_FLAGS
    from elftools.elf.elffile import ELFFile
    from unicorn import UcError, UC_ERR_FETCH_PROT

    stream = BytesIO(path.read_bytes())
    image = ELFFile(stream)
    for index, segment in enumerate(image.iter_segments()):
        if segment["p_type"] == "PT_LOAD" and segment["p_flags"] & P_FLAGS.PF_X:
            header = segment.header.copy()
            header["p_flags"] &= ~P_FLAGS.PF_X
            stream.seek(image["e_phoff"] + index * image["e_phentsize"])
            stream.write(image.structs.Elf_Phdr.build(header))
    try:
        run_micromips64(image, release, exit_nr, expected)
    except UcError as error:
        if error.errno != UC_ERR_FETCH_PROT:
            raise
        return
    raise ValueError(f"{path}: ELF entry executed without PF_X")
