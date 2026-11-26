# A byte-guarded dispatch over more cases than a signed byte holds (GCC -O2
# shape, as in sqlite3VdbeExec): `cmpb $0x81, %dl; ja` bounds the byte lane,
# and the table index is that lane zero-extended (`movzbl %dl, %eax`), so all
# 130 slots form the domain.  The other dispatches must stay unresolved:
# jt_narrow_sext indexes with the lane sign-extended (`movsbq %dl, %rax`),
# jt_narrow_sext_before sign-extends the byte into the compared register
# before the compare and indexes with that register, jt_narrow_sextl
# sign-extends the lane to 32 bits before the implicit zero-extend, and
# jt_copied_lane copies the byte into the index's low lane while the rest of
# the index register keeps the caller's bits, so even its 128-slot bound does
# not bound the index.  A plain word separates the tables' relocation runs.

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

  .text
  .globl jt_narrow_sext_before
  .type jt_narrow_sext_before, @function
  .p2align 4
jt_narrow_sext_before:
  movsbq (%rdi), %rdx
  cmpb $0x81, %dl
  ja .Lsb_default
  leaq .Lsb_table(%rip), %rcx
  movslq (%rcx,%rdx,4), %rax
  addq %rcx, %rax
  jmpq *%rax
.Lsb_c0:
  movl $10, %eax
  retq
.Lsb_c1:
  movl $11, %eax
  retq
.Lsb_c2:
  movl $12, %eax
  retq
.Lsb_c3:
  movl $13, %eax
  retq
.Lsb_default:
  xorl %eax, %eax
  retq
  .size jt_narrow_sext_before, .-jt_narrow_sext_before

  .text
  .globl jt_narrow_sextl
  .type jt_narrow_sextl, @function
  .p2align 4
jt_narrow_sextl:
  movzbl (%rdi), %edx
  cmpb $0x81, %dl
  ja .Lsl_default
  leaq .Lsl_table(%rip), %rcx
  movsbl %dl, %eax
  movslq (%rcx,%rax,4), %rax
  addq %rcx, %rax
  jmpq *%rax
.Lsl_c0:
  movl $10, %eax
  retq
.Lsl_c1:
  movl $11, %eax
  retq
.Lsl_c2:
  movl $12, %eax
  retq
.Lsl_c3:
  movl $13, %eax
  retq
.Lsl_default:
  xorl %eax, %eax
  retq
  .size jt_narrow_sextl, .-jt_narrow_sextl

  .text
  .globl jt_copied_lane
  .type jt_copied_lane, @function
  .p2align 4
jt_copied_lane:
  movq %rsi, %rax
  movzbl (%rdi), %edx
  movb %dl, %al
  cmpb $0x7f, %dl
  ja .Lcl_default
  leaq .Lcl_table(%rip), %rcx
  movslq (%rcx,%rax,4), %rax
  addq %rcx, %rax
  jmpq *%rax
.Lcl_c0:
  movl $10, %eax
  retq
.Lcl_c1:
  movl $11, %eax
  retq
.Lcl_c2:
  movl $12, %eax
  retq
.Lcl_c3:
  movl $13, %eax
  retq
