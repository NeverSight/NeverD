# A byte-guarded dispatch over more cases than a signed byte holds (GCC -O2
# shape, as in sqlite3VdbeExec): `cmpb $0x81, %dl; ja` bounds the byte lane,
# and the table index is that lane zero-extended (`movzbl %dl, %eax`), so all
# 130 slots form the domain.  jt_narrow_sext indexes with the lane
# sign-extended instead (`movsbq %dl, %rax`): lane values 0x80 and 0x81 become
# negative indices, the byte bound does not bound the index, and no table may
# be published.  A plain word separates the two tables' relocation runs.

  .text
  .globl jt_narrow_zext
  .type jt_narrow_zext, @function
  .p2align 4
jt_narrow_zext:
  movzbl (%rdi), %edx
  cmpb $0x81, %dl
  ja .Lnz_default
  leaq .Lnz_table(%rip), %rcx
  movzbl %dl, %eax
  movslq (%rcx,%rax,4), %rax
  addq %rcx, %rax
  jmpq *%rax
.Lnz_c0:
  movl $10, %eax
  retq
.Lnz_c1:
  movl $11, %eax
  retq
.Lnz_c2:
  movl $12, %eax
  retq
.Lnz_c3:
  movl $13, %eax
  retq
.Lnz_default:
  xorl %eax, %eax
  retq
  .size jt_narrow_zext, .-jt_narrow_zext

  .text
  .globl jt_narrow_sext
  .type jt_narrow_sext, @function
  .p2align 4
jt_narrow_sext:
  movzbl (%rdi), %edx
  cmpb $0x81, %dl
  ja .Lns_default
  leaq .Lns_table(%rip), %rcx
  movsbq %dl, %rax
  movslq (%rcx,%rax,4), %rax
  addq %rcx, %rax
  jmpq *%rax
.Lns_c0:
  movl $10, %eax
  retq
.Lns_c1:
  movl $11, %eax
  retq
.Lns_c2:
  movl $12, %eax
  retq
.Lns_c3:
  movl $13, %eax
  retq
.Lns_default:
  xorl %eax, %eax
  retq
  .size jt_narrow_sext, .-jt_narrow_sext

  .section .rodata,"a",@progbits
  .p2align 2
