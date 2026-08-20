.section __TEXT,__text,regular,pure_instructions
.globl _main
.p2align 4
_main:
        pushq %rbx
        pushq %r12
        pushq %r13
        leaq _table(%rip), %r12
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

.section __DATA,__data
.p2align 3
_table:
        .quad _entry0
        .quad _entry1

.section __TEXT,__const
.p2align 2
_entry0:
        .long 3
_entry1:
        .long 4
