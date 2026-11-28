# Jumps through relative tables of their own code, without a bound, in a
# Win64 object, whose calling convention also keeps %rsi and %rdi.
#
# jt_win64_kept names two labels; one reads %rsi before any write, which is
# the caller's value under Win64, so following that edge would make %rsi an
# input and only the other label is kept.  jt_win64_call_keeps loads the
# table's address into %rsi before a call and reads the table after it, as
# MSVC does with a switch after a call: Win64 leaves %rsi as it was, so the
# table read survives the call.  Its frame stays set up across the jump and
# each label returns through the epilogue.  Every table ends with a zero
# word, which names the table itself.  A COFF object's symbols carry no
# sizes, so each function ends where the next begins and jt_win64_ext comes
# last.

  .text
  .def jt_win64_kept;
  .scl 2;
  .type 32;
  .endef
  .globl jt_win64_kept
  .p2align 4
jt_win64_kept:
  movq (%rcx), %rax
  leaq .Lwk_table(%rip), %rdx
  movslq (%rdx,%rax,4), %r8
  addq %rdx, %r8
  jmpq *%r8
.Lwk_plain:
  movl $1, %eax
  retq
.Lwk_reads_kept:
  movq %rsi, %rax
  retq

  .def jt_win64_call_keeps;
  .scl 2;
  .type 32;
  .endef
  .globl jt_win64_call_keeps
  .p2align 4
jt_win64_call_keeps:
  pushq %rsi
  subq $32, %rsp
  leaq .Lwc_table(%rip), %rsi
  callq jt_win64_ext
  movslq (%rsi,%rax,4), %rdx
  addq %rsi, %rdx
  jmpq *%rdx
.Lwc_first:
  movl $10, %eax
  addq $32, %rsp
  popq %rsi
  retq
.Lwc_second:
  movl $11, %eax
  addq $32, %rsp
  popq %rsi
  retq

  .def jt_win64_ext;
  .scl 2;
  .type 32;
  .endef
  .globl jt_win64_ext
  .p2align 4
jt_win64_ext:
  movl $1, %eax
  retq

  .section .rdata,"dr"
  .p2align 2
.Lwk_table:
  .long .Lwk_plain-.Lwk_table
  .long .Lwk_reads_kept-.Lwk_table
  .long .Lwk_plain-.Lwk_table
  .long 0
.Lwc_table:
  .long .Lwc_first-.Lwc_table
  .long .Lwc_second-.Lwc_table
  .long 0