.Lnz_table:
  .long .Lnz_c0-.Lnz_table
  .long .Lnz_c1-.Lnz_table
  .long .Lnz_c2-.Lnz_table
  .long .Lnz_c3-.Lnz_table
  .long .Lnz_c0-.Lnz_table
  .long .Lnz_c1-.Lnz_table
  .long .Lnz_c2-.Lnz_table
  .long .Lnz_c3-.Lnz_table
  .long .Lnz_c0-.Lnz_table
  .long .Lnz_c1-.Lnz_table
  .long .Lnz_c2-.Lnz_table
  .long .Lnz_c3-.Lnz_table
  .long .Lnz_c0-.Lnz_table
  .long .Lnz_c1-.Lnz_table
  .long .Lnz_c2-.Lnz_table
  .long .Lnz_c3-.Lnz_table
  .long .Lnz_c0-.Lnz_table
  .long .Lnz_c1-.Lnz_table
  .long .Lnz_c2-.Lnz_table
  .long .Lnz_c3-.Lnz_table
  .long .Lnz_c0-.Lnz_table
  .long .Lnz_c1-.Lnz_table
  .long .Lnz_c2-.Lnz_table
  .long .Lnz_c3-.Lnz_table
  .long .Lnz_c0-.Lnz_table
  .long .Lnz_c1-.Lnz_table
  .long .Lnz_c2-.Lnz_table
  .long .Lnz_c3-.Lnz_table
  .long .Lnz_c0-.Lnz_table
  .long .Lnz_c1-.Lnz_table
  .long .Lnz_c2-.Lnz_table
  .long .Lnz_c3-.Lnz_table
  .long .Lnz_c0-.Lnz_table
  .long .Lnz_c1-.Lnz_table
  .long .Lnz_c2-.Lnz_table
  .long .Lnz_c3-.Lnz_table
  .long .Lnz_c0-.Lnz_table
  .long .Lnz_c1-.Lnz_table
  .long .Lnz_c2-.Lnz_table
  .long .Lnz_c3-.Lnz_table
  .long .Lnz_c0-.Lnz_table
  .long .Lnz_c1-.Lnz_table
  .long .Lnz_c2-.Lnz_table
  .long .Lnz_c3-.Lnz_table
  .long .Lnz_c0-.Lnz_table
  .long .Lnz_c1-.Lnz_table
  .long .Lnz_c2-.Lnz_table
  .long .Lnz_c3-.Lnz_table
  .long .Lnz_c0-.Lnz_table
  .long .Lnz_c1-.Lnz_table
  .long .Lnz_c2-.Lnz_table
  .long .Lnz_c3-.Lnz_table
  .long .Lnz_c0-.Lnz_table
  .long .Lnz_c1-.Lnz_table
  .long .Lnz_c2-.Lnz_table
  .long .Lnz_c3-.Lnz_table
  .long .Lnz_c0-.Lnz_table
  .long .Lnz_c1-.Lnz_table
  .long .Lnz_c2-.Lnz_table
  .long .Lnz_c3-.Lnz_table
  .long .Lnz_c0-.Lnz_table
  .long .Lnz_c1-.Lnz_table
  .long .Lnz_c2-.Lnz_table
  .long .Lnz_c3-.Lnz_table
  .long .Lnz_c0-.Lnz_table
  .long .Lnz_c1-.Lnz_table
  .long .Lnz_c2-.Lnz_table
  .long .Lnz_c3-.Lnz_table
  .long .Lnz_c0-.Lnz_table
  .long .Lnz_c1-.Lnz_table
  .long .Lnz_c2-.Lnz_table
  .long .Lnz_c3-.Lnz_table
  .long .Lnz_c0-.Lnz_table
  .long .Lnz_c1-.Lnz_table
  .long .Lnz_c2-.Lnz_table
  .long .Lnz_c3-.Lnz_table
  .long .Lnz_c0-.Lnz_table
  .long .Lnz_c1-.Lnz_table
  .long .Lnz_c2-.Lnz_table
  .long .Lnz_c3-.Lnz_table
  .long .Lnz_c0-.Lnz_table
  .long .Lnz_c1-.Lnz_table
  .long .Lnz_c2-.Lnz_table
  .long .Lnz_c3-.Lnz_table
  .long .Lnz_c0-.Lnz_table
  .long .Lnz_c1-.Lnz_table
  .long .Lnz_c2-.Lnz_table
  .long .Lnz_c3-.Lnz_table
  .long .Lnz_c0-.Lnz_table
  .long .Lnz_c1-.Lnz_table
  .long .Lnz_c2-.Lnz_table
  .long .Lnz_c3-.Lnz_table
  .long .Lnz_c0-.Lnz_table
  .long .Lnz_c1-.Lnz_table
  .long .Lnz_c2-.Lnz_table
  .long .Lnz_c3-.Lnz_table
  .long .Lnz_c0-.Lnz_table
  .long .Lnz_c1-.Lnz_table
  .long .Lnz_c2-.Lnz_table
  .long .Lnz_c3-.Lnz_table
  .long .Lnz_c0-.Lnz_table
  .long .Lnz_c1-.Lnz_table
  .long .Lnz_c2-.Lnz_table
  .long .Lnz_c3-.Lnz_table
  .long .Lnz_c0-.Lnz_table
  .long .Lnz_c1-.Lnz_table
  .long .Lnz_c2-.Lnz_table
  .long .Lnz_c3-.Lnz_table
  .long .Lnz_c0-.Lnz_table
  .long .Lnz_c1-.Lnz_table
  .long .Lnz_c2-.Lnz_table
  .long .Lnz_c3-.Lnz_table
  .long .Lnz_c0-.Lnz_table
  .long .Lnz_c1-.Lnz_table
  .long .Lnz_c2-.Lnz_table
  .long .Lnz_c3-.Lnz_table
  .long .Lnz_c0-.Lnz_table
  .long .Lnz_c1-.Lnz_table
  .long .Lnz_c2-.Lnz_table
  .long .Lnz_c3-.Lnz_table
  .long .Lnz_c0-.Lnz_table
  .long .Lnz_c1-.Lnz_table
  .long .Lnz_c2-.Lnz_table
  .long .Lnz_c3-.Lnz_table
  .long .Lnz_c0-.Lnz_table
  .long .Lnz_c1-.Lnz_table
  .long .Lnz_c2-.Lnz_table
  .long .Lnz_c3-.Lnz_table
  .long .Lnz_c0-.Lnz_table
  .long .Lnz_c1-.Lnz_table
  .long 0
