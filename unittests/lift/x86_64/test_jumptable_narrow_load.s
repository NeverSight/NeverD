# Byte-guarded dispatches over more cases than a signed byte holds, whose
# index register is the one the byte was loaded into (GCC -O2 shape, as in
# pcre2's set_start_bits): `cmpb $0x81, %al; ja` bounds the low lane, so all
# 130 slots form the domain when the load zero-extends the byte into the
# index.  jt_load_movzbl loads it with `movzbl`, whose implicit upper-half
# zero-extend writes the index, and jt_load_movzbq with `movzbq`, which
# writes the whole index at once.  The other dispatches must stay
# unresolved: jt_load_cltq sign-extends the byte through `movsbl` and
# `cltq`, jt_load_movsbq loads it with `movsbq`, and jt_load_movsbl
# sign-extends it to 32 bits before the implicit zero-extend, so lane values
# 0x80 and 0x81 are no slots.  A plain word separates the tables'
# relocation runs.

  .text
  .globl jt_load_movzbl
  .type jt_load_movzbl, @function
  .p2align 4
jt_load_movzbl:
  leaq .Llz_table(%rip), %rcx
  movzbl (%rdi), %eax
  cmpb $0x81, %al
  ja .Llz_default
  movslq (%rcx,%rax,4), %rax
  addq %rcx, %rax
  jmpq *%rax
.Llz_c0:
  movl $10, %eax
  retq
.Llz_c1:
  movl $11, %eax
  retq
.Llz_c2:
  movl $12, %eax
  retq
.Llz_c3:
  movl $13, %eax
  retq
.Llz_default:
  xorl %eax, %eax
  retq
  .size jt_load_movzbl, .-jt_load_movzbl

  .text
  .globl jt_load_movzbq
  .type jt_load_movzbq, @function
  .p2align 4
jt_load_movzbq:
  leaq .Llq_table(%rip), %rcx
  movzbq (%rdi), %rax
  cmpb $0x81, %al
  ja .Llq_default
  movslq (%rcx,%rax,4), %rax
  addq %rcx, %rax
  jmpq *%rax
.Llq_c0:
  movl $10, %eax
  retq
.Llq_c1:
  movl $11, %eax
  retq
.Llq_c2:
  movl $12, %eax
  retq
.Llq_c3:
  movl $13, %eax
  retq
.Llq_default:
  xorl %eax, %eax
  retq
  .size jt_load_movzbq, .-jt_load_movzbq

  .text
  .globl jt_load_cltq
  .type jt_load_cltq, @function
  .p2align 4
jt_load_cltq:
  leaq .Llc_table(%rip), %rcx
  movsbl (%rdi), %eax
  cltq
  cmpb $0x81, %al
  ja .Llc_default
  movslq (%rcx,%rax,4), %rax
  addq %rcx, %rax
  jmpq *%rax
.Llc_c0:
  movl $10, %eax
  retq
.Llc_c1:
  movl $11, %eax
  retq
.Llc_c2:
  movl $12, %eax
  retq
.Llc_c3:
  movl $13, %eax
  retq
.Llc_default:
  xorl %eax, %eax
  retq
  .size jt_load_cltq, .-jt_load_cltq

  .text
  .globl jt_load_movsbq
  .type jt_load_movsbq, @function
  .p2align 4
jt_load_movsbq:
  leaq .Lls_table(%rip), %rcx
  movsbq (%rdi), %rax
  cmpb $0x81, %al
  ja .Lls_default
  movslq (%rcx,%rax,4), %rax
  addq %rcx, %rax
  jmpq *%rax
.Lls_c0:
  movl $10, %eax
  retq
.Lls_c1:
  movl $11, %eax
  retq
.Lls_c2:
  movl $12, %eax
  retq
.Lls_c3:
  movl $13, %eax
  retq
.Lls_default:
  xorl %eax, %eax
  retq
  .size jt_load_movsbq, .-jt_load_movsbq

  .text
  .globl jt_load_movsbl
  .type jt_load_movsbl, @function
  .p2align 4
