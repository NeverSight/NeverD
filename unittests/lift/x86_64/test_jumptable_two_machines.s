# Two loop-carried state machines in one function (clang -O2 shape).  Each
# `while (st) switch (st)` has a constant entry dispatch and a loop dispatch,
# both reading one unguarded relative table; the two tables are adjacent, so
# their relocations form one run.  The second machine is entered only once the
# first exits, so its entry dispatch is reachable only through the first
# machine's table edges, while the first machine's entry dispatch is closed
# only once the second machine is resolved.

  .text
  .globl jt_two_machines
  .type jt_two_machines, @function
  .p2align 4
jt_two_machines:
  orl $1, %edi
  movl $1, %ecx
  leal -1(%rcx), %eax
  leaq .Ltm_table1(%rip), %rdx
  movslq (%rdx,%rax,4), %rsi
  addq %rdx, %rsi
  xorl %eax, %eax
  jmpq *%rsi
.Ltm_loop1:
  incl %ecx
  cmpl $4001, %ecx
  je .Ltm_limit1
  decl %esi
  movslq (%rdx,%rsi,4), %rsi
  addq %rdx, %rsi
  jmpq *%rsi
.Ltm_a1:
  imull $1103515245, %edi, %edi
  addl $12345, %edi
  xorl %edi, %eax
  movl $2, %esi
  testl %esi, %esi
  jne .Ltm_loop1
  jmp .Ltm_machine2
.Ltm_a4:
  movl %eax, %esi
  leal (%rdi,%rdi), %eax
  xorl %esi, %eax
  xorl %esi, %esi
  testl $1024, %edi
  sete %sil
  leal (%rsi,%rsi,2), %esi
  addl $2, %esi
  testl %esi, %esi
  jne .Ltm_loop1
  jmp .Ltm_machine2
.Ltm_a5:
  movl %edi, %esi
  shrl $5, %esi
  subl %esi, %eax
  xorl %esi, %esi
  testb $112, %al
  setne %sil
  testl %esi, %esi
  jne .Ltm_loop1
  jmp .Ltm_machine2
.Ltm_a2:
  movl %eax, %r8d
  shll $5, %r8d
  subl %eax, %r8d
  addl %edi, %r8d
  movl %edi, %eax
  andl $256, %eax
  cmpl $1, %eax
  movl $0, %esi
  adcl $3, %esi
  movl %r8d, %eax
  testl %esi, %esi
  jne .Ltm_loop1
  jmp .Ltm_machine2
.Ltm_a3:
  movl %eax, %esi
  movl %edi, %eax
  shrl $3, %eax
  addl %esi, %eax
  movl %edi, %esi
  andl $512, %esi
  cmpl $1, %esi
  movl $0, %esi
  adcl $4, %esi
  testl %esi, %esi
  jne .Ltm_loop1
  jmp .Ltm_machine2
.Ltm_limit1:
  movl $4001, %ecx
.Ltm_machine2:
  movl $1, %edx
  leal -1(%rdx), %r8d
  leaq .Ltm_table2(%rip), %rsi
  movslq (%rsi,%r8,4), %r8
  addq %rsi, %r8
  jmpq *%r8
.Ltm_loop2:
  incl %edx
  cmpl $4001, %edx
  je .Ltm_limit2
  decl %r8d
  movslq (%rsi,%r8,4), %r8
  addq %rsi, %r8
  jmpq *%r8
.Ltm_b1:
  imull $22695477, %edi, %edi
  incl %edi
  xorl %edi, %eax
  movl $2, %r8d
  testl %r8d, %r8d
  jne .Ltm_loop2
  jmp .Ltm_done
.Ltm_b4:
  movl %eax, %r8d
  leal (,%rdi,4), %eax
  xorl %r8d, %eax
  xorl %r8d, %r8d
  testl $4096, %edi
  sete %r8b
  leal (%r8,%r8,2), %r8d
  addl $2, %r8d
  testl %r8d, %r8d
  jne .Ltm_loop2
  jmp .Ltm_done
.Ltm_b5:
  movl %edi, %r8d
  shrl $6, %r8d
  subl %r8d, %eax
  xorl %r8d, %r8d
  testb $56, %al
  setne %r8b
  testl %r8d, %r8d
  jne .Ltm_loop2
  jmp .Ltm_done
.Ltm_b2:
  movl %eax, %r8d
  shll $5, %r8d
  addl %edi, %eax
  addl %r8d, %eax
  xorl %r8d, %r8d
  testb %dil, %dil
  setns %r8b
  addl $3, %r8d
  testl %r8d, %r8d
  jne .Ltm_loop2
  jmp .Ltm_done
.Ltm_b3:
  movl %eax, %r8d
  movl %edi, %eax
  shrl $2, %eax
  addl %r8d, %eax
  movl %edi, %r9d
  andl $2048, %r9d
  xorl %r8d, %r8d
  cmpl $1, %r9d
  adcl $4, %r8d
  testl %r8d, %r8d
  jne .Ltm_loop2
  jmp .Ltm_done
.Ltm_limit2:
  movl $4001, %edx
.Ltm_done:
  addl %ecx, %eax
  addl %edx, %eax
  retq
  .size jt_two_machines, .-jt_two_machines

  .section .rodata,"a",@progbits
  .p2align 2
.Ltm_table1:
  .long .Ltm_a1-.Ltm_table1
  .long .Ltm_a2-.Ltm_table1
  .long .Ltm_a3-.Ltm_table1
  .long .Ltm_a4-.Ltm_table1
  .long .Ltm_a5-.Ltm_table1
.Ltm_table2:
  .long .Ltm_b1-.Ltm_table2
  .long .Ltm_b2-.Ltm_table2
  .long .Ltm_b3-.Ltm_table2
  .long .Ltm_b4-.Ltm_table2
  .long .Ltm_b5-.Ltm_table2
  .section .note.GNU-stack,"",@progbits
