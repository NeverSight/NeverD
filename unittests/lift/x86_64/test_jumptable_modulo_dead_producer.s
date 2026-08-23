// A valid `%140` producer elsewhere in the function cannot authorize a raw
// selector that overwrites it before the table address is formed.  Keep this
// deliberately unresolved function in its own object so whole-object emitter
// tests for the other modulo negatives remain independent.

        .text
        .globl  jt_modulo_u140_dead_producer
        .type   jt_modulo_u140_dead_producer,@function
jt_modulo_u140_dead_producer:
        movl    %edi, %eax
        movl    %eax, %ecx
        shrl    $2, %ecx
        imulq   $0x3a83a83b, %rcx, %rcx
        shrq    $35, %rcx
        imull   $140, %ecx, %ecx
        subl    %ecx, %eax
        movl    %esi, %eax
        leaq    .Lmod140_dead_table(%rip), %rcx
        movslq  (%rcx,%rax,4), %rax
        addq    %rcx, %rax
        jmpq    *%rax

        .altmacro
        .macro  JT_MOD140_DEAD_CASE
.Lmod140_dead_case_\+:
        movl    $(6000+\+), %eax
        retq
        .endm
        .rept   140
        JT_MOD140_DEAD_CASE
        .endr
        .purgem JT_MOD140_DEAD_CASE
        .noaltmacro
        .size   jt_modulo_u140_dead_producer, .-jt_modulo_u140_dead_producer

        .section .rodata,"a",@progbits
        .p2align 2
.Lmod140_dead_table:
        .altmacro
        .macro  JT_MOD140_DEAD_ENTRY
        .long   .Lmod140_dead_case_\+-.Lmod140_dead_table
        .endm
        .rept   140
        JT_MOD140_DEAD_ENTRY
        .endr
        .purgem JT_MOD140_DEAD_ENTRY
        .noaltmacro
        .long   0

        .section .note.GNU-stack,"",@progbits
