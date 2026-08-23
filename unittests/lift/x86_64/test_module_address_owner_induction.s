        .text

// A writable table remains a valid static switch while no code can mutate or
// escape it.  The unrelated induction loops below must not make this owner
// unsafe merely because module address analysis needs to widen their states.
        .globl  module_owner_safe_dispatch
        .type   module_owner_safe_dispatch,@function
module_owner_safe_dispatch:
        andl    $1, %edi
        leaq    .Lmodule_owner_safe_table(%rip), %rax
        jmpq    *(%rax,%rdi,8)
.Lmodule_owner_safe_case0:
        movl    $6100, %eax
        retq
.Lmodule_owner_safe_case1:
        movl    $6101, %eax
        retq
        .size   module_owner_safe_dispatch, .-module_owner_safe_dispatch

// This second writable table has an independent cross-function writer.  The
// writer intentionally advances the destination pointer around a machine
// loop, so widening must retain the table root and record a may-write instead
// of either losing the owner or poisoning every writable table in the module.
        .globl  module_owner_mutated_dispatch
        .type   module_owner_mutated_dispatch,@function
module_owner_mutated_dispatch:
        cmpl    $19, %edi
        ja      .Lmodule_owner_mutated_default
        movl    %edi, %edi
        leaq    .Lmodule_owner_mutated_table(%rip), %rax
        jmpq    *(%rax,%rdi,8)

        .macro  MODULE_OWNER_MUTATED_CASE number
.Lmodule_owner_mutated_case\number:
        movl    $(6200+\number), %eax
        retq
        .endm
        .irp    number,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19
        MODULE_OWNER_MUTATED_CASE \number
        .endr
        .purgem MODULE_OWNER_MUTATED_CASE
.Lmodule_owner_mutated_default:
        movl    $6299, %eax
        retq
        .size   module_owner_mutated_dispatch, .-module_owner_mutated_dispatch

        .globl  module_owner_mutated_loop_writer
        .type   module_owner_mutated_loop_writer,@function
module_owner_mutated_loop_writer:
        leaq    .Lmodule_owner_mutated_table(%rip), %rcx
        leaq    .Lmodule_owner_mutated_table_end(%rip), %rdx
.Lmodule_owner_mutated_loop:
        movq    %rax, (%rcx)
        addq    $8, %rcx
        cmpq    %rdx, %rcx
        jne     .Lmodule_owner_mutated_loop
        retq
        .size   module_owner_mutated_loop_writer, .-module_owner_mutated_loop_writer

// This bounded read loop is unrelated to either jump-table object.  Its
// recurrent pointer state has the same shape as real pointer-table induction
// code that previously made module arbitration exhaust its fixed-point work.
        .globl  module_owner_unrelated_pointer_induction
        .type   module_owner_unrelated_pointer_induction,@function
module_owner_unrelated_pointer_induction:
        leaq    .Lmodule_owner_unrelated_data(%rip), %rcx
        leaq    .Lmodule_owner_unrelated_data_end(%rip), %rdx
        xorl    %eax, %eax
.Lmodule_owner_unrelated_loop:
        addl    (%rcx), %eax
        addq    $8, %rcx
        cmpq    %rdx, %rcx
        jne     .Lmodule_owner_unrelated_loop
        retq
        .size   module_owner_unrelated_pointer_induction, .-module_owner_unrelated_pointer_induction

        .section .data.module_address_owner_induction,"aw",@progbits
        .p2align 3
.Lmodule_owner_safe_table:
        .quad   .Lmodule_owner_safe_case0
        .quad   .Lmodule_owner_safe_case1
.Lmodule_owner_safe_table_end:
        .quad   0

        .p2align 3
.Lmodule_owner_mutated_table:
        .irp    number,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19
        .quad   .Lmodule_owner_mutated_case\number
        .endr
.Lmodule_owner_mutated_table_end:

        .section .rodata.module_address_owner_induction,"a",@progbits
        .p2align 3
.Lmodule_owner_unrelated_data:
        .quad   7
        .quad   11
.Lmodule_owner_unrelated_data_end:

        .section .note.GNU-stack,"",@progbits