.Lcl_default:
  xorl %eax, %eax
  retq
  .size jt_copied_lane, .-jt_copied_lane

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
.Lsb_table:
  .long .Lsb_c0-.Lsb_table
  .long .Lsb_c1-.Lsb_table
  .long .Lsb_c2-.Lsb_table
  .long .Lsb_c3-.Lsb_table
  .long .Lsb_c0-.Lsb_table
  .long .Lsb_c1-.Lsb_table
  .long .Lsb_c2-.Lsb_table
  .long .Lsb_c3-.Lsb_table
  .long .Lsb_c0-.Lsb_table
  .long .Lsb_c1-.Lsb_table
  .long .Lsb_c2-.Lsb_table
  .long .Lsb_c3-.Lsb_table
  .long .Lsb_c0-.Lsb_table
  .long .Lsb_c1-.Lsb_table
  .long .Lsb_c2-.Lsb_table
  .long .Lsb_c3-.Lsb_table
  .long .Lsb_c0-.Lsb_table
  .long .Lsb_c1-.Lsb_table
  .long .Lsb_c2-.Lsb_table
  .long .Lsb_c3-.Lsb_table
  .long .Lsb_c0-.Lsb_table
  .long .Lsb_c1-.Lsb_table
  .long .Lsb_c2-.Lsb_table
  .long .Lsb_c3-.Lsb_table
  .long .Lsb_c0-.Lsb_table
  .long .Lsb_c1-.Lsb_table
  .long .Lsb_c2-.Lsb_table
  .long .Lsb_c3-.Lsb_table
  .long .Lsb_c0-.Lsb_table
  .long .Lsb_c1-.Lsb_table
  .long .Lsb_c2-.Lsb_table
  .long .Lsb_c3-.Lsb_table
  .long .Lsb_c0-.Lsb_table
  .long .Lsb_c1-.Lsb_table
  .long .Lsb_c2-.Lsb_table
  .long .Lsb_c3-.Lsb_table
  .long .Lsb_c0-.Lsb_table
  .long .Lsb_c1-.Lsb_table
  .long .Lsb_c2-.Lsb_table
  .long .Lsb_c3-.Lsb_table
  .long .Lsb_c0-.Lsb_table
  .long .Lsb_c1-.Lsb_table
  .long .Lsb_c2-.Lsb_table
  .long .Lsb_c3-.Lsb_table
  .long .Lsb_c0-.Lsb_table
  .long .Lsb_c1-.Lsb_table
  .long .Lsb_c2-.Lsb_table
  .long .Lsb_c3-.Lsb_table
  .long .Lsb_c0-.Lsb_table
  .long .Lsb_c1-.Lsb_table
  .long .Lsb_c2-.Lsb_table
  .long .Lsb_c3-.Lsb_table
  .long .Lsb_c0-.Lsb_table
  .long .Lsb_c1-.Lsb_table
  .long .Lsb_c2-.Lsb_table
  .long .Lsb_c3-.Lsb_table
  .long .Lsb_c0-.Lsb_table
  .long .Lsb_c1-.Lsb_table
  .long .Lsb_c2-.Lsb_table
  .long .Lsb_c3-.Lsb_table
  .long .Lsb_c0-.Lsb_table
  .long .Lsb_c1-.Lsb_table
  .long .Lsb_c2-.Lsb_table
  .long .Lsb_c3-.Lsb_table
  .long .Lsb_c0-.Lsb_table
  .long .Lsb_c1-.Lsb_table
  .long .Lsb_c2-.Lsb_table
  .long .Lsb_c3-.Lsb_table
  .long .Lsb_c0-.Lsb_table
  .long .Lsb_c1-.Lsb_table
  .long .Lsb_c2-.Lsb_table
  .long .Lsb_c3-.Lsb_table
  .long .Lsb_c0-.Lsb_table
  .long .Lsb_c1-.Lsb_table
  .long .Lsb_c2-.Lsb_table
  .long .Lsb_c3-.Lsb_table
  .long .Lsb_c0-.Lsb_table
  .long .Lsb_c1-.Lsb_table
  .long .Lsb_c2-.Lsb_table
  .long .Lsb_c3-.Lsb_table
  .long .Lsb_c0-.Lsb_table
  .long .Lsb_c1-.Lsb_table
  .long .Lsb_c2-.Lsb_table
  .long .Lsb_c3-.Lsb_table
  .long .Lsb_c0-.Lsb_table
  .long .Lsb_c1-.Lsb_table
  .long .Lsb_c2-.Lsb_table
  .long .Lsb_c3-.Lsb_table
  .long .Lsb_c0-.Lsb_table
  .long .Lsb_c1-.Lsb_table
  .long .Lsb_c2-.Lsb_table
  .long .Lsb_c3-.Lsb_table
  .long .Lsb_c0-.Lsb_table
  .long .Lsb_c1-.Lsb_table
  .long .Lsb_c2-.Lsb_table
  .long .Lsb_c3-.Lsb_table
  .long .Lsb_c0-.Lsb_table
  .long .Lsb_c1-.Lsb_table
  .long .Lsb_c2-.Lsb_table
  .long .Lsb_c3-.Lsb_table
  .long .Lsb_c0-.Lsb_table
  .long .Lsb_c1-.Lsb_table
  .long .Lsb_c2-.Lsb_table
  .long .Lsb_c3-.Lsb_table
  .long .Lsb_c0-.Lsb_table
  .long .Lsb_c1-.Lsb_table
  .long .Lsb_c2-.Lsb_table
  .long .Lsb_c3-.Lsb_table
  .long .Lsb_c0-.Lsb_table
  .long .Lsb_c1-.Lsb_table
  .long .Lsb_c2-.Lsb_table
  .long .Lsb_c3-.Lsb_table
  .long .Lsb_c0-.Lsb_table
  .long .Lsb_c1-.Lsb_table
  .long .Lsb_c2-.Lsb_table
  .long .Lsb_c3-.Lsb_table
  .long .Lsb_c0-.Lsb_table
  .long .Lsb_c1-.Lsb_table
  .long .Lsb_c2-.Lsb_table
  .long .Lsb_c3-.Lsb_table
  .long .Lsb_c0-.Lsb_table
  .long .Lsb_c1-.Lsb_table
  .long .Lsb_c2-.Lsb_table
  .long .Lsb_c3-.Lsb_table
  .long .Lsb_c0-.Lsb_table
  .long .Lsb_c1-.Lsb_table
  .long .Lsb_c2-.Lsb_table
  .long .Lsb_c3-.Lsb_table
  .long .Lsb_c0-.Lsb_table
  .long .Lsb_c1-.Lsb_table
  .long 0
