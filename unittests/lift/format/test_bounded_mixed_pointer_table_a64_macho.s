.section __TEXT,__text,regular,pure_instructions
.globl _main
.globl _probe
.globl _target
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
    adrp x23, _handlers@PAGE
    add x23, x23, _handlers@PAGEOFF
    add x23, x23, #8
    mov w25, #0
1:
    cmp w25, #2
    b.cs 2f
    ldr w2, [x23, #-8]
    ldr x24, [x23, #8]
    ldr x8, [x23]
    blr x8
    add x23, x23, #24
    add w25, w25, #1
    b 1b
2:
    ldp x29, x30, [sp], #16
    ret

.p2align 2
_target:
    ret

.section __DATA_CONST,__const
.p2align 3
_handlers:
    .quad 4
    .quad _target
    .quad _name_a
    .quad 5
    .quad _target
    .quad _name_b
    .quad 6
    .quad _name_c
    .quad _name_d

.section __TEXT,__cstring
_name_a:
    .asciz "a"
_name_b:
    .asciz "b"
_name_c:
    .asciz "c"
_name_d:
    .asciz "d"
