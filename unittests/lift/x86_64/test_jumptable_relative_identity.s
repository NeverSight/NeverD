//===- test_jumptable_relative_identity.s - PIC identity fixtures --------===//

        .text

// Unsized PIC-relative callback tables use the same owner-bounded tri-state
// identity policy as absolute pointer tables.  The raw relocation run is only
// an identity scan boundary; it never supplies a selector domain.
        .globl  jt_relative_identity_foreign_target
        .type   jt_relative_identity_foreign_target,@function
jt_relative_identity_foreign_target:
        movl    $4962, %eax
.Lrelative_identity_unknown_target:
        retq
        .size jt_relative_identity_foreign_target, .-jt_relative_identity_foreign_target

        .globl  jt_modulo_unsized_relative_mixed_callback
        .type   jt_modulo_unsized_relative_mixed_callback,@function
jt_modulo_unsized_relative_mixed_callback:
        leaq    jt_modulo_unsized_relative_mixed_table(%rip), %rax
        movslq  (%rax,%rdi,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lmodulo_unsized_relative_local:
        movl    $4964, %eax
        retq
        .size jt_modulo_unsized_relative_mixed_callback, .-jt_modulo_unsized_relative_mixed_callback

        .globl  jt_modulo_unsized_relative_foreign_callback
        .type   jt_modulo_unsized_relative_foreign_callback,@function
jt_modulo_unsized_relative_foreign_callback:
        leaq    jt_modulo_unsized_relative_foreign_table(%rip), %rax
        movslq  (%rax,%rdi,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
        .size jt_modulo_unsized_relative_foreign_callback, .-jt_modulo_unsized_relative_foreign_callback

        .globl  jt_modulo_unsized_relative_unknown_callback
        .type   jt_modulo_unsized_relative_unknown_callback,@function
jt_modulo_unsized_relative_unknown_callback:
        leaq    jt_modulo_unsized_relative_unknown_table(%rip), %rax
        movslq  (%rax,%rdi,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
        .size jt_modulo_unsized_relative_unknown_callback, .-jt_modulo_unsized_relative_unknown_callback

        .globl  jt_modulo_unsized_relative_unknown_two_callback
        .type   jt_modulo_unsized_relative_unknown_two_callback,@function
jt_modulo_unsized_relative_unknown_two_callback:
        leaq    jt_modulo_unsized_relative_unknown_two_table(%rip), %rax
        movslq  (%rax,%rdi,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
        .size jt_modulo_unsized_relative_unknown_two_callback, .-jt_modulo_unsized_relative_unknown_two_callback

// A run exactly at the global limit is still classifiable when its sized
// object proves the end.  The two unsized companions continue for one more
// slot; their hidden local/unknown tail must keep the original JMP opaque.
        .globl  jt_identity_exact_limit_foreign_callback
        .type   jt_identity_exact_limit_foreign_callback,@function
jt_identity_exact_limit_foreign_callback:
        leaq    jt_identity_exact_limit_foreign_table(%rip), %rax
        jmpq    *(%rax,%rdi,8)
        .size jt_identity_exact_limit_foreign_callback, .-jt_identity_exact_limit_foreign_callback

        .globl  jt_identity_over_limit_absolute_callback
        .type   jt_identity_over_limit_absolute_callback,@function
jt_identity_over_limit_absolute_callback:
        leaq    jt_identity_over_limit_absolute_table(%rip), %rax
        jmpq    *(%rax,%rdi,8)
.Lidentity_over_limit_absolute_local:
        movl    $4965, %eax
        retq
        .size jt_identity_over_limit_absolute_callback, .-jt_identity_over_limit_absolute_callback

        .globl  jt_identity_over_limit_relative_callback
        .type   jt_identity_over_limit_relative_callback,@function
jt_identity_over_limit_relative_callback:
        leaq    jt_identity_over_limit_relative_table(%rip), %rax
        movslq  (%rax,%rdi,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
        .size jt_identity_over_limit_relative_callback, .-jt_identity_over_limit_relative_callback

        .section .rodata.jt_relative_identity_mixed,"a",@progbits
        .p2align 2
        .globl jt_modulo_unsized_relative_mixed_table
jt_modulo_unsized_relative_mixed_table:
        .long jt_relative_identity_foreign_target-jt_modulo_unsized_relative_mixed_table
        .long jt_relative_identity_foreign_target-jt_modulo_unsized_relative_mixed_table
        .long .Lmodulo_unsized_relative_local-jt_modulo_unsized_relative_mixed_table

        .section .rodata.jt_relative_identity_foreign,"a",@progbits
        .p2align 2
        .globl jt_modulo_unsized_relative_foreign_table
jt_modulo_unsized_relative_foreign_table:
        .long jt_relative_identity_foreign_target-jt_modulo_unsized_relative_foreign_table
        .long jt_relative_identity_foreign_target-jt_modulo_unsized_relative_foreign_table
        .long jt_relative_identity_foreign_target-jt_modulo_unsized_relative_foreign_table

        .section .rodata.jt_relative_identity_unknown,"a",@progbits
        .p2align 2
        .globl jt_modulo_unsized_relative_unknown_table
jt_modulo_unsized_relative_unknown_table:
        .long jt_relative_identity_foreign_target-jt_modulo_unsized_relative_unknown_table
        .long jt_relative_identity_foreign_target-jt_modulo_unsized_relative_unknown_table
        .long .Lrelative_identity_unknown_target-jt_modulo_unsized_relative_unknown_table

        .section .rodata.jt_relative_identity_unknown_two,"a",@progbits
        .p2align 2
        .globl jt_modulo_unsized_relative_unknown_two_table
jt_modulo_unsized_relative_unknown_two_table:
        .long jt_relative_identity_foreign_target-jt_modulo_unsized_relative_unknown_two_table
        .long .Lrelative_identity_unknown_target-jt_modulo_unsized_relative_unknown_two_table

        .section .rodata.jt_identity_exact_limit_foreign,"a",@progbits
        .p2align 3
        .globl jt_identity_exact_limit_foreign_table
        .type  jt_identity_exact_limit_foreign_table,@object
jt_identity_exact_limit_foreign_table:
        .rept 4096
        .quad jt_relative_identity_foreign_target
        .endr
        .size jt_identity_exact_limit_foreign_table, .-jt_identity_exact_limit_foreign_table
        .quad 0xdeadbeefdeadbeef

        .section .rodata.jt_identity_over_limit_absolute,"a",@progbits
        .p2align 3
        .globl jt_identity_over_limit_absolute_table
jt_identity_over_limit_absolute_table:
        .rept 4096
        .quad jt_relative_identity_foreign_target
        .endr
        .quad .Lidentity_over_limit_absolute_local

        .section .rodata.jt_identity_over_limit_relative,"a",@progbits
        .p2align 2
        .globl jt_identity_over_limit_relative_table
jt_identity_over_limit_relative_table:
        .rept 4096
        .long jt_relative_identity_foreign_target-jt_identity_over_limit_relative_table
        .endr
        .long .Lrelative_identity_unknown_target-jt_identity_over_limit_relative_table
