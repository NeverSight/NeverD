.section __TEXT,__text,regular,pure_instructions
.globl _main
.globl _probe
.p2align 2
_main:
        sub sp, sp, #16
        stp x29, x30, [sp]
        bl _probe
        ldp x29, x30, [sp]
        add sp, sp, #16
        ret

.p2align 2
_probe:
        sub sp, sp, #64
        stp x29, x30, [sp, #48]
        str x22, [sp]
        str x28, [sp, #8]
        adrp x13, _table_a@PAGE
        add x13, x13, _table_a@PAGEOFF
        adrp x12, _table_b@PAGE
        add x12, x12, _table_b@PAGEOFF
        adrp x14, _rhs@PAGE
        add x14, x14, _rhs@PAGEOFF
        adrp x15, _table_a@PAGE
        add x15, x15, _table_a@PAGEOFF
        mov x22, #0
        mov x28, #1
        mov w0, #0
        cmp x14, x15
        sub x11, x14, x15
        str x11, [sp, #16]
        b.ne Ldead
        b Lheader

Lheader:
        cmp w0, #1
        csel x8, x13, x12, eq
        ldr x9, [x8, x22, lsl #3]
        ldr x10, [x8, x28, lsl #3]
        add w0, w0, #1
        cmp w0, #1
        b.eq Lfirst
        b Lsecond

Lfirst:
        b Ljoin

Lsecond:
        cmp w0, #2
        b.eq Lreset
        b Ljoin

Lreset:
        adrp x13, _table_a@PAGE
        add x13, x13, _table_a@PAGEOFF
        b Ljoin

Ljoin:
        cmp w0, #3
        b.eq Lexit
        b Lheader

Ldead:
        ldr x11, [sp, #16]
        cmp x11, #0
        cset w0, ne
        add w0, w0, #6
        b Lexit

Lexit:
        ldr x22, [sp]
        ldr x28, [sp, #8]
        ldp x29, x30, [sp, #48]
        add sp, sp, #64
        ret

.section __TEXT,__const
.p2align 3
_table_a:
        .quad 100
        .quad 84
        .quad 44
        .quad 28
        .quad 48
        .quad 32
        .quad 8
_table_b:
        .quad 136
        .quad 120
        .quad 56
        .quad 40
        .quad 16

.section __TEXT,__cstring
_rhs:
        .asciz "rhs-cache"
