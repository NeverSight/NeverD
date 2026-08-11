# Windows x64 FH3 fixture whose state graph is intentionally limited to the
# verifier-clean native reconstruction closure exercised by NeverD.  The
# personality is a local executable stub so the linked image is self-contained.

        .def    cxx_fixture_entry;
        .scl    2;
        .type   32;
        .endef
        .globl  cxx_fixture_entry
        .p2align 4, 0x90
cxx_fixture_entry:
        callq   cxx_main
        retq

        .def    cxx_main;
        .scl    2;
        .type   32;
        .endef
        .globl  cxx_main
        .p2align 4, 0x90
cxx_main:
.seh_proc cxx_main
        .seh_handler __CxxFrameHandler3, @except
        .seh_handlerdata
        .long   (cxx_func_info)@IMGREL
        .text
cxx_try_begin:
        callq   cxx_may_throw
cxx_try_end:
        xorl    %eax, %eax
        retq
cxx_catch_body:
        movl    $7, %eax
        retq
.seh_endproc

        .def    cxx_may_throw;
        .scl    2;
        .type   32;
        .endef
        .globl  cxx_may_throw
        .p2align 4, 0x90
cxx_may_throw:
.seh_proc cxx_may_throw
        retq
.seh_endproc

        .def    __CxxFrameHandler3;
        .scl    2;
        .type   32;
        .endef
        .globl  __CxxFrameHandler3
        .p2align 4, 0x90
__CxxFrameHandler3:
        retq

        .section .xdata,"dr"
        .p2align 2
cxx_func_info:
        .long   0x19930522
        .long   2
        .long   (cxx_unwind_map)@IMGREL
        .long   1
        .long   (cxx_try_map)@IMGREL
        .long   2
        .long   (cxx_ip_map)@IMGREL
        .long   0
        .long   0
        .long   1

cxx_unwind_map:
        .long   -1
        .long   0
        .long   0
        .long   0

cxx_try_map:
        .long   0
        .long   0
        .long   1
        .long   1
        .long   (cxx_handler_map)@IMGREL

cxx_handler_map:
        .long   0x40
        .long   0
        .long   0
        .long   (cxx_catch_body)@IMGREL
        .long   0

cxx_ip_map:
        .long   (cxx_try_begin)@IMGREL
        .long   0
        .long   (cxx_try_end)@IMGREL
        .long   -1
