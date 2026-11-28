// A machine-outlined sequence reads its caller's frame: it sets up no frame
// of its own, so x29 is still the caller's frame pointer, and [x29, #-8] is
// a slot of the caller.  Its FDE gives it exception metadata.
        .text
        .globl  ocf_outlined
        .p2align 2
        .type   ocf_outlined,%function
ocf_outlined:
        .cfi_startproc
        ldur    w0, [x29, #-8]
        b       ocf_sink
        .cfi_endproc
        .size   ocf_outlined, .-ocf_outlined

        .globl  ocf_sink
        .p2align 2
        .type   ocf_sink,%function
ocf_sink:
        .cfi_startproc
        add     w0, w0, #1
        ret
        .cfi_endproc
        .size   ocf_sink, .-ocf_sink

        .globl  ocf_caller
        .p2align 2
        .type   ocf_caller,%function
ocf_caller:
        .cfi_startproc
        stp     x29, x30, [sp, #-32]!
        .cfi_def_cfa_offset 32
        .cfi_offset w30, -24
        .cfi_offset w29, -32
        add     x29, sp, #32
        .cfi_def_cfa w29, 0
        str     w0, [x29, #-8]
        bl      ocf_outlined
        ldp     x29, x30, [sp], #32
        .cfi_def_cfa wsp, 0
        .cfi_restore w30
        .cfi_restore w29
        ret
        .cfi_endproc
        .size   ocf_caller, .-ocf_caller
        .section .note.GNU-stack,"",@progbits
