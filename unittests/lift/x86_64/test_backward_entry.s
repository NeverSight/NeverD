// A valid function entry can transfer control to code laid out at a lower
// address. CFG block byte ranges remain address-sorted, but the public LowFunc
// invariant requires the symbol entry itself to be block 0.

        .text
        .p2align 4
.Lbackward_body:
        callq   .Lcallee
        movl    %r8d, %eax
        retq

.Lcallee:
        retq

        .globl  test_backward_entry
        .type   test_backward_entry,@function
test_backward_entry:
        movl    $7, %r8d
        jmp     .Lbackward_body
        .size   test_backward_entry, .-test_backward_entry

// A decoded direct branch can point outside every mapped executable segment
// (for example when function discovery lands on padding/data). CFGBuilder
// represents that target as an empty block; Med CFG simplification must remove
// both the empty block and every edge that named it.
        .p2align 4
        .globl  test_unmapped_branch
        .type   test_unmapped_branch,@function
test_unmapped_branch:
        .byte   0xe9
        .long   0x70000000
        .size   test_unmapped_branch, .-test_unmapped_branch

// FNINIT/FNCLEX change x87 state but do not define a general-purpose return
// register. The lifted function must therefore return its original argument
// while preserving both state instructions as side-effecting inline asm.
        .p2align 4
        .globl  test_x87_state_ops
        .type   test_x87_state_ops,@function
test_x87_state_ops:
        movq    %rdi, %rax
        fninit
        fnclex
        retq
        .size   test_x87_state_ops, .-test_x87_state_ops

        .section .note.GNU-stack,"",@progbits
