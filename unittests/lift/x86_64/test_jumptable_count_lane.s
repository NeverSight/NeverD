// A scalar bit count fits in its low byte even when the table address uses
// the complete destination register. Each table has independent storage.
        .macro count_dispatch name, instruction, wide, byte_guard, clobber=0, relative=1
        .text
        .globl \name
        .type \name,@function
\name:
        .if \wide
        \instruction %rdi, %rcx
        .else
        \instruction %edi, %ecx
        .endif
        .if \clobber == 1
        orl $256, %ecx
        .elseif \clobber == 2
        movb $1, %ch
        .endif
        .if \byte_guard
        cmpb $3, %cl
        .elseif \wide
        cmpq $3, %rcx
        .else
        cmpl $3, %ecx
        .endif
        ja .Ldefault\@
        leaq .Ltable\@(%rip), %rax
        .if \relative
        movslq (%rax,%rcx,4), %rdx
        addq %rax, %rdx
        .else
        movq (%rax,%rcx,8), %rdx
        .endif
        jmpq *%rdx
.Lcase0\@:
        movl $8100, %eax
        retq
.Lcase1\@:
        movl $8101, %eax
        retq
.Lcase2\@:
        movl $8102, %eax
        retq
.Lcase3\@:
        movl $8103, %eax
        retq
.Ldefault\@:
        movl $8999, %eax
        retq
        .size \name, .-\name
        .if \relative
        .section .rodata.\name,"a",@progbits
        .p2align 2
.Ltable\@:
        .long .Lcase0\@-.Ltable\@, .Lcase1\@-.Ltable\@
        .long .Lcase2\@-.Ltable\@, .Lcase3\@-.Ltable\@
        .else
        .section .data.rel.ro.\name,"aw",@progbits
        .p2align 3
.Ltable\@:
        .quad .Lcase0\@, .Lcase1\@, .Lcase2\@, .Lcase3\@
        .endif
        .endm

        count_dispatch jt_ctz_byte32, tzcntl, 0, 1
        count_dispatch jt_ctz_full32, tzcntl, 0, 0
        count_dispatch jt_ctz_byte64, tzcntq, 1, 1
        count_dispatch jt_ctz_full64, tzcntq, 1, 0
        count_dispatch jt_pop_byte32, popcntl, 0, 1
        count_dispatch jt_pop_full32, popcntl, 0, 0
        count_dispatch jt_pop_byte64, popcntq, 1, 1
        count_dispatch jt_pop_full64, popcntq, 1, 0
        count_dispatch jt_lz_byte32, lzcntl, 0, 1
        count_dispatch jt_lz_full32, lzcntl, 0, 0
        count_dispatch jt_lz_byte64, lzcntq, 1, 1
        count_dispatch jt_lz_full64, lzcntq, 1, 0
        count_dispatch jt_ctz_byte32_absolute, tzcntl, 0, 1, 0, 0
        count_dispatch jt_ctz_byte64_absolute, tzcntq, 1, 1, 0, 0
        count_dispatch jt_pop_byte32_absolute, popcntl, 0, 1, 0, 0
        count_dispatch jt_pop_byte64_absolute, popcntq, 1, 1, 0, 0
        count_dispatch jt_lz_byte32_absolute, lzcntl, 0, 1, 0, 0
        count_dispatch jt_lz_byte64_absolute, lzcntq, 1, 1, 0, 0
        count_dispatch jt_count_or_high32, popcntl, 0, 1, 1
        count_dispatch jt_count_or_high64, popcntq, 1, 1, 1
        count_dispatch jt_count_write_high32, popcntl, 0, 1, 2
        count_dispatch jt_count_write_high64, popcntq, 1, 1, 2
        .purgem count_dispatch
        .section .note.GNU-stack,"",@progbits