jt_load_movsbl:
  leaq .Lll_table(%rip), %rcx
  movsbl (%rdi), %eax
  cmpb $0x81, %al
  ja .Lll_default
  movslq (%rcx,%rax,4), %rax
  addq %rcx, %rax
  jmpq *%rax
.Lll_c0:
  movl $10, %eax
  retq
.Lll_c1:
  movl $11, %eax
  retq
.Lll_c2:
  movl $12, %eax
  retq
.Lll_c3:
  movl $13, %eax
  retq
.Lll_default:
  xorl %eax, %eax
  retq
  .size jt_load_movsbl, .-jt_load_movsbl

  .section .rodata,"a",@progbits
  .p2align 2
.Llz_table:
  .long .Llz_c0-.Llz_table
  .long .Llz_c1-.Llz_table
  .long .Llz_c2-.Llz_table
  .long .Llz_c3-.Llz_table
  .long .Llz_c0-.Llz_table
  .long .Llz_c1-.Llz_table
  .long .Llz_c2-.Llz_table
  .long .Llz_c3-.Llz_table
  .long .Llz_c0-.Llz_table
  .long .Llz_c1-.Llz_table
  .long .Llz_c2-.Llz_table
  .long .Llz_c3-.Llz_table
  .long .Llz_c0-.Llz_table
  .long .Llz_c1-.Llz_table
  .long .Llz_c2-.Llz_table
  .long .Llz_c3-.Llz_table
  .long .Llz_c0-.Llz_table
  .long .Llz_c1-.Llz_table
  .long .Llz_c2-.Llz_table
  .long .Llz_c3-.Llz_table
  .long .Llz_c0-.Llz_table
  .long .Llz_c1-.Llz_table
  .long .Llz_c2-.Llz_table
  .long .Llz_c3-.Llz_table
  .long .Llz_c0-.Llz_table
  .long .Llz_c1-.Llz_table
  .long .Llz_c2-.Llz_table
  .long .Llz_c3-.Llz_table
  .long .Llz_c0-.Llz_table
  .long .Llz_c1-.Llz_table
  .long .Llz_c2-.Llz_table
  .long .Llz_c3-.Llz_table
  .long .Llz_c0-.Llz_table
  .long .Llz_c1-.Llz_table
  .long .Llz_c2-.Llz_table
  .long .Llz_c3-.Llz_table
  .long .Llz_c0-.Llz_table
  .long .Llz_c1-.Llz_table
  .long .Llz_c2-.Llz_table
  .long .Llz_c3-.Llz_table
  .long .Llz_c0-.Llz_table
  .long .Llz_c1-.Llz_table
  .long .Llz_c2-.Llz_table
  .long .Llz_c3-.Llz_table
  .long .Llz_c0-.Llz_table
  .long .Llz_c1-.Llz_table
  .long .Llz_c2-.Llz_table
  .long .Llz_c3-.Llz_table
  .long .Llz_c0-.Llz_table
  .long .Llz_c1-.Llz_table
  .long .Llz_c2-.Llz_table
  .long .Llz_c3-.Llz_table
  .long .Llz_c0-.Llz_table
  .long .Llz_c1-.Llz_table
  .long .Llz_c2-.Llz_table
  .long .Llz_c3-.Llz_table
  .long .Llz_c0-.Llz_table
  .long .Llz_c1-.Llz_table
  .long .Llz_c2-.Llz_table
  .long .Llz_c3-.Llz_table
  .long .Llz_c0-.Llz_table
  .long .Llz_c1-.Llz_table
  .long .Llz_c2-.Llz_table
  .long .Llz_c3-.Llz_table
  .long .Llz_c0-.Llz_table
  .long .Llz_c1-.Llz_table
  .long .Llz_c2-.Llz_table
  .long .Llz_c3-.Llz_table
  .long .Llz_c0-.Llz_table
  .long .Llz_c1-.Llz_table
  .long .Llz_c2-.Llz_table
  .long .Llz_c3-.Llz_table
  .long .Llz_c0-.Llz_table
  .long .Llz_c1-.Llz_table
  .long .Llz_c2-.Llz_table
  .long .Llz_c3-.Llz_table
  .long .Llz_c0-.Llz_table
  .long .Llz_c1-.Llz_table
  .long .Llz_c2-.Llz_table
  .long .Llz_c3-.Llz_table
  .long .Llz_c0-.Llz_table
  .long .Llz_c1-.Llz_table
  .long .Llz_c2-.Llz_table
  .long .Llz_c3-.Llz_table
  .long .Llz_c0-.Llz_table
  .long .Llz_c1-.Llz_table
  .long .Llz_c2-.Llz_table
  .long .Llz_c3-.Llz_table
  .long .Llz_c0-.Llz_table
  .long .Llz_c1-.Llz_table
  .long .Llz_c2-.Llz_table
  .long .Llz_c3-.Llz_table
  .long .Llz_c0-.Llz_table
  .long .Llz_c1-.Llz_table
  .long .Llz_c2-.Llz_table
  .long .Llz_c3-.Llz_table
  .long .Llz_c0-.Llz_table
  .long .Llz_c1-.Llz_table
  .long .Llz_c2-.Llz_table
  .long .Llz_c3-.Llz_table
  .long .Llz_c0-.Llz_table
  .long .Llz_c1-.Llz_table
  .long .Llz_c2-.Llz_table
  .long .Llz_c3-.Llz_table
  .long .Llz_c0-.Llz_table
  .long .Llz_c1-.Llz_table
  .long .Llz_c2-.Llz_table
  .long .Llz_c3-.Llz_table
  .long .Llz_c0-.Llz_table
  .long .Llz_c1-.Llz_table
  .long .Llz_c2-.Llz_table
  .long .Llz_c3-.Llz_table
  .long .Llz_c0-.Llz_table
  .long .Llz_c1-.Llz_table
  .long .Llz_c2-.Llz_table
  .long .Llz_c3-.Llz_table
  .long .Llz_c0-.Llz_table
  .long .Llz_c1-.Llz_table
  .long .Llz_c2-.Llz_table
  .long .Llz_c3-.Llz_table
  .long .Llz_c0-.Llz_table
  .long .Llz_c1-.Llz_table
  .long .Llz_c2-.Llz_table
  .long .Llz_c3-.Llz_table
  .long .Llz_c0-.Llz_table
  .long .Llz_c1-.Llz_table
  .long .Llz_c2-.Llz_table
  .long .Llz_c3-.Llz_table
  .long .Llz_c0-.Llz_table
  .long .Llz_c1-.Llz_table
  .long 0
  .p2align 2
