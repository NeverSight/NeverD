        .text
        .globl main
        .globl probe
        .def main; .scl 2; .type 32; .endef
        .def probe; .scl 2; .type 32; .endef
        .p2align 4
main:
        subq $40, %rsp
        xorl %ecx, %ecx
        xorl %edx, %edx
        callq probe
        addq $40, %rsp
        retq

        .p2align 4
probe:
        pushq %r12
        pushq %r13
        pushq %r14
        pushq %r15
        leaq table(%rip), %r13
        leaq table(%rip), %r12
        xorl %r14d, %r14d
        movl $1, %r15d

.Lheader:
        testl %edx, %edx
        movq %r12, %r8
        cmoveq %r13, %r8
        movq (%r8,%r14,8), %r9
        movq (%r8,%r15,8), %r10
        addl $1, %ecx
        testl %edx, %edx
        je .Lsetbase
        jmp .Lbypass

.Lsetbase:
        leaq table(%rip), %r13
        jmp .Ljoin

.Lbypass:
.Ljoin:
        cmpl $1, %ecx
        je .Lexit
        jmp .Lheader

.Lexit:
        movl $7, %eax
        popq %r15
        popq %r14
        popq %r13
        popq %r12
        retq

        .section .rdata,"dr"
        .p2align 3
table:
        .quad message
        .quad message
message:
        .asciz "rematerialized-x64-pe"
