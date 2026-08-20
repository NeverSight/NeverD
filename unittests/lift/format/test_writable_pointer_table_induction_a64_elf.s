.text
.globl _start
.globl probe
.type _start, %function
.type probe, %function
.p2align 2
_start:
        bl probe
        mov x8, #93
        svc #0
.Lhalt:
        b .Lhalt
.size _start, .-_start

.p2align 2
probe:
        stp x19, x20, [sp, #-32]!
        str x21, [sp, #16]
        adrp x19, table
        add x19, x19, :lo12:table
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
.size probe, .-probe

.section .data,"aw",%progbits
.p2align 3
table:
        .quad entry0
        .quad entry1

.section .rodata,"a",%progbits
.p2align 2
entry0:
        .long 3
entry1:
        .long 4
