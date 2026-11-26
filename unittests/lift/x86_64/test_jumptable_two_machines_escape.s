# test_jumptable_two_machines.s with the second machine seeded from the first
# machine's exit state instead of 1.  The ordinary exit leaves 0, so the second
# entry dispatch reads the word before its own table: no edge may be
# published that decodes a slot outside its own table.

  .text
  .globl jt_two_machines_escape
  .type jt_two_machines_escape, @function
  .p2align 4
jt_two_machines_escape:
  orl $1, %edi
  movl $1, %ecx
  leal -1(%rcx), %eax
  leaq .Lte_table1(%rip), %rdx
  movslq (%rdx,%rax,4), %rsi
  addq %rdx, %rsi
  xorl %eax, %eax
  jmpq *%rsi
.Lte_loop1:
  incl %ecx
  cmpl $4001, %ecx
  je .Lte_machine2
  decl %esi
  movslq (%rdx,%rsi,4), %rsi
  addq %rdx, %rsi
  jmpq *%rsi
.Lte_a1:
  imull $1103515245, %edi, %edi
  addl $12345, %edi
  xorl %edi, %eax
  movl $2, %esi
  testl %esi, %esi
  jne .Lte_loop1
  jmp .Lte_machine2
.Lte_a4:
  movl %eax, %esi
  leal (%rdi,%rdi), %eax
  xorl %esi, %eax
  xorl %esi, %esi
  testl $1024, %edi
  sete %sil
  leal (%rsi,%rsi,2), %esi
  addl $2, %esi
  testl %esi, %esi
  jne .Lte_loop1
  jmp .Lte_machine2
.Lte_a5:
  movl %edi, %esi
  shrl $5, %esi
  subl %esi, %eax
  xorl %esi, %esi
  testb $112, %al
  setne %sil
  testl %esi, %esi
  jne .Lte_loop1
  jmp .Lte_machine2
.Lte_a2:
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
  jne .Lte_loop1
  jmp .Lte_machine2
.Lte_a3:
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
  jne .Lte_loop1
  jmp .Lte_machine2
.Lte_machine2:
  movl %esi, %edx
  leal -1(%rdx), %r8d
  leaq .Lte_table2(%rip), %rsi
  movslq (%rsi,%r8,4), %r8
  addq %rsi, %r8
  jmpq *%r8
.Lte_loop2:
  incl %edx
  cmpl $4001, %edx
  je .Lte_done
  decl %r8d
  movslq (%rsi,%r8,4), %r8
  addq %rsi, %r8
  jmpq *%r8
.Lte_b1:
  imull $22695477, %edi, %edi
  incl %edi
  xorl %edi, %eax
  movl $2, %r8d
  testl %r8d, %r8d
  jne .Lte_loop2
  jmp .Lte_done
.Lte_b4:
  movl %eax, %r8d
  leal (,%rdi,4), %eax
  xorl %r8d, %eax
  xorl %r8d, %r8d
  testl $4096, %edi
  sete %r8b
  leal (%r8,%r8,2), %r8d
  addl $2, %r8d
  testl %r8d, %r8d
  jne .Lte_loop2
  jmp .Lte_done
.Lte_b5:
  movl %edi, %r8d
  shrl $6, %r8d
  subl %r8d, %eax
  xorl %r8d, %r8d
  testb $56, %al
  setne %r8b
  testl %r8d, %r8d
  jne .Lte_loop2
  jmp .Lte_done
.Lte_b2:
  movl %eax, %r8d
  shll $5, %r8d
  addl %edi, %eax
  addl %r8d, %eax
  xorl %r8d, %r8d
  testb %dil, %dil
  setns %r8b
  addl $3, %r8d
  testl %r8d, %r8d
  jne .Lte_loop2
  jmp .Lte_done
.Lte_b3:
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
  jne .Lte_loop2
  jmp .Lte_done
.Lte_done:
  addl %ecx, %eax
  addl %edx, %eax
  retq
  .size jt_two_machines_escape, .-jt_two_machines_escape

  .section .rodata,"a",@progbits
  .p2align 2
.Lte_table1:
  .long .Lte_a1-.Lte_table1
  .long .Lte_a2-.Lte_table1
  .long .Lte_a3-.Lte_table1
  .long .Lte_a4-.Lte_table1
  .long .Lte_a5-.Lte_table1
.Lte_table2:
  .long .Lte_b1-.Lte_table2
  .long .Lte_b2-.Lte_table2
  .long .Lte_b3-.Lte_table2
  .long .Lte_b4-.Lte_table2
  .long .Lte_b5-.Lte_table2
  .section .note.GNU-stack,"",@progbits
