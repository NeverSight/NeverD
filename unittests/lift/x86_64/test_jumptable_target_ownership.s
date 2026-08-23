    .text

    .globl jt_target_ownership_rejects_next_function_interior
    .type jt_target_ownership_rejects_next_function_interior,@function
jt_target_ownership_rejects_next_function_interior:
    andl $1, %edi
    movq .Lownership_table(,%rdi,8), %rax
    jmp *%rax
.Lownership_local_case:
    movl $4100, %eax
    ret
    .size jt_target_ownership_rejects_next_function_interior, .-jt_target_ownership_rejects_next_function_interior

    .globl jt_target_ownership_foreign_function
    .type jt_target_ownership_foreign_function,@function
jt_target_ownership_foreign_function:
    nop
.Lownership_foreign_interior:
    movl $4999, %eax
    ret
    .size jt_target_ownership_foreign_function, .-jt_target_ownership_foreign_function

    .section .rodata,"a",@progbits
    .p2align 3
.Lownership_table:
    .quad .Lownership_local_case
    .quad .Lownership_foreign_interior

    .section .note.GNU-stack,"",@progbits
