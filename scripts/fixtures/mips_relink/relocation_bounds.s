    .set noreorder
    .section .text.calc,"ax",@progbits
    .p2align 2
    .globl calc
    .ent calc
calc:
    # Read both ends of the signed GP-relative range.
    .if MIPS16
    li $2, 0
    addiu $2, %gp_rel(GP_LOW)
    move $3, $gp
    addu $2, $3, $2
    lbu $2, 0($2)
    li $4, 0
    addiu $4, %gp_rel(GP_HIGH)
    addu $3, $3, $4
    lbu $3, 0($3)
    li $4, 10
    xor $2, $4
    li $4, 15
    xor $3, $4
    or $2, $3
    .else
    lbu $2, %gp_rel(GP_LOW)($gp)
    lbu $3, %gp_rel(GP_HIGH)($gp)
    xori $2, $2, 10
    xori $3, $3, 15
    or $2, $2, $3
    .endif
    .if R6
    bnezc $2, .Lbad
    .else
    bnez $2, .Lbad
    .endif
    nop

    # The caller checks every bit after signed low-half carry propagation.
    .if MIPS16
    li $2, %hi(CARRY_VALUE)
    sll $2, $2, 16
    addiu $2, %lo(CARRY_VALUE)
    .elseif N64
    lui $2, %highest(CARRY_VALUE)
    daddiu $2, $2, %higher(CARRY_VALUE)
    dsll $2, $2, 16
    daddiu $2, $2, %hi(CARRY_VALUE)
    dsll $2, $2, 16
    daddiu $2, $2, %lo(CARRY_VALUE)
    .else
    lui $2, %hi(CARRY_VALUE)
    addiu $2, $2, %lo(CARRY_VALUE)
    .endif
    .if R6 || MIPS16
    jrc $ra
    .else
    jr $ra
    nop
    .endif
.Lbad:
    li $2, 0
    .if R6 || MIPS16
    jrc $ra
    .else
    jr $ra
    nop
    .endif
    .end calc