.Lsl_table:
  .long .Lsl_c0-.Lsl_table
  .long .Lsl_c1-.Lsl_table
  .long .Lsl_c2-.Lsl_table
  .long .Lsl_c3-.Lsl_table
  .long .Lsl_c0-.Lsl_table
  .long .Lsl_c1-.Lsl_table
  .long .Lsl_c2-.Lsl_table
  .long .Lsl_c3-.Lsl_table
  .long .Lsl_c0-.Lsl_table
  .long .Lsl_c1-.Lsl_table
  .long .Lsl_c2-.Lsl_table
  .long .Lsl_c3-.Lsl_table
  .long .Lsl_c0-.Lsl_table
  .long .Lsl_c1-.Lsl_table
  .long .Lsl_c2-.Lsl_table
  .long .Lsl_c3-.Lsl_table
  .long .Lsl_c0-.Lsl_table
  .long .Lsl_c1-.Lsl_table
  .long .Lsl_c2-.Lsl_table
  .long .Lsl_c3-.Lsl_table
  .long .Lsl_c0-.Lsl_table
  .long .Lsl_c1-.Lsl_table
  .long .Lsl_c2-.Lsl_table
  .long .Lsl_c3-.Lsl_table
  .long .Lsl_c0-.Lsl_table
  .long .Lsl_c1-.Lsl_table
  .long .Lsl_c2-.Lsl_table
  .long .Lsl_c3-.Lsl_table
  .long .Lsl_c0-.Lsl_table
  .long .Lsl_c1-.Lsl_table
  .long .Lsl_c2-.Lsl_table
  .long .Lsl_c3-.Lsl_table
  .long .Lsl_c0-.Lsl_table
  .long .Lsl_c1-.Lsl_table
  .long .Lsl_c2-.Lsl_table
  .long .Lsl_c3-.Lsl_table
  .long .Lsl_c0-.Lsl_table
  .long .Lsl_c1-.Lsl_table
  .long .Lsl_c2-.Lsl_table
  .long .Lsl_c3-.Lsl_table
  .long .Lsl_c0-.Lsl_table
  .long .Lsl_c1-.Lsl_table
  .long .Lsl_c2-.Lsl_table
  .long .Lsl_c3-.Lsl_table
  .long .Lsl_c0-.Lsl_table
  .long .Lsl_c1-.Lsl_table
  .long .Lsl_c2-.Lsl_table
  .long .Lsl_c3-.Lsl_table
  .long .Lsl_c0-.Lsl_table
  .long .Lsl_c1-.Lsl_table
  .long .Lsl_c2-.Lsl_table
  .long .Lsl_c3-.Lsl_table
  .long .Lsl_c0-.Lsl_table
  .long .Lsl_c1-.Lsl_table
  .long .Lsl_c2-.Lsl_table
  .long .Lsl_c3-.Lsl_table
  .long .Lsl_c0-.Lsl_table
  .long .Lsl_c1-.Lsl_table
  .long .Lsl_c2-.Lsl_table
  .long .Lsl_c3-.Lsl_table
  .long .Lsl_c0-.Lsl_table
  .long .Lsl_c1-.Lsl_table
  .long .Lsl_c2-.Lsl_table
  .long .Lsl_c3-.Lsl_table
  .long .Lsl_c0-.Lsl_table
  .long .Lsl_c1-.Lsl_table
  .long .Lsl_c2-.Lsl_table
  .long .Lsl_c3-.Lsl_table
  .long .Lsl_c0-.Lsl_table
  .long .Lsl_c1-.Lsl_table
  .long .Lsl_c2-.Lsl_table
  .long .Lsl_c3-.Lsl_table
  .long .Lsl_c0-.Lsl_table
  .long .Lsl_c1-.Lsl_table
  .long .Lsl_c2-.Lsl_table
  .long .Lsl_c3-.Lsl_table
  .long .Lsl_c0-.Lsl_table
  .long .Lsl_c1-.Lsl_table
  .long .Lsl_c2-.Lsl_table
  .long .Lsl_c3-.Lsl_table
  .long .Lsl_c0-.Lsl_table
  .long .Lsl_c1-.Lsl_table
  .long .Lsl_c2-.Lsl_table
  .long .Lsl_c3-.Lsl_table
  .long .Lsl_c0-.Lsl_table
  .long .Lsl_c1-.Lsl_table
  .long .Lsl_c2-.Lsl_table
  .long .Lsl_c3-.Lsl_table
  .long .Lsl_c0-.Lsl_table
  .long .Lsl_c1-.Lsl_table
  .long .Lsl_c2-.Lsl_table
  .long .Lsl_c3-.Lsl_table
  .long .Lsl_c0-.Lsl_table
  .long .Lsl_c1-.Lsl_table
  .long .Lsl_c2-.Lsl_table
  .long .Lsl_c3-.Lsl_table
  .long .Lsl_c0-.Lsl_table
  .long .Lsl_c1-.Lsl_table
  .long .Lsl_c2-.Lsl_table
  .long .Lsl_c3-.Lsl_table
  .long .Lsl_c0-.Lsl_table
  .long .Lsl_c1-.Lsl_table
  .long .Lsl_c2-.Lsl_table
  .long .Lsl_c3-.Lsl_table
  .long .Lsl_c0-.Lsl_table
  .long .Lsl_c1-.Lsl_table
  .long .Lsl_c2-.Lsl_table
  .long .Lsl_c3-.Lsl_table
  .long .Lsl_c0-.Lsl_table
  .long .Lsl_c1-.Lsl_table
  .long .Lsl_c2-.Lsl_table
  .long .Lsl_c3-.Lsl_table
  .long .Lsl_c0-.Lsl_table
  .long .Lsl_c1-.Lsl_table
  .long .Lsl_c2-.Lsl_table
  .long .Lsl_c3-.Lsl_table
  .long .Lsl_c0-.Lsl_table
  .long .Lsl_c1-.Lsl_table
  .long .Lsl_c2-.Lsl_table
  .long .Lsl_c3-.Lsl_table
  .long .Lsl_c0-.Lsl_table
  .long .Lsl_c1-.Lsl_table
  .long .Lsl_c2-.Lsl_table
  .long .Lsl_c3-.Lsl_table
  .long .Lsl_c0-.Lsl_table
  .long .Lsl_c1-.Lsl_table
  .long .Lsl_c2-.Lsl_table
  .long .Lsl_c3-.Lsl_table
  .long .Lsl_c0-.Lsl_table
  .long .Lsl_c1-.Lsl_table
  .long 0
