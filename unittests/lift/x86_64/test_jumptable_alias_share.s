# A byte-code walk, the shape of pcre2's find_minlength: each case keeps a
# running maximum with `cmova`, whose predicate has the shape of the
# dispatch guard `ja`, and loops back to the dispatch.  Every such select
# before the guard proposes a predicated index alias, which the guard proof
# asks the value resolver about.  Those queries only add index
# alternatives; the guard bounds the index without them.  The table ends
# with a zero word, which is no entry.

  .text
  .globl jt_alias_share
  .type jt_alias_share, @function
  .p2align 4
jt_alias_share:
  pushq %rbx
  movq %rdi, %rbx
  xorl %eax, %eax
  jmp .Las_top
.Las_c0:
  movzbl 1(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $1, %rbx
  jmp .Las_top
.Las_c1:
  movzbl 2(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $2, %rbx
  jmp .Las_top
.Las_c2:
  movzbl 3(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $3, %rbx
  jmp .Las_top
.Las_c3:
  movzbl 4(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $4, %rbx
  jmp .Las_top
.Las_c4:
  movzbl 5(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $5, %rbx
  jmp .Las_top
.Las_c5:
  movzbl 6(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $1, %rbx
  jmp .Las_top
.Las_c6:
  movzbl 7(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $2, %rbx
  jmp .Las_top
.Las_c7:
  movzbl 1(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $3, %rbx
  jmp .Las_top
.Las_c8:
  movzbl 2(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $4, %rbx
  jmp .Las_top
.Las_c9:
  movzbl 3(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $5, %rbx
  jmp .Las_top
.Las_c10:
  movzbl 4(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $1, %rbx
  jmp .Las_top
.Las_c11:
  movzbl 5(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $2, %rbx
  jmp .Las_top
.Las_c12:
  movzbl 6(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $3, %rbx
  jmp .Las_top
.Las_c13:
  movzbl 7(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $4, %rbx
  jmp .Las_top
.Las_c14:
  movzbl 1(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $5, %rbx
  jmp .Las_top
.Las_c15:
  movzbl 2(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $1, %rbx
  jmp .Las_top
.Las_c16:
  movzbl 3(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $2, %rbx
  jmp .Las_top
.Las_c17:
  movzbl 4(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $3, %rbx
  jmp .Las_top
.Las_c18:
  movzbl 5(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $4, %rbx
  jmp .Las_top
.Las_c19:
  movzbl 6(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $5, %rbx
  jmp .Las_top
.Las_c20:
  movzbl 7(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $1, %rbx
  jmp .Las_top
.Las_c21:
  movzbl 1(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $2, %rbx
  jmp .Las_top
.Las_c22:
  movzbl 2(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $3, %rbx
  jmp .Las_top
.Las_c23:
  movzbl 3(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $4, %rbx
  jmp .Las_top
.Las_c24:
  movzbl 4(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $5, %rbx
  jmp .Las_top
.Las_c25:
  movzbl 5(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $1, %rbx
  jmp .Las_top
.Las_c26:
  movzbl 6(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $2, %rbx
  jmp .Las_top
.Las_c27:
  movzbl 7(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $3, %rbx
  jmp .Las_top
.Las_c28:
  movzbl 1(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $4, %rbx
  jmp .Las_top
.Las_c29:
  movzbl 2(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $5, %rbx
  jmp .Las_top
.Las_c30:
  movzbl 3(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $1, %rbx
  jmp .Las_top
.Las_c31:
  movzbl 4(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $2, %rbx
  jmp .Las_top
.Las_c32:
  movzbl 5(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $3, %rbx
  jmp .Las_top
.Las_c33:
  movzbl 6(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $4, %rbx
  jmp .Las_top
.Las_c34:
  movzbl 7(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $5, %rbx
  jmp .Las_top
.Las_c35:
  movzbl 1(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $1, %rbx
  jmp .Las_top
.Las_c36:
  movzbl 2(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $2, %rbx
  jmp .Las_top
.Las_c37:
  movzbl 3(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $3, %rbx
  jmp .Las_top
.Las_c38:
  movzbl 4(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $4, %rbx
  jmp .Las_top
.Las_c39:
  movzbl 5(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $5, %rbx
  jmp .Las_top
.Las_c40:
  movzbl 6(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $1, %rbx
  jmp .Las_top
.Las_c41:
  movzbl 7(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $2, %rbx
  jmp .Las_top
.Las_c42:
  movzbl 1(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $3, %rbx
  jmp .Las_top
.Las_c43:
  movzbl 2(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $4, %rbx
  jmp .Las_top
.Las_c44:
  movzbl 3(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $5, %rbx
  jmp .Las_top
.Las_c45:
  movzbl 4(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $1, %rbx
  jmp .Las_top
.Las_c46:
  movzbl 5(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $2, %rbx
  jmp .Las_top
.Las_c47:
  movzbl 6(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $3, %rbx
  jmp .Las_top
.Las_c48:
  movzbl 7(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $4, %rbx
  jmp .Las_top
.Las_c49:
  movzbl 1(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $5, %rbx
  jmp .Las_top
.Las_c50:
  movzbl 2(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $1, %rbx
  jmp .Las_top
.Las_c51:
  movzbl 3(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $2, %rbx
  jmp .Las_top
.Las_c52:
  movzbl 4(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $3, %rbx
  jmp .Las_top
.Las_c53:
  movzbl 5(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $4, %rbx
  jmp .Las_top
.Las_c54:
  movzbl 6(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $5, %rbx
  jmp .Las_top
.Las_c55:
  movzbl 7(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $1, %rbx
  jmp .Las_top
.Las_c56:
  movzbl 1(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $2, %rbx
  jmp .Las_top
.Las_c57:
  movzbl 2(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $3, %rbx
  jmp .Las_top
.Las_c58:
  movzbl 3(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $4, %rbx
  jmp .Las_top
.Las_c59:
  movzbl 4(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $5, %rbx
  jmp .Las_top
.Las_c60:
  movzbl 5(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $1, %rbx
  jmp .Las_top
.Las_c61:
  movzbl 6(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $2, %rbx
  jmp .Las_top
.Las_c62:
  movzbl 7(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $3, %rbx
  jmp .Las_top
.Las_c63:
  movzbl 1(%rbx), %edx
  cmpl %edx, %eax
  cmova %edx, %eax
  addq $4, %rbx
  jmp .Las_top
.Las_top:
  movzbl (%rbx), %ecx
  cmpl $63, %ecx
  ja .Las_done
  leaq .Las_table(%rip), %rdx
  movslq (%rdx,%rcx,4), %rcx
  addq %rdx, %rcx
  jmpq *%rcx
.Las_done:
  popq %rbx
  ret
  .size jt_alias_share, .-jt_alias_share

  .section .rodata
  .p2align 2
.Las_table:
  .long .Las_c0-.Las_table
  .long .Las_c1-.Las_table
  .long .Las_c2-.Las_table
  .long .Las_c3-.Las_table
  .long .Las_c4-.Las_table
  .long .Las_c5-.Las_table
  .long .Las_c6-.Las_table
  .long .Las_c7-.Las_table
  .long .Las_c8-.Las_table
  .long .Las_c9-.Las_table
  .long .Las_c10-.Las_table
  .long .Las_c11-.Las_table
  .long .Las_c12-.Las_table
  .long .Las_c13-.Las_table
  .long .Las_c14-.Las_table
  .long .Las_c15-.Las_table
  .long .Las_c16-.Las_table
  .long .Las_c17-.Las_table
  .long .Las_c18-.Las_table
  .long .Las_c19-.Las_table
  .long .Las_c20-.Las_table
  .long .Las_c21-.Las_table
  .long .Las_c22-.Las_table
  .long .Las_c23-.Las_table
  .long .Las_c24-.Las_table
  .long .Las_c25-.Las_table
  .long .Las_c26-.Las_table
  .long .Las_c27-.Las_table
  .long .Las_c28-.Las_table
  .long .Las_c29-.Las_table
  .long .Las_c30-.Las_table
  .long .Las_c31-.Las_table
  .long .Las_c32-.Las_table
  .long .Las_c33-.Las_table
  .long .Las_c34-.Las_table
  .long .Las_c35-.Las_table
  .long .Las_c36-.Las_table
  .long .Las_c37-.Las_table
  .long .Las_c38-.Las_table
  .long .Las_c39-.Las_table
  .long .Las_c40-.Las_table
  .long .Las_c41-.Las_table
  .long .Las_c42-.Las_table
  .long .Las_c43-.Las_table
  .long .Las_c44-.Las_table
  .long .Las_c45-.Las_table
  .long .Las_c46-.Las_table
  .long .Las_c47-.Las_table
  .long .Las_c48-.Las_table
  .long .Las_c49-.Las_table
  .long .Las_c50-.Las_table
  .long .Las_c51-.Las_table
  .long .Las_c52-.Las_table
  .long .Las_c53-.Las_table
  .long .Las_c54-.Las_table
  .long .Las_c55-.Las_table
  .long .Las_c56-.Las_table
  .long .Las_c57-.Las_table
  .long .Las_c58-.Las_table
  .long .Las_c59-.Las_table
  .long .Las_c60-.Las_table
  .long .Las_c61-.Las_table
  .long .Las_c62-.Las_table
  .long .Las_c63-.Las_table
  .long 0

  .section .note.GNU-stack,"",@progbits
