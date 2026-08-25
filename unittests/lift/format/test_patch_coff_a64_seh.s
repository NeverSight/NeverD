// AArch64 Windows SEH fixture with one native catch-all scope.  The real
// prologue and epilogue force the assembler to emit unpacked .pdata/.xdata;
// no GS, CFG, filter, finally, or fragment metadata is present.

        .text

        .def    a64_seh_fixture_entry;
        .scl    2;
        .type   32;
        .endef
        .globl  a64_seh_fixture_entry
        .p2align 2
a64_seh_fixture_entry:
        b       a64_seh_main

        .def    a64_seh_main;
        .scl    2;
        .type   32;
        .endef
        .globl  a64_seh_main
        .p2align 2
        .seh_proc a64_seh_main
a64_seh_main:
        stp     x29, x30, [sp, #-16]!
        .seh_save_fplr_x 16
        .seh_endprologue
a64_seh_guard_begin:
        bl      a64_seh_may_throw
a64_seh_guard_end:
        mov     w0, #0
        b       a64_seh_return
a64_seh_handler:
        mov     w0, #1
a64_seh_return:
        .seh_startepilogue
        ldp     x29, x30, [sp], #16
        .seh_save_fplr_x 16
        .seh_endepilogue
        ret
        .seh_handler __C_specific_handler, @except
        .seh_handlerdata
        .long   1
        .long   (a64_seh_guard_begin)@IMGREL
        .long   (a64_seh_guard_end)@IMGREL
        .long   1
        .long   (a64_seh_handler)@IMGREL
        .text
        .seh_endproc

        .def    a64_seh_may_throw;
        .scl    2;
        .type   32;
        .endef
        .globl  a64_seh_may_throw
        .p2align 2
a64_seh_may_throw:
        ret

        .def    __C_specific_handler;
        .scl    2;
        .type   32;
        .endef
        .globl  __C_specific_handler
        .p2align 2
__C_specific_handler:
        ret
