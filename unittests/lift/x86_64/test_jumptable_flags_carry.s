# A guard that reuses the flags of the previous block's compare (GCC -O2
# jump threading, as in libzstd's sequence decoder): `cmp $7, %edx; jle L`
# leads to `L: movzbl ...; ja default`, and the `ja` reads the compare's flags.
# jt_flags_carry dispatches its eight cases right after that `ja`;
# jt_flags_carry_chain reaches the `ja` through one more block that keeps the
# flags.  jt_flags_stale_index changes the index between the compare and the
# dispatch, and jt_flags_two_paths enters the `ja` block from a second compare
# that admits 101 values: neither dispatch may be resolved.  Every table ends
# with a zero word, which is no entry.

  .text
  .globl jt_flags_carry
  .type jt_flags_carry, @function
  .p2align 4
jt_flags_carry:
  testl %edx, %edx
  je .Lfc_out
  cmpl $7, %edx
  jle .Lfc_switch
.Lfc_out:
  xorl %eax, %eax
  ret
.Lfc_switch:
  movzbl 3(%rsi), %r8d
  ja .Lfc_default
  leaq .Lfc_table(%rip), %rcx
  movl %edx, %r11d
  movslq (%rcx,%r11,4), %r11
  addq %rcx, %r11
  jmpq *%r11
.Lfc_c0:
  movl $10, %eax
  ret
.Lfc_c1:
  movl $11, %eax
  ret
.Lfc_c2:
  movl $12, %eax
  ret
.Lfc_c3:
  movl $13, %eax
  ret
.Lfc_c4:
  movl $14, %eax
  ret
.Lfc_c5:
  movl $15, %eax
  ret
.Lfc_c6:
  movl $16, %eax
  ret
.Lfc_c7:
  movl %r8d, %eax
  ret
.Lfc_default:
  movl $-1, %eax
  ret
  .size jt_flags_carry, .-jt_flags_carry

  .globl jt_flags_carry_chain
  .type jt_flags_carry_chain, @function
  .p2align 4
jt_flags_carry_chain:
  testl %edx, %edx
  je .Lfh_out
  cmpl $7, %edx
  jle .Lfh_relay
.Lfh_out:
  xorl %eax, %eax
  ret
.Lfh_relay:
  movzbl 3(%rsi), %r8d
  jmp .Lfh_switch
.Lfh_c0:
  movl $20, %eax
  ret
.Lfh_c1:
  movl $21, %eax
  ret
.Lfh_c2:
  movl $22, %eax
  ret
.Lfh_c3:
  movl $23, %eax
  ret
.Lfh_c4:
  movl $24, %eax
  ret
.Lfh_c5:
  movl $25, %eax
  ret
.Lfh_c6:
  movl $26, %eax
  ret
.Lfh_c7:
  movl %r8d, %eax
  ret
.Lfh_switch:
  movq %r8, %r9
  ja .Lfh_default
  leaq .Lfh_table(%rip), %rcx
  movl %edx, %r11d
  movslq (%rcx,%r11,4), %r11
  addq %rcx, %r11
  jmpq *%r11
.Lfh_default:
  movl $-1, %eax
  ret
  .size jt_flags_carry_chain, .-jt_flags_carry_chain

  .globl jt_flags_stale_index
  .type jt_flags_stale_index, @function
  .p2align 4
jt_flags_stale_index:
  testl %edx, %edx
  je .Lfs_out
  cmpl $7, %edx
  leal 1(%rdx), %edx
  jle .Lfs_switch
.Lfs_out:
  xorl %eax, %eax
  ret
.Lfs_switch:
  movzbl 3(%rsi), %r8d
  ja .Lfs_default
  leaq .Lfs_table(%rip), %rcx
  movl %edx, %r11d
  movslq (%rcx,%r11,4), %r11
  addq %rcx, %r11
  jmpq *%r11
.Lfs_c0:
  movl $30, %eax
  ret
.Lfs_c1:
  movl $31, %eax
  ret
.Lfs_c2:
  movl $32, %eax
  ret
.Lfs_c3:
  movl $33, %eax
  ret
.Lfs_c4:
  movl $34, %eax
  ret
.Lfs_c5:
  movl $35, %eax
  ret
.Lfs_c6:
  movl $36, %eax
  ret
.Lfs_c7:
  movl %r8d, %eax
  ret
.Lfs_default:
  movl $-1, %eax
  ret
  .size jt_flags_stale_index, .-jt_flags_stale_index

  .globl jt_flags_two_paths
  .type jt_flags_two_paths, @function
  .p2align 4
jt_flags_two_paths:
  cmpl $1, %edi
  je .Lft_wide
  cmpl $7, %edx
  jle .Lft_switch
  xorl %eax, %eax
  ret
.Lft_wide:
  cmpl $100, %edx
  jbe .Lft_switch
  xorl %eax, %eax
  ret
.Lft_switch:
  movzbl 3(%rsi), %r8d
  ja .Lft_default
  leaq .Lft_table(%rip), %rcx
  movl %edx, %r11d
  movslq (%rcx,%r11,4), %r11
  addq %rcx, %r11
  jmpq *%r11
.Lft_c0:
  movl $40, %eax
  ret
.Lft_c1:
  movl $41, %eax
  ret
.Lft_c2:
  movl $42, %eax
  ret
.Lft_c3:
  movl $43, %eax
  ret
.Lft_c4:
  movl $44, %eax
  ret
.Lft_c5:
  movl $45, %eax
  ret
.Lft_c6:
  movl $46, %eax
  ret
.Lft_c7:
  movl %r8d, %eax
  ret
.Lft_default:
  movl $-1, %eax
  ret
  .size jt_flags_two_paths, .-jt_flags_two_paths

  .section .rodata
  .p2align 2
.Lfc_table:
  .long .Lfc_c0-.Lfc_table
  .long .Lfc_c1-.Lfc_table
  .long .Lfc_c2-.Lfc_table
  .long .Lfc_c3-.Lfc_table
  .long .Lfc_c4-.Lfc_table
  .long .Lfc_c5-.Lfc_table
  .long .Lfc_c6-.Lfc_table
  .long .Lfc_c7-.Lfc_table
  .long 0
.Lfh_table:
  .long .Lfh_c0-.Lfh_table
  .long .Lfh_c1-.Lfh_table
  .long .Lfh_c2-.Lfh_table
  .long .Lfh_c3-.Lfh_table
  .long .Lfh_c4-.Lfh_table
  .long .Lfh_c5-.Lfh_table
  .long .Lfh_c6-.Lfh_table
  .long .Lfh_c7-.Lfh_table
  .long 0
.Lfs_table:
  .long .Lfs_c0-.Lfs_table
  .long .Lfs_c1-.Lfs_table
  .long .Lfs_c2-.Lfs_table
  .long .Lfs_c3-.Lfs_table
  .long .Lfs_c4-.Lfs_table
  .long .Lfs_c5-.Lfs_table
  .long .Lfs_c6-.Lfs_table
  .long .Lfs_c7-.Lfs_table
  .long 0
.Lft_table:
  .long .Lft_c0-.Lft_table
  .long .Lft_c1-.Lft_table
  .long .Lft_c2-.Lft_table
  .long .Lft_c3-.Lft_table
  .long .Lft_c4-.Lft_table
  .long .Lft_c5-.Lft_table
  .long .Lft_c6-.Lft_table
  .long .Lft_c7-.Lft_table
  .long 0

  .section .note.GNU-stack,"",@progbits
