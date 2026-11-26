# Cyclic switch whose selector is a sum of absolute differences masked to
# three bits (clang -O2 -mssse3).  The case bodies rewrite the arrays the
# next iteration sums, so the search for masks upstream of `and $7` cannot
# finish once the cases are reachable.
#
# jt_cyclic_sad8 owns eight relative-code relocations: the mask domain is
# exactly its table.  jt_cyclic_sad_short masks with the same seven but owns
# only four relocated slots; the next four words carry no relocation, so its
# dispatch must stay unresolved.

  .text
  .globl jt_cyclic_sad8
  .type jt_cyclic_sad8, @function
jt_cyclic_sad8:
  subq $136, %rsp
  orl $3, %edi
  xorl %eax, %eax
  .p2align 4
.Ls8_fill:
  imull $1664525, %edi, %ecx
  addl $1013904223, %ecx
  shrl $16, %ecx
  movb %cl, (%rsp,%rax)
  imull $389569705, %edi, %ecx
  addl $1196435762, %ecx
  shrl $16, %ecx
  movb %cl, -128(%rsp,%rax)
  imull $-1354167659, %edi, %ecx
  addl $-775096599, %ecx
  shrl $16, %ecx
  movb %cl, 1(%rsp,%rax)
  imull $158984081, %edi, %edi
  addl $-1426500812, %edi
  movl %edi, %ecx
  shrl $16, %ecx
  movb %cl, -127(%rsp,%rax)
  addq $2, %rax
  cmpq $128, %rax
  jne .Ls8_fill
  xorl %edx, %edx
  leaq jt_cyclic_sad8_table(%rip), %rax
  xorl %ecx, %ecx
  jmp .Ls8_round
.Ls8_case4:
  shll $6, %esi
.Ls8_mix:
  xorl %edx, %esi
.Ls8_store:
  leal (%rcx,%rsi), %edx
  movb %dl, (%rsp,%rcx)
  movl %esi, %edx
  shrl $3, %edx
  leaq (%rcx,%rcx,2), %rdi
  movb %dl, -128(%rsp,%rdi)
  movl %esi, %edx
  shrl $7, %edx
  xorl %esi, %edx
  incq %rcx
  cmpq $40, %rcx
  je .Ls8_done
.Ls8_round:
  xorl %esi, %esi
  pxor %xmm1, %xmm1
  pxor %xmm0, %xmm0
  .p2align 4
.Ls8_sum:
  movsd (%rsp,%rsi), %xmm2
  movsd -128(%rsp,%rsi), %xmm3
  xorps %xmm4, %xmm4
  movss %xmm2, %xmm4
  xorps %xmm2, %xmm2
  movss %xmm3, %xmm2
  psadbw %xmm4, %xmm2
  paddd %xmm2, %xmm1
  movd 4(%rsp,%rsi), %xmm2
  movd -124(%rsp,%rsi), %xmm3
  psadbw %xmm2, %xmm3
  paddd %xmm3, %xmm0
  addq $8, %rsi
  cmpq $128, %rsi
  jne .Ls8_sum
  paddd %xmm1, %xmm0
  pshufd $238, %xmm0, %xmm1
  paddd %xmm0, %xmm1
  pshufd $85, %xmm1, %xmm0
  paddd %xmm1, %xmm0
  movd %xmm0, %esi
  movl %esi, %edi
  andl $7, %edi
  movslq (%rax,%rdi,4), %rdi
  addq %rax, %rdi
.globl jt_cyclic_sad8_branch
jt_cyclic_sad8_branch:
  jmpq *%rdi
.Ls8_case0:
  addl %edx, %esi
  jmp .Ls8_store
.Ls8_case2:
  shrl $2, %esi
  jmp .Ls8_sub
.Ls8_case3:
  leal (%rsi,%rsi,2), %esi
  addl %edx, %esi
  jmp .Ls8_store
.Ls8_case7:
  imull $131, %esi, %esi
  jmp .Ls8_mix
.Ls8_case1:
  shll $4, %esi
  jmp .Ls8_mix
.Ls8_case5:
  addl %esi, %esi
.Ls8_sub:
  subl %esi, %edx
  movl %edx, %esi
  jmp .Ls8_store
.Ls8_case6:
  shll $8, %esi
  addl %edx, %esi
  jmp .Ls8_store
.Ls8_done:
  movl %edx, %eax
  addq $136, %rsp
  retq
  .size jt_cyclic_sad8, .-jt_cyclic_sad8

  .globl jt_cyclic_sad_short
  .type jt_cyclic_sad_short, @function