.Llq_table:
  .long .Llq_c0-.Llq_table
  .long .Llq_c1-.Llq_table
  .long .Llq_c2-.Llq_table
  .long .Llq_c3-.Llq_table
  .long .Llq_c0-.Llq_table
  .long .Llq_c1-.Llq_table
  .long .Llq_c2-.Llq_table
  .long .Llq_c3-.Llq_table
  .long .Llq_c0-.Llq_table
  .long .Llq_c1-.Llq_table
  .long .Llq_c2-.Llq_table
  .long .Llq_c3-.Llq_table
  .long .Llq_c0-.Llq_table
  .long .Llq_c1-.Llq_table
  .long .Llq_c2-.Llq_table
  .long .Llq_c3-.Llq_table
  .long .Llq_c0-.Llq_table
  .long .Llq_c1-.Llq_table
  .long .Llq_c2-.Llq_table
  .long .Llq_c3-.Llq_table
  .long .Llq_c0-.Llq_table
  .long .Llq_c1-.Llq_table
  .long .Llq_c2-.Llq_table
  .long .Llq_c3-.Llq_table
  .long .Llq_c0-.Llq_table
  .long .Llq_c1-.Llq_table
  .long .Llq_c2-.Llq_table
  .long .Llq_c3-.Llq_table
  .long .Llq_c0-.Llq_table
  .long .Llq_c1-.Llq_table
  .long .Llq_c2-.Llq_table
  .long .Llq_c3-.Llq_table
  .long .Llq_c0-.Llq_table
  .long .Llq_c1-.Llq_table
  .long .Llq_c2-.Llq_table
  .long .Llq_c3-.Llq_table
  .long .Llq_c0-.Llq_table
  .long .Llq_c1-.Llq_table
  .long .Llq_c2-.Llq_table
  .long .Llq_c3-.Llq_table
  .long .Llq_c0-.Llq_table
  .long .Llq_c1-.Llq_table
  .long .Llq_c2-.Llq_table
  .long .Llq_c3-.Llq_table
  .long .Llq_c0-.Llq_table
  .long .Llq_c1-.Llq_table
  .long .Llq_c2-.Llq_table
  .long .Llq_c3-.Llq_table
  .long .Llq_c0-.Llq_table
  .long .Llq_c1-.Llq_table
  .long .Llq_c2-.Llq_table
  .long .Llq_c3-.Llq_table
  .long .Llq_c0-.Llq_table
  .long .Llq_c1-.Llq_table
  .long .Llq_c2-.Llq_table
  .long .Llq_c3-.Llq_table
  .long .Llq_c0-.Llq_table
  .long .Llq_c1-.Llq_table
  .long .Llq_c2-.Llq_table
  .long .Llq_c3-.Llq_table
  .long .Llq_c0-.Llq_table
  .long .Llq_c1-.Llq_table
  .long .Llq_c2-.Llq_table
  .long .Llq_c3-.Llq_table
  .long .Llq_c0-.Llq_table
  .long .Llq_c1-.Llq_table
  .long .Llq_c2-.Llq_table
  .long .Llq_c3-.Llq_table
  .long .Llq_c0-.Llq_table
  .long .Llq_c1-.Llq_table
  .long .Llq_c2-.Llq_table
  .long .Llq_c3-.Llq_table
  .long .Llq_c0-.Llq_table
  .long .Llq_c1-.Llq_table
  .long .Llq_c2-.Llq_table
  .long .Llq_c3-.Llq_table
  .long .Llq_c0-.Llq_table
  .long .Llq_c1-.Llq_table
  .long .Llq_c2-.Llq_table
  .long .Llq_c3-.Llq_table
  .long .Llq_c0-.Llq_table
  .long .Llq_c1-.Llq_table
  .long .Llq_c2-.Llq_table
  .long .Llq_c3-.Llq_table
  .long .Llq_c0-.Llq_table
  .long .Llq_c1-.Llq_table
  .long .Llq_c2-.Llq_table
  .long .Llq_c3-.Llq_table
  .long .Llq_c0-.Llq_table
  .long .Llq_c1-.Llq_table
  .long .Llq_c2-.Llq_table
  .long .Llq_c3-.Llq_table
  .long .Llq_c0-.Llq_table
  .long .Llq_c1-.Llq_table
  .long .Llq_c2-.Llq_table
  .long .Llq_c3-.Llq_table
  .long .Llq_c0-.Llq_table
  .long .Llq_c1-.Llq_table
  .long .Llq_c2-.Llq_table
  .long .Llq_c3-.Llq_table
  .long .Llq_c0-.Llq_table
  .long .Llq_c1-.Llq_table
  .long .Llq_c2-.Llq_table
  .long .Llq_c3-.Llq_table
  .long .Llq_c0-.Llq_table
  .long .Llq_c1-.Llq_table
  .long .Llq_c2-.Llq_table
  .long .Llq_c3-.Llq_table
  .long .Llq_c0-.Llq_table
  .long .Llq_c1-.Llq_table
  .long .Llq_c2-.Llq_table
  .long .Llq_c3-.Llq_table
  .long .Llq_c0-.Llq_table
  .long .Llq_c1-.Llq_table
  .long .Llq_c2-.Llq_table
  .long .Llq_c3-.Llq_table
  .long .Llq_c0-.Llq_table
  .long .Llq_c1-.Llq_table
  .long .Llq_c2-.Llq_table
  .long .Llq_c3-.Llq_table
  .long .Llq_c0-.Llq_table
  .long .Llq_c1-.Llq_table
  .long .Llq_c2-.Llq_table
  .long .Llq_c3-.Llq_table
  .long .Llq_c0-.Llq_table
  .long .Llq_c1-.Llq_table
  .long .Llq_c2-.Llq_table
  .long .Llq_c3-.Llq_table
  .long .Llq_c0-.Llq_table
  .long .Llq_c1-.Llq_table
  .long 0
  .p2align 2
