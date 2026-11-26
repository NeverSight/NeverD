# MIPS n32 short function used for the warm launch comparison.
# The loop updates either the low word or the shift count before returning.

    .abicalls
    .set noreorder
    .text
    .globl route_value
    .type route_value,%function
route_value:
    daddiu $12, $zero, 257
    beq $a1, $12, .Lbare
    nop
    beqz $a0, .Lleft
    or $14, $a1, $zero
    ori $12, $zero, 1028
    dins $10, $12, 0, 16
    b .Lhead
    daddiu $13, $zero, 1
.Lleft:
    daddiu $10, $zero, 386
    b .Lhead
    daddiu $13, $zero, 0
.Lbare:
    jr $ra
    nop
.Lhead:
    beqz $14, .Ldone
    daddiu $12, $zero, 1
    beq $14, $12, .Ldone
    andi $12, $14, 1
    beqz $12, .Lcount
    nop
    xori $12, $10, 514
    dsrl $14, $14, 1
    b .Lhead
    dins $10, $12, 0, 16
.Lcount:
    dsrl $14, $14, 1
    b .Lhead
    xori $13, $13, 1
.Ldone:
    andi $v0, $10, 255
    jr $ra
    dsllv $v0, $v0, $13
    .size route_value,.-route_value
