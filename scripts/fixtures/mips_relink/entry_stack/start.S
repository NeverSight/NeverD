    .set noreorder
    .section .text.start,"ax",@progbits
    .globl _start
    .ent _start
_start:
    move $4, $sp
#if __mips == 64
    daddiu $sp, $sp, -32
#else
    addiu $sp, $sp, -32
#endif
    jal check_start
    nop
    move $4, $2
    addiu $2, $zero, EXIT_NR
    syscall
    .end _start
