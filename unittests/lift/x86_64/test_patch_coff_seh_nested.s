# Windows x64 SEH fixture with two catch-all scopes nested over one protected
# call.  The inner scope's row precedes the outer one's, so a scan meets the
# inner handler first.

        .def    nested_entry;
        .scl    2;
        .type   32;
        .endef
        .globl  nested_entry
        .p2align 4, 0x90
nested_entry:
        callq   nested_main
        retq

        .def    nested_main;
        .scl    2;
        .type   32;
        .endef
        .globl  nested_main
        .p2align 4, 0x90
nested_main:
.seh_proc nested_main
        .seh_handler __C_specific_handler, @unwind, @except
        .seh_handlerdata
        .long   2
        .long   (inner_begin)@IMGREL
        .long   (inner_end)@IMGREL
        .long   1
        .long   (inner_handler)@IMGREL
        .long   (outer_begin)@IMGREL
        .long   (outer_end)@IMGREL
        .long   1
        .long   (outer_handler)@IMGREL
        .text
outer_begin:
        nop
inner_begin:
        callq   nested_may_throw
inner_end:
        nop
outer_end:
        xorl    %eax, %eax
        retq
inner_handler:
        movl    $1, %eax
        retq
outer_handler:
        movl    $2, %eax
        retq
.seh_endproc

        .def    nested_may_throw;
        .scl    2;
        .type   32;
        .endef
        .globl  nested_may_throw
        .p2align 4, 0x90
nested_may_throw:
        retq

        .def    __C_specific_handler;
        .scl    2;
        .type   32;
        .endef
        .globl  __C_specific_handler
        .p2align 4, 0x90
__C_specific_handler:
        retq
