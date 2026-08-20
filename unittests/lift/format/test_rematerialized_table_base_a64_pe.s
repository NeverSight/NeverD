        .text
        .globl start
        .globl probe
        .def start; .scl 2; .type 32; .endef
        .def probe; .scl 2; .type 32; .endef
        .p2align 2
start:
        stp x29, x30, [sp, #-16]!
        mov x29, sp
        mov w0, #0
        mov w1, #0
        bl probe
        ldp x29, x30, [sp], #16
        ret

        .p2align 2
probe:
        sub sp, sp, #48
        stp x29, x30, [sp, #32]
        str x22, [sp]
        str x28, [sp, #8]
        adrp x13, table
        add x13, x13, :lo12:table
        adrp x12, table
        add x12, x12, :lo12:table
        mov x22, #0
        mov x28, #1

.Lheader:
        cmp w1, #0
        csel x8, x13, x12, eq
        ldr x9, [x8, x22, lsl #3]
        ldr x10, [x8, x28, lsl #3]
        add w0, w0, #1
        cmp w1, #0
        b.eq .Lsetbase
        b .Lbypass

.Lsetbase:
        adrp x13, table
        add x13, x13, :lo12:table
        b .Ljoin

.Lbypass:
.Ljoin:
        cmp w0, #1
        b.eq .Lexit
        b .Lheader

.Lexit:
        mov w0, #7
        ldr x22, [sp]
        ldr x28, [sp, #8]
        ldp x29, x30, [sp, #32]
        add sp, sp, #48
        ret

        .section .rdata,"dr"
        .p2align 3
table:
        .quad message
        .quad message
message:
        .asciz "rematerialized-a64-pe"