.Llc_table:
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long 0
  .p2align 2
.Lls_table:
  .long .Lls_c0-.Lls_table
  .long .Lls_c1-.Lls_table
  .long .Lls_c2-.Lls_table
  .long .Lls_c3-.Lls_table
  .long .Lls_c0-.Lls_table
  .long .Lls_c1-.Lls_table
  .long .Lls_c2-.Lls_table
  .long .Lls_c3-.Lls_table
  .long .Lls_c0-.Lls_table
  .long .Lls_c1-.Lls_table
  .long .Lls_c2-.Lls_table
  .long .Lls_c3-.Lls_table
  .long .Lls_c0-.Lls_table
  .long .Lls_c1-.Lls_table
  .long .Lls_c2-.Lls_table
  .long .Lls_c3-.Lls_table
  .long .Lls_c0-.Lls_table
  .long .Lls_c1-.Lls_table
  .long .Lls_c2-.Lls_table
  .long .Lls_c3-.Lls_table
  .long .Lls_c0-.Lls_table
  .long .Lls_c1-.Lls_table
  .long .Lls_c2-.Lls_table
  .long .Lls_c3-.Lls_table
  .long .Lls_c0-.Lls_table
  .long .Lls_c1-.Lls_table
  .long .Lls_c2-.Lls_table
  .long .Lls_c3-.Lls_table
  .long .Lls_c0-.Lls_table
  .long .Lls_c1-.Lls_table
  .long .Lls_c2-.Lls_table
  .long .Lls_c3-.Lls_table
  .long .Lls_c0-.Lls_table
  .long .Lls_c1-.Lls_table
  .long .Lls_c2-.Lls_table
  .long .Lls_c3-.Lls_table
  .long .Lls_c0-.Lls_table
  .long .Lls_c1-.Lls_table
  .long .Lls_c2-.Lls_table
  .long .Lls_c3-.Lls_table
  .long .Lls_c0-.Lls_table
  .long .Lls_c1-.Lls_table
  .long .Lls_c2-.Lls_table
  .long .Lls_c3-.Lls_table
  .long .Lls_c0-.Lls_table
  .long .Lls_c1-.Lls_table
  .long .Lls_c2-.Lls_table
  .long .Lls_c3-.Lls_table
  .long .Lls_c0-.Lls_table
  .long .Lls_c1-.Lls_table
  .long .Lls_c2-.Lls_table
  .long .Lls_c3-.Lls_table
  .long .Lls_c0-.Lls_table
  .long .Lls_c1-.Lls_table
  .long .Lls_c2-.Lls_table
  .long .Lls_c3-.Lls_table
  .long .Lls_c0-.Lls_table
  .long .Lls_c1-.Lls_table
  .long .Lls_c2-.Lls_table
  .long .Lls_c3-.Lls_table
  .long .Lls_c0-.Lls_table
  .long .Lls_c1-.Lls_table
  .long .Lls_c2-.Lls_table
  .long .Lls_c3-.Lls_table
  .long .Lls_c0-.Lls_table
  .long .Lls_c1-.Lls_table
  .long .Lls_c2-.Lls_table
  .long .Lls_c3-.Lls_table
  .long .Lls_c0-.Lls_table
  .long .Lls_c1-.Lls_table
  .long .Lls_c2-.Lls_table
  .long .Lls_c3-.Lls_table
  .long .Lls_c0-.Lls_table
  .long .Lls_c1-.Lls_table
  .long .Lls_c2-.Lls_table
  .long .Lls_c3-.Lls_table
  .long .Lls_c0-.Lls_table
  .long .Lls_c1-.Lls_table
  .long .Lls_c2-.Lls_table
  .long .Lls_c3-.Lls_table
  .long .Lls_c0-.Lls_table
  .long .Lls_c1-.Lls_table
  .long .Lls_c2-.Lls_table
  .long .Lls_c3-.Lls_table
  .long .Lls_c0-.Lls_table
  .long .Lls_c1-.Lls_table
  .long .Lls_c2-.Lls_table
  .long .Lls_c3-.Lls_table
  .long .Lls_c0-.Lls_table
  .long .Lls_c1-.Lls_table
  .long .Lls_c2-.Lls_table
  .long .Lls_c3-.Lls_table
  .long .Lls_c0-.Lls_table
  .long .Lls_c1-.Lls_table
  .long .Lls_c2-.Lls_table
  .long .Lls_c3-.Lls_table
  .long .Lls_c0-.Lls_table
  .long .Lls_c1-.Lls_table
  .long .Lls_c2-.Lls_table
  .long .Lls_c3-.Lls_table
  .long .Lls_c0-.Lls_table
  .long .Lls_c1-.Lls_table
  .long .Lls_c2-.Lls_table
  .long .Lls_c3-.Lls_table
  .long .Lls_c0-.Lls_table
  .long .Lls_c1-.Lls_table
  .long .Lls_c2-.Lls_table
  .long .Lls_c3-.Lls_table
  .long .Lls_c0-.Lls_table
  .long .Lls_c1-.Lls_table
  .long .Lls_c2-.Lls_table
  .long .Lls_c3-.Lls_table
  .long .Lls_c0-.Lls_table
  .long .Lls_c1-.Lls_table
  .long .Lls_c2-.Lls_table
  .long .Lls_c3-.Lls_table
  .long .Lls_c0-.Lls_table
  .long .Lls_c1-.Lls_table
  .long .Lls_c2-.Lls_table
  .long .Lls_c3-.Lls_table
  .long .Lls_c0-.Lls_table
  .long .Lls_c1-.Lls_table
  .long .Lls_c2-.Lls_table
  .long .Lls_c3-.Lls_table
  .long .Lls_c0-.Lls_table
  .long .Lls_c1-.Lls_table
  .long .Lls_c2-.Lls_table
  .long .Lls_c3-.Lls_table
  .long .Lls_c0-.Lls_table
  .long .Lls_c1-.Lls_table
  .long 0
  .p2align 2
