    .set noreorder
    .section .text.calc,"ax",@progbits
    .p2align 2
    .globl calc
    .ent calc
calc:
    addiu $2, $zero, BASE
    .if COMPACT
    beqzc $4, .Lsecond
    .else
    beqz $4, .Lsecond
    .endif
    addiu $2, $2, 1
    addiu $2, $2, 2
.Lsecond:
    .if COMPACT
    bnezc $4, .Lreturn
    .else
    bnez $4, .Lreturn
    .endif
    addiu $2, $2, 4
    addiu $2, $2, 8
.Lreturn:
    .if COMPACT
    jrc $ra
    .else
    jr $ra
    nop
    .endif
    .end calc
