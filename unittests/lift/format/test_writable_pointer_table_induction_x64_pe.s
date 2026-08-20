.text
.globl main
.globl probe
.def main; .scl 2; .type 32; .endef
.def probe; .scl 2; .type 32; .endef
.p2align 4
main:
        subq $40, %rsp
        callq probe
        addq $40, %rsp
        retq

.p2align 4
probe:
        pushq %rbx
        pushq %r12
        pushq %r13
        leaq table(%rip), %r12
        xorl %ebx, %ebx
        xorl %r13d, %r13d
.Lloop:
        movq (%r12), %rax
        movl (%rax), %eax
        addl %eax, %r13d
        addq $8, %r12
        addl $1, %ebx
        cmpl $2, %ebx
        jb .Lloop
        movl %r13d, %eax
        popq %r13
        popq %r12
        popq %rbx
        retq

.section .data,"dw"
.p2align 3
table:
        .quad entry0
        .quad entry1
.p2align 2
entry0:
        .long 3
entry1:
        .long 4
