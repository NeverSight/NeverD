# Windows x64 FH4 fixture limited to the exact catch-all reconstruction model.
# The personality is a local executable stub so the linked image is
# self-contained.

        .def    cxx_fh4_fixture_entry;
        .scl    2;
        .type   32;
        .endef
        .globl  cxx_fh4_fixture_entry
        .p2align 4, 0x90
cxx_fh4_fixture_entry:
        callq   cxx_fh4_main
        retq

        .def    cxx_fh4_main;
        .scl    2;
        .type   32;
        .endef
        .globl  cxx_fh4_main
        .p2align 4, 0x90
cxx_fh4_main:
.seh_proc cxx_fh4_main
        .seh_handler __CxxFrameHandler4, @except
        .seh_handlerdata
        .long   (cxx_fh4_func_info)@IMGREL
        .text
        nop
cxx_fh4_try_begin:
        callq   cxx_fh4_may_throw
cxx_fh4_try_end:
        xorl    %eax, %eax
        retq
cxx_fh4_catch_body:
        movl    $7, %eax
        retq
.seh_endproc

        .def    cxx_fh4_may_throw;
        .scl    2;
        .type   32;
        .endef
        .globl  cxx_fh4_may_throw
        .p2align 4, 0x90
cxx_fh4_may_throw:
.seh_proc cxx_fh4_may_throw
        retq
.seh_endproc

        .def    __CxxFrameHandler4;
        .scl    2;
        .type   32;
        .endef
        .globl  __CxxFrameHandler4
        .p2align 4, 0x90
__CxxFrameHandler4:
        retq

        .section .xdata,"dr"
cxx_fh4_func_info:
        .byte   0x38
        .long   (cxx_fh4_unwind_map)@IMGREL
        .long   (cxx_fh4_try_map)@IMGREL
        .long   (cxx_fh4_ip_map)@IMGREL

cxx_fh4_unwind_map:
        .byte   0x04
        .byte   0x00
        .byte   0x00

cxx_fh4_try_map:
        .byte   0x02
cxx_fh4_try_row:
        .byte   0x00
        .byte   0x00
        .byte   0x02
        .long   (cxx_fh4_handler_map)@IMGREL

cxx_fh4_handler_map:
        .byte   0x02
cxx_fh4_handler_row:
        .byte   0x01
        .byte   0x80
        .long   (cxx_fh4_catch_body)@IMGREL

cxx_fh4_ip_map:
        .byte   0x06
        .byte   0x00
        .byte   0x00
        .byte   (cxx_fh4_try_begin-cxx_fh4_main)*2
        .byte   0x02
        .byte   (cxx_fh4_try_end-cxx_fh4_try_begin)*2
        .byte   0x00
