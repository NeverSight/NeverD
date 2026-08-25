// ARM32 Windows SEH fixture with one native catch-all scope.  Every scope
// code field is an image-relative Thumb address, matching the ARMNT language
// table consumed by __C_specific_handler.

        .syntax unified
        .text
        .thumb

        .def    arm32_seh_fixture_entry;
        .scl    2;
        .type   32;
        .endef
        .globl  arm32_seh_fixture_entry
        .p2align 1
        .thumb_func
arm32_seh_fixture_entry:
        b       arm32_seh_main

        .def    arm32_seh_main;
        .scl    2;
        .type   32;
        .endef
        .globl  arm32_seh_main
        .p2align 1
        .thumb_func
        .seh_proc arm32_seh_main
        .seh_handler __C_specific_handler, %unwind, %except
arm32_seh_main:
        push.w  {r11, lr}
        .seh_save_regs_w {r11, lr}
        .seh_endprologue
arm32_seh_guard_begin:
        bl      arm32_seh_may_throw
arm32_seh_guard_end:
        movs    r0, #0
        b       arm32_seh_return
arm32_seh_handler:
        movs    r0, #1
arm32_seh_return:
        .seh_startepilogue
        pop.w   {r11, pc}
        .seh_save_regs_w {r11, lr}
        .seh_endepilogue
        .seh_handlerdata
        .long   1
        .rva    arm32_seh_guard_begin
        .rva    arm32_seh_guard_end
        .long   1
        .rva    arm32_seh_handler
        .text
        .seh_endproc

        .def    arm32_seh_may_throw;
        .scl    2;
        .type   32;
        .endef
        .globl  arm32_seh_may_throw
        .p2align 1
        .thumb_func
arm32_seh_may_throw:
        bx      lr

        .def    __C_specific_handler;
        .scl    2;
        .type   32;
        .endef
        .globl  __C_specific_handler
        .p2align 1
        .thumb_func
__C_specific_handler:
        movs    r0, #0
        bx      lr
