# Callers of the -g functions in test_debug_frame_argument.c.  Each sets the
# first argument to the address of its own buffer on both paths to the one
# call, so the argument register is the same frame address merged at the
# call, which no single assignment forwards into it.
        .text
        .globl  dfa_caller
        .type   dfa_caller,@function
dfa_caller:
        pushq   %rbp
        movq    %rsp, %rbp
        subq    $16, %rsp
        movq    $0, -16(%rbp)
        movq    $0, -8(%rbp)
        testl   %edi, %edi
        je      .Lsum_two
        leaq    -16(%rbp), %rdi
        movl    $1, %esi
        jmp     .Lsum_call
.Lsum_two:
        leaq    -16(%rbp), %rdi
        movl    $2, %esi
.Lsum_call:
        callq   dfa_sum
        addq    $16, %rsp
        popq    %rbp
        retq
        .size   dfa_caller, .-dfa_caller

        .globl  dfa_pointer_caller
        .type   dfa_pointer_caller,@function
dfa_pointer_caller:
        pushq   %rbp
        movq    %rsp, %rbp
        subq    $16, %rsp
        movq    $0, -16(%rbp)
        movq    $0, -8(%rbp)
        testl   %edi, %edi
        je      .Lfirst_two
        leaq    -16(%rbp), %rdi
        movl    $1, %esi
        jmp     .Lfirst_call
.Lfirst_two:
        leaq    -16(%rbp), %rdi
        movl    $2, %esi
.Lfirst_call:
        callq   dfa_first
        addq    $16, %rsp
        popq    %rbp
        retq
        .size   dfa_pointer_caller, .-dfa_pointer_caller

        .globl  _start
        .type   _start,@function
_start:
        xorl    %edi, %edi
        callq   dfa_caller
        movl    $1, %edi
        callq   dfa_pointer_caller
        movl    $60, %eax
        xorl    %edi, %edi
        syscall
        .size   _start, .-_start
        .section .note.GNU-stack,"",@progbits