.Lns_table:
  .long .Lns_c0-.Lns_table
  .long .Lns_c1-.Lns_table
  .long .Lns_c2-.Lns_table
  .long .Lns_c3-.Lns_table
  .long .Lns_c0-.Lns_table
  .long .Lns_c1-.Lns_table
  .long .Lns_c2-.Lns_table
  .long .Lns_c3-.Lns_table
  .long .Lns_c0-.Lns_table
  .long .Lns_c1-.Lns_table
  .long .Lns_c2-.Lns_table
  .long .Lns_c3-.Lns_table
  .long .Lns_c0-.Lns_table
  .long .Lns_c1-.Lns_table
  .long .Lns_c2-.Lns_table
  .long .Lns_c3-.Lns_table
  .long .Lns_c0-.Lns_table
  .long .Lns_c1-.Lns_table
  .long .Lns_c2-.Lns_table
  .long .Lns_c3-.Lns_table
  .long .Lns_c0-.Lns_table
  .long .Lns_c1-.Lns_table
  .long .Lns_c2-.Lns_table
  .long .Lns_c3-.Lns_table
  .long .Lns_c0-.Lns_table
  .long .Lns_c1-.Lns_table
  .long .Lns_c2-.Lns_table
  .long .Lns_c3-.Lns_table
  .long .Lns_c0-.Lns_table
  .long .Lns_c1-.Lns_table
  .long .Lns_c2-.Lns_table
  .long .Lns_c3-.Lns_table
  .long .Lns_c0-.Lns_table
  .long .Lns_c1-.Lns_table
  .long .Lns_c2-.Lns_table
  .long .Lns_c3-.Lns_table
  .long .Lns_c0-.Lns_table
  .long .Lns_c1-.Lns_table
  .long .Lns_c2-.Lns_table
  .long .Lns_c3-.Lns_table
  .long .Lns_c0-.Lns_table
  .long .Lns_c1-.Lns_table
  .long .Lns_c2-.Lns_table
  .long .Lns_c3-.Lns_table
  .long .Lns_c0-.Lns_table
  .long .Lns_c1-.Lns_table
  .long .Lns_c2-.Lns_table
  .long .Lns_c3-.Lns_table
  .long .Lns_c0-.Lns_table
  .long .Lns_c1-.Lns_table
  .long .Lns_c2-.Lns_table
  .long .Lns_c3-.Lns_table
  .long .Lns_c0-.Lns_table
  .long .Lns_c1-.Lns_table
  .long .Lns_c2-.Lns_table
  .long .Lns_c3-.Lns_table
  .long .Lns_c0-.Lns_table
  .long .Lns_c1-.Lns_table
  .long .Lns_c2-.Lns_table
  .long .Lns_c3-.Lns_table
  .long .Lns_c0-.Lns_table
  .long .Lns_c1-.Lns_table
  .long .Lns_c2-.Lns_table
  .long .Lns_c3-.Lns_table
  .long .Lns_c0-.Lns_table
  .long .Lns_c1-.Lns_table
  .long .Lns_c2-.Lns_table
  .long .Lns_c3-.Lns_table
  .long .Lns_c0-.Lns_table
  .long .Lns_c1-.Lns_table
  .long .Lns_c2-.Lns_table
  .long .Lns_c3-.Lns_table
  .long .Lns_c0-.Lns_table
  .long .Lns_c1-.Lns_table
  .long .Lns_c2-.Lns_table
  .long .Lns_c3-.Lns_table
  .long .Lns_c0-.Lns_table
  .long .Lns_c1-.Lns_table
  .long .Lns_c2-.Lns_table
  .long .Lns_c3-.Lns_table
  .long .Lns_c0-.Lns_table
  .long .Lns_c1-.Lns_table
  .long .Lns_c2-.Lns_table
  .long .Lns_c3-.Lns_table
  .long .Lns_c0-.Lns_table
  .long .Lns_c1-.Lns_table
  .long .Lns_c2-.Lns_table
  .long .Lns_c3-.Lns_table
  .long .Lns_c0-.Lns_table
  .long .Lns_c1-.Lns_table
  .long .Lns_c2-.Lns_table
  .long .Lns_c3-.Lns_table
  .long .Lns_c0-.Lns_table
  .long .Lns_c1-.Lns_table
  .long .Lns_c2-.Lns_table
  .long .Lns_c3-.Lns_table
  .long .Lns_c0-.Lns_table
  .long .Lns_c1-.Lns_table
  .long .Lns_c2-.Lns_table
  .long .Lns_c3-.Lns_table
  .long .Lns_c0-.Lns_table
  .long .Lns_c1-.Lns_table
  .long .Lns_c2-.Lns_table
  .long .Lns_c3-.Lns_table
  .long .Lns_c0-.Lns_table
  .long .Lns_c1-.Lns_table
  .long .Lns_c2-.Lns_table
  .long .Lns_c3-.Lns_table
  .long .Lns_c0-.Lns_table
  .long .Lns_c1-.Lns_table
  .long .Lns_c2-.Lns_table
  .long .Lns_c3-.Lns_table
  .long .Lns_c0-.Lns_table
  .long .Lns_c1-.Lns_table
  .long .Lns_c2-.Lns_table
  .long .Lns_c3-.Lns_table
  .long .Lns_c0-.Lns_table
  .long .Lns_c1-.Lns_table
  .long .Lns_c2-.Lns_table
  .long .Lns_c3-.Lns_table
  .long .Lns_c0-.Lns_table
  .long .Lns_c1-.Lns_table
  .long .Lns_c2-.Lns_table
  .long .Lns_c3-.Lns_table
  .long .Lns_c0-.Lns_table
  .long .Lns_c1-.Lns_table
  .long .Lns_c2-.Lns_table
  .long .Lns_c3-.Lns_table
  .long .Lns_c0-.Lns_table
  .long .Lns_c1-.Lns_table
  .long 0
  .section .note.GNU-stack,"",@progbits
