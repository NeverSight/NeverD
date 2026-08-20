        .text
        .globl _start
        .globl probe
        .type _start, %function
        .type probe, %function
        .p2align 2
_start:
        stp x29, x30, [sp, #-16]!
        mov x29, sp
        bl probe
        ldp x29, x30, [sp], #16
        mov x8, #93
        svc #0
.Lhalt:
        b .Lhalt
        .size _start, .-_start

        .p2align 2
probe:
        sub sp, sp, #64
        stp x29, x30, [sp, #48]
        str x22, [sp]
        str x28, [sp, #8]
        adrp x13, table_a
        add x13, x13, :lo12:table_a
        adrp x12, table_b
        add x12, x12, :lo12:table_b
        adrp x14, rhs
        add x14, x14, :lo12:rhs
        adrp x15, table_a
        add x15, x15, :lo12:table_a
        mov x22, #0
        mov x28, #1
        mov w0, #0
        cmp x14, x15
        sub x11, x14, x15
        str x11, [sp, #16]
        b.ne .Ldead
        b .Lheader

.Lheader:
        cmp w0, #1
        csel x8, x13, x12, eq
        ldr x9, [x8, x22, lsl #3]
        ldr x10, [x8, x28, lsl #3]
        add w0, w0, #1
        cmp w0, #1
        b.eq .Lfirst
        b .Lsecond

.Lfirst:
        b .Ljoin

.Lsecond:
        cmp w0, #2
        b.eq .Lreset
        b .Ljoin

.Lreset:
        adrp x13, table_a
        add x13, x13, :lo12:table_a
        b .Ljoin

.Ljoin:
        cmp w0, #3
        b.eq .Lexit
        b .Lheader

.Ldead:
        ldr x11, [sp, #16]
        cmp x11, #0
        cset w0, ne
        add w0, w0, #6
        b .Lexit

.Lexit:
        ldr x22, [sp]
        ldr x28, [sp, #8]
        ldp x29, x30, [sp, #48]
        add sp, sp, #64
        ret
        .size probe, .-probe

        .section .rodata,"a",%progbits
        .p2align 3
table_a:
        .quad 100
        .quad 84
        .quad 44
        .quad 28
        .quad 48
        .quad 32
        .quad 8
table_b:
        .quad 136
        .quad 120
        .quad 56
        .quad 40
        .quad 16

        .section .rodata.str1.1,"aMS",%progbits,1
rhs:
        .asciz "rhs-cache"
