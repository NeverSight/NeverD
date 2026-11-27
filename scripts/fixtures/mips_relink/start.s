    .set noreorder
    .option pic0
    .section .bss.stack,"aw",@nobits
    .p2align 4
    .space 4096
stack_top:
    .section .text.start,"ax",@progbits
    .globl _start
    .ent _start
_start:
    LOAD_ADDRESS $sp, stack_top
    LOAD_ADDRESS $gp, _gp
    SET_THREAD_POINTER
    LOAD_ADDRESS $25, run_calls
    CALL_REGISTER
    addu $4, $2, $zero
    addiu $2, $zero, EXIT_NR
    syscall
    .end _start
