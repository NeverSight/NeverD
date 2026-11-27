    .set noreorder
    .set mips16
    .text
    .space 0x1100
    .section .text.calc,"ax",@progbits
    .globl calc
    .ent calc
calc:
    beqz $4, TAKEN_ARM
    b FALLTHROUGH_ARM
    .end calc
