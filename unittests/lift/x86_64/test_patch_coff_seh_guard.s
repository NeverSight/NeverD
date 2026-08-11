# Windows x64 SEH fixture with both Control Flow Guard and EH continuation
# metadata.  The protected call is deliberately out-of-line so the lifted IR
# retains a potentially-unwinding operation.

        .def    @feat.00;
        .scl    3;
        .type   0;
        .endef
        .globl  @feat.00
@feat.00 = 0x4800

        .def    seh_fixture_entry;
        .scl    2;
        .type   32;
        .endef
        .globl  seh_fixture_entry
        .p2align 4, 0x90
seh_fixture_entry:
        callq   guarded_main
        retq

        .def    guarded_main;
        .scl    2;
        .type   32;
        .endef
        .globl  guarded_main
        .p2align 4, 0x90
guarded_main:
.seh_proc guarded_main
        .seh_handler __C_specific_handler, @unwind, @except
        .seh_handlerdata
        .long   1
        .long   (guard_begin)@IMGREL
        .long   (guard_end)@IMGREL
        .long   1
        .long   (guard_handler)@IMGREL
        .text
guard_begin:
        callq   may_throw_stub
guard_end:
        xorl    %eax, %eax
        retq
guard_handler:
        movl    $1, %eax
        retq
.seh_endproc

        .def    may_throw_stub;
        .scl    2;
        .type   32;
        .endef
        .globl  may_throw_stub
        .p2align 4, 0x90
may_throw_stub:
        retq

        .def    __C_specific_handler;
        .scl    2;
        .type   32;
        .endef
        .globl  __C_specific_handler
        .p2align 4, 0x90
__C_specific_handler:
        retq

        .section .gfids$y,"dr"
        .symidx guarded_main
        .symidx guard_handler

        .section .gehcont$y,"dr"
        .symidx guard_handler

        .section .rdata,"dr"
        .p2align 3
        .globl  _load_config_used
_load_config_used:
        .long   312
        .fill   124, 1, 0
        .quad   __guard_fids_table
        .quad   __guard_fids_count
        .long   __guard_flags
        .fill   12, 1, 0
        .quad   __guard_iat_table
        .quad   __guard_iat_count
        .quad   __guard_longjmp_table
        .quad   __guard_longjmp_count
        .fill   72, 1, 0
        .quad   __guard_eh_cont_table
        .quad   __guard_eh_cont_count
        .fill   32, 1, 0
