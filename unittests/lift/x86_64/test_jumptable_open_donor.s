# A first machine whose entry dispatch can never close: one of its cases
# dispatches through the same table with an index loaded from memory.  The
# second machine's constant entry dispatch is reached through the first
# entry dispatch's edge without passing that dispatch, so its singleton reads
# the first machine's provisional edge, which never becomes a published edge.
# Nothing past the first entry dispatch may become reachable.

  .text
  .globl jt_open_donor
  .type jt_open_donor, @function
  .p2align 4
jt_open_donor:
  movl $1, %ecx
  leal -1(%rcx), %eax
  leaq .Lod_table1(%rip), %rdx
  movslq (%rdx,%rax,4), %rsi
  addq %rdx, %rsi
  jmpq *%rsi
.Lod_a1:
  testl %edi, %edi
  je .Lod_machine2
  movl (%r9), %eax
  movslq (%rdx,%rax,4), %rsi
  addq %rdx, %rsi
  jmpq *%rsi
.Lod_a2:
  movl $0x5a02, %eax
  retq
.Lod_a3:
  movl $0x5a03, %eax
  retq
.Lod_a4:
  movl $0x5a04, %eax
  retq
.Lod_a5:
  movl $0x5a05, %eax
  retq
.Lod_machine2:
  movl $1, %edx
  leal -1(%rdx), %r8d
  leaq .Lod_table2(%rip), %rsi
  movslq (%rsi,%r8,4), %r8
  addq %rsi, %r8
  jmpq *%r8
.Lod_b1:
  movl $0x5b01, %eax
  retq
.Lod_b2:
  movl $0x5b02, %eax
  retq
.Lod_b3:
  movl $0x5b03, %eax
  retq
.Lod_b4:
  movl $0x5b04, %eax
  retq
.Lod_b5:
  movl $0x5b05, %eax
  retq
  .size jt_open_donor, .-jt_open_donor

  .section .rodata,"a",@progbits
  .p2align 2
.Lod_table1:
  .long .Lod_a1-.Lod_table1
  .long .Lod_a2-.Lod_table1
  .long .Lod_a3-.Lod_table1
  .long .Lod_a4-.Lod_table1
  .long .Lod_a5-.Lod_table1
.Lod_table2:
  .long .Lod_b1-.Lod_table2
  .long .Lod_b2-.Lod_table2
  .long .Lod_b3-.Lod_table2
  .long .Lod_b4-.Lod_table2
  .long .Lod_b5-.Lod_table2
  .section .note.GNU-stack,"",@progbits