jt_cyclic_sad_short:
  subq $136, %rsp
  orl $3, %edi
  xorl %eax, %eax
  .p2align 4
.Lss_fill:
  imull $1664525, %edi, %ecx
  addl $1013904223, %ecx
  shrl $16, %ecx
  movb %cl, (%rsp,%rax)
  imull $389569705, %edi, %ecx
  addl $1196435762, %ecx
  shrl $16, %ecx
  movb %cl, -128(%rsp,%rax)
  addq $1, %rax
  cmpq $128, %rax
  jne .Lss_fill
  xorl %edx, %edx
  leaq jt_cyclic_sad_short_table(%rip), %rax
  xorl %ecx, %ecx
  jmp .Lss_round
.Lss_case3:
  shll $6, %esi
.Lss_store:
  leal (%rcx,%rsi), %edx
  movb %dl, (%rsp,%rcx)
  movl %esi, %edx
  shrl $3, %edx
  leaq (%rcx,%rcx,2), %rdi
  movb %dl, -128(%rsp,%rdi)
  movl %esi, %edx
  shrl $7, %edx
  xorl %esi, %edx
  incq %rcx
  cmpq $40, %rcx
  je .Lss_done
.Lss_round:
  xorl %esi, %esi
  pxor %xmm1, %xmm1
  pxor %xmm0, %xmm0
  .p2align 4
.Lss_sum:
  movsd (%rsp,%rsi), %xmm2
  movsd -128(%rsp,%rsi), %xmm3
  xorps %xmm4, %xmm4
  movss %xmm2, %xmm4
  xorps %xmm2, %xmm2
  movss %xmm3, %xmm2
  psadbw %xmm4, %xmm2
  paddd %xmm2, %xmm1
  movd 4(%rsp,%rsi), %xmm2
  movd -124(%rsp,%rsi), %xmm3
  psadbw %xmm2, %xmm3
  paddd %xmm3, %xmm0
  addq $8, %rsi
  cmpq $128, %rsi
  jne .Lss_sum
  paddd %xmm1, %xmm0
  pshufd $238, %xmm0, %xmm1
  paddd %xmm0, %xmm1
  pshufd $85, %xmm1, %xmm0
  paddd %xmm1, %xmm0
  movd %xmm0, %esi
  movl %esi, %edi
  andl $7, %edi
  movslq (%rax,%rdi,4), %rdi
  addq %rax, %rdi
.globl jt_cyclic_sad_short_branch
jt_cyclic_sad_short_branch:
  jmpq *%rdi
.Lss_case0:
  addl %edx, %esi
  jmp .Lss_store
.Lss_case1:
  shll $4, %esi
  xorl %edx, %esi
  jmp .Lss_store
.Lss_case2:
  shrl $2, %esi
  subl %esi, %edx
  movl %edx, %esi
  jmp .Lss_store
.Lss_done:
  movl %edx, %eax
  addq $136, %rsp
  retq
  .size jt_cyclic_sad_short, .-jt_cyclic_sad_short

  .section .rodata,"a",@progbits
  .p2align 2
  .globl jt_cyclic_sad8_table
  .type jt_cyclic_sad8_table, @object
jt_cyclic_sad8_table:
  .long .Ls8_case0-jt_cyclic_sad8_table
  .long .Ls8_case1-jt_cyclic_sad8_table
  .long .Ls8_case2-jt_cyclic_sad8_table
  .long .Ls8_case3-jt_cyclic_sad8_table
  .long .Ls8_case4-jt_cyclic_sad8_table
  .long .Ls8_case5-jt_cyclic_sad8_table
  .long .Ls8_case6-jt_cyclic_sad8_table
  .long .Ls8_case7-jt_cyclic_sad8_table
  .size jt_cyclic_sad8_table, .-jt_cyclic_sad8_table
  .globl jt_cyclic_sad_short_table
  .type jt_cyclic_sad_short_table, @object
jt_cyclic_sad_short_table:
  .long .Lss_case0-jt_cyclic_sad_short_table
  .long .Lss_case1-jt_cyclic_sad_short_table
  .long .Lss_case2-jt_cyclic_sad_short_table
  .long .Lss_case3-jt_cyclic_sad_short_table
  .long 0, 0, 0, 0
  .size jt_cyclic_sad_short_table, .-jt_cyclic_sad_short_table
  .section .note.GNU-stack,"",@progbits
