// AArch64 Windows FH3 fixture constrained to NeverD's bounded native
// reconstruction contract.  The personality is an executable local stub so
// the linked PE remains self-contained.

        .text

        .def    a64_cxx_fh3_fixture_entry;
        .scl    2;
        .type   32;
        .endef
        .globl  a64_cxx_fh3_fixture_entry
        .p2align 2
a64_cxx_fh3_fixture_entry:
        b       a64_cxx_fh3_main

        .def    a64_cxx_fh3_main;
        .scl    2;
        .type   32;
        .endef
        .globl  a64_cxx_fh3_main
        .p2align 2
        .seh_proc a64_cxx_fh3_main
a64_cxx_fh3_main:
        stp     x29, x30, [sp, #-16]!
        .seh_save_fplr_x 16
        .seh_endprologue
a64_cxx_fh3_try_begin:
        bl      a64_cxx_fh3_may_throw
a64_cxx_fh3_try_end:
        mov     w0, #0
        b       a64_cxx_fh3_return
a64_cxx_fh3_typed_catch:
        mov     w0, #5
        b       a64_cxx_fh3_return
a64_cxx_fh3_catch_all:
        mov     w0, #7
a64_cxx_fh3_return:
        .seh_startepilogue
        ldp     x29, x30, [sp], #16
        .seh_save_fplr_x 16
        .seh_endepilogue
        ret
        .seh_handler __CxxFrameHandler3, @unwind, @except
        .seh_handlerdata
        .long   (a64_cxx_fh3_func_info)@IMGREL
        .text
        .seh_endproc

        .def    a64_cxx_fh3_may_throw;
        .scl    2;
        .type   32;
        .endef
        .globl  a64_cxx_fh3_may_throw
        .p2align 2
a64_cxx_fh3_may_throw:
        ret

        .def    __CxxFrameHandler3;
        .scl    2;
        .type   32;
        .endef
        .globl  __CxxFrameHandler3
        .p2align 2
__CxxFrameHandler3:
        ret

        .section .rdata,"dr"
        .p2align 3
a64_cxx_fh3_type_descriptor:
        .quad   0
        .quad   0
        .asciz  ".H"

        .section .xdata,"dr"
        .p2align 2
a64_cxx_fh3_func_info:
        .long   0x19930522
        .long   2
        .long   (a64_cxx_fh3_unwind_map)@IMGREL
        .long   1
        .long   (a64_cxx_fh3_try_map)@IMGREL
        .long   4
        .long   (a64_cxx_fh3_ip_map)@IMGREL
        .long   0
        .long   0
        .long   1

a64_cxx_fh3_unwind_map:
        .long   -1
        .long   0
        .long   0
        .long   0

a64_cxx_fh3_try_map:
        .long   0
        .long   0
        .long   1
        .long   2
        .long   (a64_cxx_fh3_handler_map)@IMGREL

a64_cxx_fh3_handler_map:
        .long   0x40
        .long   (a64_cxx_fh3_type_descriptor)@IMGREL
        .long   0
        .long   (a64_cxx_fh3_typed_catch)@IMGREL
        .long   0
        .long   0x40
        .long   0
        .long   0
        .long   (a64_cxx_fh3_catch_all)@IMGREL
        .long   0

a64_cxx_fh3_ip_map:
        .long   (a64_cxx_fh3_try_begin)@IMGREL
        .long   0
        .long   (a64_cxx_fh3_try_end)@IMGREL
        .long   -1
        .long   (a64_cxx_fh3_typed_catch)@IMGREL
        .long   1
        .long   (a64_cxx_fh3_catch_all)@IMGREL
        .long   1
