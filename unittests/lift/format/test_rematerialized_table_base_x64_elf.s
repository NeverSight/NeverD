        .text
        .globl _start
        .globl probe
        .type _start,@function
        .type probe,@function
        .p2align 4
_start:
        xorl %edi, %edi
        xorl %esi, %esi
        callq probe
        movl %eax, %edi
        movl $60, %eax
        syscall
.Lhalt:
        jmp .Lhalt
        .size _start, .-_start

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
        testl %esi, %esi
        movq %r12, %r8
        cmoveq %r13, %r8
        movq (%r8,%r14,8), %r9
        movq (%r8,%r15,8), %r10
        addl $1, %edi
        testl %esi, %esi
        je .Lsetbase
        jmp .Lbypass

.Lsetbase:
        leaq table(%rip), %r13
        jmp .Ljoin

.Lbypass:
.Ljoin:
        cmpl $1, %edi
        je .Lexit
        jmp .Lheader

.Lexit:
        movl $7, %eax
        popq %r15
        popq %r14
        popq %r13
        popq %r12
        retq
        .size probe, .-probe

        .section .data.rel.ro,"aw",@progbits
        .p2align 3
table:
        .quad message
        .quad message

        .section .rodata,"a",@progbits
message:
        .asciz "rematerialized-x64-elf"
