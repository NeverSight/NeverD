    .text
    .globl set_block
    .ent set_block
set_block:
    ADD_POINTER $4, $4, 0x7000
    addiu $2, $zero, SET_THREAD_AREA_NR
    syscall
    RETURN_TO_CALLER
    .end set_block