.Lcl_table:
  .long .Lcl_c0-.Lcl_table
  .long .Lcl_c1-.Lcl_table
  .long .Lcl_c2-.Lcl_table
  .long .Lcl_c3-.Lcl_table
  .long .Lcl_c0-.Lcl_table
  .long .Lcl_c1-.Lcl_table
  .long .Lcl_c2-.Lcl_table
  .long .Lcl_c3-.Lcl_table
  .long .Lcl_c0-.Lcl_table
  .long .Lcl_c1-.Lcl_table
  .long .Lcl_c2-.Lcl_table
  .long .Lcl_c3-.Lcl_table
  .long .Lcl_c0-.Lcl_table
  .long .Lcl_c1-.Lcl_table
  .long .Lcl_c2-.Lcl_table
  .long .Lcl_c3-.Lcl_table
  .long .Lcl_c0-.Lcl_table
  .long .Lcl_c1-.Lcl_table
  .long .Lcl_c2-.Lcl_table
  .long .Lcl_c3-.Lcl_table
  .long .Lcl_c0-.Lcl_table
  .long .Lcl_c1-.Lcl_table
  .long .Lcl_c2-.Lcl_table
  .long .Lcl_c3-.Lcl_table
  .long .Lcl_c0-.Lcl_table
  .long .Lcl_c1-.Lcl_table
  .long .Lcl_c2-.Lcl_table
  .long .Lcl_c3-.Lcl_table
  .long .Lcl_c0-.Lcl_table
  .long .Lcl_c1-.Lcl_table
  .long .Lcl_c2-.Lcl_table
  .long .Lcl_c3-.Lcl_table
  .long .Lcl_c0-.Lcl_table
  .long .Lcl_c1-.Lcl_table
  .long .Lcl_c2-.Lcl_table
  .long .Lcl_c3-.Lcl_table
  .long .Lcl_c0-.Lcl_table
  .long .Lcl_c1-.Lcl_table
  .long .Lcl_c2-.Lcl_table
  .long .Lcl_c3-.Lcl_table
  .long .Lcl_c0-.Lcl_table
  .long .Lcl_c1-.Lcl_table
  .long .Lcl_c2-.Lcl_table
  .long .Lcl_c3-.Lcl_table
  .long .Lcl_c0-.Lcl_table
  .long .Lcl_c1-.Lcl_table
  .long .Lcl_c2-.Lcl_table
  .long .Lcl_c3-.Lcl_table
  .long .Lcl_c0-.Lcl_table
  .long .Lcl_c1-.Lcl_table
  .long .Lcl_c2-.Lcl_table
  .long .Lcl_c3-.Lcl_table
  .long .Lcl_c0-.Lcl_table
  .long .Lcl_c1-.Lcl_table
  .long .Lcl_c2-.Lcl_table
  .long .Lcl_c3-.Lcl_table
  .long .Lcl_c0-.Lcl_table
  .long .Lcl_c1-.Lcl_table
  .long .Lcl_c2-.Lcl_table
  .long .Lcl_c3-.Lcl_table
  .long .Lcl_c0-.Lcl_table
  .long .Lcl_c1-.Lcl_table
  .long .Lcl_c2-.Lcl_table
  .long .Lcl_c3-.Lcl_table
  .long .Lcl_c0-.Lcl_table
  .long .Lcl_c1-.Lcl_table
  .long .Lcl_c2-.Lcl_table
  .long .Lcl_c3-.Lcl_table
  .long .Lcl_c0-.Lcl_table
  .long .Lcl_c1-.Lcl_table
  .long .Lcl_c2-.Lcl_table
  .long .Lcl_c3-.Lcl_table
  .long .Lcl_c0-.Lcl_table
  .long .Lcl_c1-.Lcl_table
  .long .Lcl_c2-.Lcl_table
  .long .Lcl_c3-.Lcl_table
  .long .Lcl_c0-.Lcl_table
  .long .Lcl_c1-.Lcl_table
  .long .Lcl_c2-.Lcl_table
  .long .Lcl_c3-.Lcl_table
  .long .Lcl_c0-.Lcl_table
  .long .Lcl_c1-.Lcl_table
  .long .Lcl_c2-.Lcl_table
  .long .Lcl_c3-.Lcl_table
  .long .Lcl_c0-.Lcl_table
  .long .Lcl_c1-.Lcl_table
  .long .Lcl_c2-.Lcl_table
  .long .Lcl_c3-.Lcl_table
  .long .Lcl_c0-.Lcl_table
  .long .Lcl_c1-.Lcl_table
  .long .Lcl_c2-.Lcl_table
  .long .Lcl_c3-.Lcl_table
  .long .Lcl_c0-.Lcl_table
  .long .Lcl_c1-.Lcl_table
  .long .Lcl_c2-.Lcl_table
  .long .Lcl_c3-.Lcl_table
  .long .Lcl_c0-.Lcl_table
  .long .Lcl_c1-.Lcl_table
  .long .Lcl_c2-.Lcl_table
  .long .Lcl_c3-.Lcl_table
  .long .Lcl_c0-.Lcl_table
  .long .Lcl_c1-.Lcl_table
  .long .Lcl_c2-.Lcl_table
  .long .Lcl_c3-.Lcl_table
  .long .Lcl_c0-.Lcl_table
  .long .Lcl_c1-.Lcl_table
  .long .Lcl_c2-.Lcl_table
  .long .Lcl_c3-.Lcl_table
  .long .Lcl_c0-.Lcl_table
  .long .Lcl_c1-.Lcl_table
  .long .Lcl_c2-.Lcl_table
  .long .Lcl_c3-.Lcl_table
  .long .Lcl_c0-.Lcl_table
  .long .Lcl_c1-.Lcl_table
  .long .Lcl_c2-.Lcl_table
  .long .Lcl_c3-.Lcl_table
  .long .Lcl_c0-.Lcl_table
  .long .Lcl_c1-.Lcl_table
  .long .Lcl_c2-.Lcl_table
  .long .Lcl_c3-.Lcl_table
  .long .Lcl_c0-.Lcl_table
  .long .Lcl_c1-.Lcl_table
  .long .Lcl_c2-.Lcl_table
  .long .Lcl_c3-.Lcl_table
  .long .Lcl_c0-.Lcl_table
  .long .Lcl_c1-.Lcl_table
  .long .Lcl_c2-.Lcl_table
  .long .Lcl_c3-.Lcl_table
  .long 0
  .section .note.GNU-stack,"",@progbits
