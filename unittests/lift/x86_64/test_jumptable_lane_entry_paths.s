# A byte-guarded dispatch entered on two paths (GCC -O2 shape, as in pcre2's
# set_start_bits): each path loads a byte with `movzbl`, compares that lane
# with `cmpb $0xac`, and reaches the dispatch through `ja` or `jbe`, so no
# one compare dominates the table load and all 173 slots form the joined
# domain.  The other dispatches must stay unresolved: jt_lane_paths_sext
# loads the second path's byte with `movsbq`, so lane values 0x80 to 0xac
# index before the table, jt_lane_paths_open enters on the second path
# without a compare, and jt_lane_paths_signed reaches the dispatch on the
# second path through `jg`, which admits every byte from 0xad to 0xff.  A
# plain word separates the tables' relocation runs.

  .text
  .globl jt_lane_paths
  .type jt_lane_paths, @function
  .p2align 4
jt_lane_paths:
  leaq .Llp_table(%rip), %rcx
  movzbl (%rdi), %eax
  cmpb $0xac, %al
  ja .Llp_default
.Llp_dispatch:
  movslq (%rcx,%rax,4), %rax
  addq %rcx, %rax
  jmpq *%rax
.Llp_c0:
  movl $10, %eax
  retq
.Llp_c1:
  movzbl 1(%rdi), %eax
  addq $1, %rdi
  cmpb $0xac, %al
  jbe .Llp_dispatch
  jmp .Llp_default
.Llp_c2:
  movl $12, %eax
  retq
.Llp_c3:
  movl $13, %eax
  retq
.Llp_default:
  xorl %eax, %eax
  retq
  .size jt_lane_paths, .-jt_lane_paths

  .text
  .globl jt_lane_paths_sext
  .type jt_lane_paths_sext, @function
  .p2align 4
jt_lane_paths_sext:
  leaq .Lls_table(%rip), %rcx
  movzbl (%rdi), %eax
  cmpb $0xac, %al
  ja .Lls_default
.Lls_dispatch:
  movslq (%rcx,%rax,4), %rax
  addq %rcx, %rax
  jmpq *%rax
.Lls_c0:
  movl $10, %eax
  retq
.Lls_c1:
  movsbq 1(%rdi), %rax
  addq $1, %rdi
  cmpb $0xac, %al
  jbe .Lls_dispatch
  jmp .Lls_default
.Lls_c2:
  movl $12, %eax
  retq
.Lls_c3:
  movl $13, %eax
  retq
.Lls_default:
  xorl %eax, %eax
  retq
  .size jt_lane_paths_sext, .-jt_lane_paths_sext

  .text
  .globl jt_lane_paths_open
  .type jt_lane_paths_open, @function
  .p2align 4
jt_lane_paths_open:
  leaq .Llo_table(%rip), %rcx
  movzbl (%rdi), %eax
  cmpb $0xac, %al
  ja .Llo_default
.Llo_dispatch:
  movslq (%rcx,%rax,4), %rax
  addq %rcx, %rax
  jmpq *%rax
.Llo_c0:
  movl $10, %eax
  retq
.Llo_c1:
  movzbl 1(%rdi), %eax
  addq $1, %rdi
  jmp .Llo_dispatch
.Llo_c2:
  movl $12, %eax
  retq
.Llo_c3:
  movl $13, %eax
  retq
.Llo_default:
  xorl %eax, %eax
  retq
  .size jt_lane_paths_open, .-jt_lane_paths_open

  .text
  .globl jt_lane_paths_signed
  .type jt_lane_paths_signed, @function
  .p2align 4
jt_lane_paths_signed:
  leaq .Llg_table(%rip), %rcx
  movzbl (%rdi), %eax
  cmpb $0xac, %al
  ja .Llg_default
.Llg_dispatch:
  movslq (%rcx,%rax,4), %rax
  addq %rcx, %rax
  jmpq *%rax
.Llg_c0:
  movl $10, %eax
  retq
.Llg_c1:
  movzbl 1(%rdi), %eax
  addq $1, %rdi
  cmpb $0xac, %al
  jg .Llg_dispatch
  jmp .Llg_default
.Llg_c2:
  movl $12, %eax
  retq
.Llg_c3:
  movl $13, %eax
  retq
.Llg_default:
  xorl %eax, %eax
  retq
  .size jt_lane_paths_signed, .-jt_lane_paths_signed

  .section .rodata,"a",@progbits
  .p2align 2
.Llp_table:
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
  .long .Llp_c1-.Llp_table
  .long .Llp_c2-.Llp_table
  .long .Llp_c3-.Llp_table
  .long .Llp_c0-.Llp_table
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
  .long 0
  .p2align 2
.Llo_table:
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long .Llo_c1-.Llo_table
  .long .Llo_c2-.Llo_table
  .long .Llo_c3-.Llo_table
  .long .Llo_c0-.Llo_table
  .long 0
  .p2align 2
.Llg_table:
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long .Llg_c1-.Llg_table
  .long .Llg_c2-.Llg_table
  .long .Llg_c3-.Llg_table
  .long .Llg_c0-.Llg_table
  .long 0

  .section .note.GNU-stack,"",@progbits
