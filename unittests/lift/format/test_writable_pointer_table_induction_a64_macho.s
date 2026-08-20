.section __TEXT,__text,regular,pure_instructions
.globl _main
.p2align 2
_main:
        stp x19, x20, [sp, #-32]!
        str x21, [sp, #16]
        adrp x19, _table@PAGE
        add x19, x19, _table@PAGEOFF
        mov w20, #0
        mov w21, #0
.Lloop:
        ldr x8, [x19]
        ldr w0, [x8]
        add w21, w21, w0
        add x19, x19, #8
        add w20, w20, #1
        cmp w20, #2
        b.lo .Lloop
        mov w0, w21
        ldr x21, [sp, #16]
        ldp x19, x20, [sp], #32
        ret

.section __DATA,__data
.p2align 3
_table:
        .quad _entry0
        .quad _entry1

.section __TEXT,__const
.p2align 2
_entry0:
        .long 3
_entry1:
        .long 4
