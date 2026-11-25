"""Execution helpers for the MIPS ELF test programs."""


def map_image(uc, image, pages, bias=0):
    for segment in image.iter_segments():
        if segment["p_type"] != "PT_LOAD":
            continue
        begin, size = bias + segment["p_vaddr"], segment["p_memsz"]
        for page in range(begin & ~4095, (begin + size + 4095) & ~4095, 4096):
            if page not in pages:
                uc.mem_map(page, 4096)
                pages.add(page)
        uc.mem_write(begin, segment.data())


def run_micromips64(image, release, exit_nr, expected):
    from unicorn import (Uc, UC_ARCH_MIPS, UC_HOOK_INTR, UC_MODE_BIG_ENDIAN,
                         UC_MODE_MIPS64, UC_TLB_VIRTUAL)
    from unicorn import mips_const as mips

    mode = UC_MODE_MIPS64 | (0 if image.little_endian else UC_MODE_BIG_ENDIAN)
    engine = Uc(UC_ARCH_MIPS, mode)
    engine.ctl_set_cpu_model(mips.UC_CPU_MIPS64_MICROMIPS64R2_GENERIC if release == 2
                             else mips.UC_CPU_MIPS64_MICROMIPS64R6_GENERIC)
    engine.ctl_set_tlb_mode(UC_TLB_VIRTUAL)
    # Enable extended addressing for the n64 image linked above 4 GiB.
    status = mips.UC_MIPS_REG_CP0_STATUS
    engine.reg_write(status, engine.reg_read(status) | 0xe0)
    map_image(engine, image, set())
    observed = []

    def interrupt(uc, number, _):
        observed.append((number, uc.reg_read(mips.UC_MIPS_REG_V0), uc.reg_read(mips.UC_MIPS_REG_A0)))
        uc.emu_stop()

    engine.hook_add(UC_HOOK_INTR, interrupt)
    engine.emu_start(image["e_entry"], 0, count=256)
    # The fork reports EXCP_SYSCALL as interrupt 17. Any other exception or
    # exhaustion of the instruction budget fails the program.
    if observed != [(17, exit_nr, expected)]:
        raise ValueError(f"microMIPS64 R{release}: expected exit({expected}) via syscall {exit_nr}, got {observed}")
