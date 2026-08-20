.text
.globl _start
.globl probe
.type _start, @function
.type probe, @function
.p2align 4
_start:
        callq probe
        movl %eax, %edi
        movl $60, %eax
        syscall
.Lhalt:
        jmp .Lhalt
.size _start, .-_start

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
.size probe, .-probe

.section .data,"aw",@progbits
.p2align 3
table:
        .quad entry0
        .quad entry1

.section .rodata,"a",@progbits
.p2align 2
entry0:
        .long 3
entry1:
        .long 4