.Lll_table:
  .long .Lll_c0-.Lll_table
  .long .Lll_c1-.Lll_table
  .long .Lll_c2-.Lll_table
  .long .Lll_c3-.Lll_table
  .long .Lll_c0-.Lll_table
  .long .Lll_c1-.Lll_table
  .long .Lll_c2-.Lll_table
  .long .Lll_c3-.Lll_table
  .long .Lll_c0-.Lll_table
  .long .Lll_c1-.Lll_table
  .long .Lll_c2-.Lll_table
  .long .Lll_c3-.Lll_table
  .long .Lll_c0-.Lll_table
  .long .Lll_c1-.Lll_table
  .long .Lll_c2-.Lll_table
  .long .Lll_c3-.Lll_table
  .long .Lll_c0-.Lll_table
  .long .Lll_c1-.Lll_table
  .long .Lll_c2-.Lll_table
  .long .Lll_c3-.Lll_table
  .long .Lll_c0-.Lll_table
  .long .Lll_c1-.Lll_table
  .long .Lll_c2-.Lll_table
  .long .Lll_c3-.Lll_table
  .long .Lll_c0-.Lll_table
  .long .Lll_c1-.Lll_table
  .long .Lll_c2-.Lll_table
  .long .Lll_c3-.Lll_table
  .long .Lll_c0-.Lll_table
  .long .Lll_c1-.Lll_table
  .long .Lll_c2-.Lll_table
  .long .Lll_c3-.Lll_table
  .long .Lll_c0-.Lll_table
  .long .Lll_c1-.Lll_table
  .long .Lll_c2-.Lll_table
  .long .Lll_c3-.Lll_table
  .long .Lll_c0-.Lll_table
  .long .Lll_c1-.Lll_table
  .long .Lll_c2-.Lll_table
  .long .Lll_c3-.Lll_table
  .long .Lll_c0-.Lll_table
  .long .Lll_c1-.Lll_table
  .long .Lll_c2-.Lll_table
  .long .Lll_c3-.Lll_table
  .long .Lll_c0-.Lll_table
  .long .Lll_c1-.Lll_table
  .long .Lll_c2-.Lll_table
  .long .Lll_c3-.Lll_table
  .long .Lll_c0-.Lll_table
  .long .Lll_c1-.Lll_table
  .long .Lll_c2-.Lll_table
  .long .Lll_c3-.Lll_table
  .long .Lll_c0-.Lll_table
  .long .Lll_c1-.Lll_table
  .long .Lll_c2-.Lll_table
  .long .Lll_c3-.Lll_table
  .long .Lll_c0-.Lll_table
  .long .Lll_c1-.Lll_table
  .long .Lll_c2-.Lll_table
  .long .Lll_c3-.Lll_table
  .long .Lll_c0-.Lll_table
  .long .Lll_c1-.Lll_table
  .long .Lll_c2-.Lll_table
  .long .Lll_c3-.Lll_table
  .long .Lll_c0-.Lll_table
  .long .Lll_c1-.Lll_table
  .long .Lll_c2-.Lll_table
  .long .Lll_c3-.Lll_table
  .long .Lll_c0-.Lll_table
  .long .Lll_c1-.Lll_table
  .long .Lll_c2-.Lll_table
  .long .Lll_c3-.Lll_table
  .long .Lll_c0-.Lll_table
  .long .Lll_c1-.Lll_table
  .long .Lll_c2-.Lll_table
  .long .Lll_c3-.Lll_table
  .long .Lll_c0-.Lll_table
  .long .Lll_c1-.Lll_table
  .long .Lll_c2-.Lll_table
  .long .Lll_c3-.Lll_table
  .long .Lll_c0-.Lll_table
  .long .Lll_c1-.Lll_table
  .long .Lll_c2-.Lll_table
  .long .Lll_c3-.Lll_table
  .long .Lll_c0-.Lll_table
  .long .Lll_c1-.Lll_table
  .long .Lll_c2-.Lll_table
  .long .Lll_c3-.Lll_table
  .long .Lll_c0-.Lll_table
  .long .Lll_c1-.Lll_table
  .long .Lll_c2-.Lll_table
  .long .Lll_c3-.Lll_table
  .long .Lll_c0-.Lll_table
  .long .Lll_c1-.Lll_table
  .long .Lll_c2-.Lll_table
  .long .Lll_c3-.Lll_table
  .long .Lll_c0-.Lll_table
  .long .Lll_c1-.Lll_table
  .long .Lll_c2-.Lll_table
  .long .Lll_c3-.Lll_table
  .long .Lll_c0-.Lll_table
  .long .Lll_c1-.Lll_table
  .long .Lll_c2-.Lll_table
  .long .Lll_c3-.Lll_table
  .long .Lll_c0-.Lll_table
  .long .Lll_c1-.Lll_table
  .long .Lll_c2-.Lll_table
  .long .Lll_c3-.Lll_table
  .long .Lll_c0-.Lll_table
  .long .Lll_c1-.Lll_table
  .long .Lll_c2-.Lll_table
  .long .Lll_c3-.Lll_table
  .long .Lll_c0-.Lll_table
  .long .Lll_c1-.Lll_table
  .long .Lll_c2-.Lll_table
  .long .Lll_c3-.Lll_table
  .long .Lll_c0-.Lll_table
  .long .Lll_c1-.Lll_table
  .long .Lll_c2-.Lll_table
  .long .Lll_c3-.Lll_table
  .long .Lll_c0-.Lll_table
  .long .Lll_c1-.Lll_table
  .long .Lll_c2-.Lll_table
  .long .Lll_c3-.Lll_table
  .long .Lll_c0-.Lll_table
  .long .Lll_c1-.Lll_table
  .long .Lll_c2-.Lll_table
  .long .Lll_c3-.Lll_table
  .long .Lll_c0-.Lll_table
  .long .Lll_c1-.Lll_table
  .long 0

  .section .note.GNU-stack,"",@progbits
