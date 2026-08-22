.section __TEXT,__text,regular,pure_instructions
.globl _main
.globl _probe
.p2align 2
_main:
    stp x29, x30, [sp, #-16]!
    mov w0, #0
    bl _probe
    mov w0, #0
    ldp x29, x30, [sp], #16
    ret

.p2align 2
_probe:
    stp x29, x30, [sp, #-16]!
    stp x22, x28, [sp, #-16]!
    adrp x13, _table_a@PAGE
    add x13, x13, _table_a@PAGEOFF
    adrp x12, _table_b@PAGE
    add x12, x12, _table_b@PAGEOFF
    mov x22, #0
    mov x28, #1
    mov w20, #0

Lheader:
    cmp w20, #1
    csel x8, x13, x12, eq
    ldr x9, [x8, x22, lsl #3]
    ldr x10, [x8, x28, lsl #3]
    add x22, x22, #1
    add w20, w20, #1
    cmp w20, #1
    b.eq Lfirst
    b Lsecond

Lfirst:
    b Ljoin

Lsecond:
    cmp w20, #2
    b.eq Lreset
    b Ljoin

Lreset:
    adrp x13, _table_a@PAGE
    add x13, x13, _table_a@PAGEOFF
    b Ljoin

Ljoin:
    cmp x22, #3
    csel x28, x22, x28, ne
    cmp x22, #3
    b.ne Lheader

Lexit:
    ldp x22, x28, [sp], #16
    ldp x29, x30, [sp], #16
    mov w0, #7
    ret

.section __TEXT,__const
.p2align 3
_table_a:
    .quad 100
    .quad 84
    .quad 44
    .quad 28
_table_b:
    .quad 136
    .quad 120
    .quad 56
    .quad 40

.section __TEXT,__cstring
_text:
    .asciz "index-cycle"
