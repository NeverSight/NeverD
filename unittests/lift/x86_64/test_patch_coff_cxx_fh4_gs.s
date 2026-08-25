# Windows x64 FH4 fixture with a canonical __GSHandlerCheck_EH4 wrapper.
# The source cookie is deliberately frame-relative; NeverD must not copy that
# offset into generated code and instead asks LLVM to allocate a fresh slot in
# the final machine frame.

        .def    cxx_fh4_gs_fixture_entry;
        .scl    2;
        .type   32;
        .endef
        .globl  cxx_fh4_gs_fixture_entry
        .p2align 4, 0x90
cxx_fh4_gs_fixture_entry:
        subq    $40, %rsp
        callq   cxx_fh4_gs_main
        addq    $40, %rsp
        retq

        .def    cxx_fh4_gs_main;
        .scl    2;
        .type   32;
        .endef
        .globl  cxx_fh4_gs_main
        .p2align 4, 0x90
cxx_fh4_gs_main:
.seh_proc cxx_fh4_gs_main
        subq    $56, %rsp
        .seh_stackalloc 56
        .seh_endprologue
        .seh_handler __GSHandlerCheck_EH4, @unwind, @except
        .seh_handlerdata
        .long   (cxx_fh4_gs_func_info)@IMGREL
        .long   0x23
        .text
        movq    __security_cookie(%rip), %rax
        xorq    %rsp, %rax
        movq    %rax, 32(%rsp)
cxx_fh4_gs_try_begin:
        callq   cxx_fh4_gs_may_throw
cxx_fh4_gs_try_end:
        movq    32(%rsp), %rcx
        xorq    %rsp, %rcx
        callq   __security_check_cookie
        xorl    %eax, %eax
        addq    $56, %rsp
        retq
cxx_fh4_gs_catch_body:
        movl    $11, %eax
        retq
.seh_endproc

        .def    cxx_fh4_gs_may_throw;
        .scl    2;
        .type   32;
        .endef
        .globl  cxx_fh4_gs_may_throw
        .p2align 4, 0x90
cxx_fh4_gs_may_throw:
.seh_proc cxx_fh4_gs_may_throw
        retq
.seh_endproc

        .def    __GSHandlerCheck_EH4;
        .scl    2;
        .type   32;
        .endef
        .globl  __GSHandlerCheck_EH4
        .p2align 4, 0x90
__GSHandlerCheck_EH4:
        retq

        .def    __security_check_cookie;
        .scl    2;
        .type   32;
        .endef
        .globl  __security_check_cookie
        .p2align 4, 0x90
__security_check_cookie:
        retq

        .data
        .p2align 3
        .def    __security_cookie;
        .scl    2;
        .type   0;
        .endef
        .globl  __security_cookie
__security_cookie:
        .quad   0x2b992ddfa232

        .section .xdata,"dr"
cxx_fh4_gs_func_info:
        .byte   0x38
        .long   (cxx_fh4_gs_unwind_map)@IMGREL
        .long   (cxx_fh4_gs_try_map)@IMGREL
        .long   (cxx_fh4_gs_ip_map)@IMGREL

cxx_fh4_gs_unwind_map:
        .byte   0x04
        .byte   0x00
        .byte   0x00

cxx_fh4_gs_try_map:
        .byte   0x02
cxx_fh4_gs_try_row:
        .byte   0x00
        .byte   0x00
        .byte   0x02
        .long   (cxx_fh4_gs_handler_map)@IMGREL

cxx_fh4_gs_handler_map:
        .byte   0x02
cxx_fh4_gs_handler_row:
        .byte   0x01
        .byte   0x80
        .long   (cxx_fh4_gs_catch_body)@IMGREL

cxx_fh4_gs_ip_map:
        .byte   0x06
        .byte   0x00
        .byte   0x00
        .byte   (cxx_fh4_gs_try_begin-cxx_fh4_gs_main)*2
        .byte   0x02
        .byte   (cxx_fh4_gs_try_end-cxx_fh4_gs_try_begin)*2
        .byte   0x00
