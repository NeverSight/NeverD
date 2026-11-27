# A guard on a narrower low lane of the index register that its own block
# never writes (GCC -O2 in sqlite's sqlite3VdbeRecordCompareWithSkip):
# `movzbl (%rdi), %eax; cmp $9, %eax; ja` bounds the loaded byte by ten, and
# a later block's `cmp $6, %al; ja` bounds it by seven before the dispatch
# indexes the table with %rax.  The lane's definition, the zero extension
# that ties %al to %rax, lies in an earlier block.  jt_lane_carry compares
# the lane in the block right after the dword guard, and jt_lane_carry_chain
# three blocks after the load, past a zero test and a compare with 7: both
# dispatches read their seven entries.  jt_lane_two_paths enters the lane
# compare from a second path that loads all of %rax, jt_lane_wide_load loads
# all of %rax before both compares, and jt_lane_high_byte writes %ah between
# them, so a bound on %al leaves the index's upper bits free: none of those
# dispatches may be resolved.  Every table ends with a zero word, which is no
# entry.

  .text
  .globl jt_lane_carry
  .type jt_lane_carry, @function
  .p2align 4
jt_lane_carry:
  movzbl (%rdi), %eax
  cmpl $9, %eax
  ja .Llc_other
  cmpb $6, %al
  ja .Llc_other
  leaq .Llc_table(%rip), %rcx
  movslq (%rcx,%rax,4), %rdx
  addq %rcx, %rdx
  notrack jmpq *%rdx
.Llc_c0:
  movl $10, %eax
  ret
.Llc_c1:
  movl $11, %eax
  ret
.Llc_c2:
  movl $12, %eax
  ret
.Llc_c3:
  movl $13, %eax
  ret
.Llc_c4:
  movl $14, %eax
  ret
.Llc_c5:
  movl $15, %eax
  ret
.Llc_c6:
  movl $16, %eax
  ret
.Llc_other:
  movl $-1, %eax
  ret
  .size jt_lane_carry, .-jt_lane_carry

  .globl jt_lane_carry_chain
  .type jt_lane_carry_chain, @function
  .p2align 4
jt_lane_carry_chain:
  movzbl (%rdi), %eax
  cmpl $9, %eax
  ja .Llh_other
  testl %eax, %eax
  je .Llh_zero
  cmpl $7, %eax
  je .Llh_seven
  cmpb $6, %al
  ja .Llh_other
  leaq .Llh_table(%rip), %rcx
  movslq (%rcx,%rax,4), %rdx
  addq %rcx, %rdx
  notrack jmpq *%rdx
.Llh_c1:
  movl $21, %eax
  ret
.Llh_c2:
  movl $22, %eax
  ret
.Llh_c3:
  movl $23, %eax
  ret
.Llh_c4:
  movl $24, %eax
  ret
.Llh_c5:
  movl $25, %eax
  ret
.Llh_c6:
  movl $26, %eax
  ret
.Llh_zero:
  xorl %eax, %eax
  ret
.Llh_seven:
  movl $7, %eax
  ret
.Llh_other:
  movl $-1, %eax
  ret
  .size jt_lane_carry_chain, .-jt_lane_carry_chain

  .globl jt_lane_two_paths
  .type jt_lane_two_paths, @function
  .p2align 4
jt_lane_two_paths:
  testl %esi, %esi
  jne .Llt_wide
  movzbl (%rdi), %eax
  cmpl $9, %eax
  ja .Llt_other
  jmp .Llt_lane
.Llt_wide:
  movq (%rdi), %rax
.Llt_lane:
  cmpb $6, %al
  ja .Llt_other
  leaq .Llt_table(%rip), %rcx
  movslq (%rcx,%rax,4), %rdx
  addq %rcx, %rdx
  notrack jmpq *%rdx
.Llt_c0:
  movl $30, %eax
  ret
.Llt_c1:
  movl $31, %eax
  ret
.Llt_c2:
  movl $32, %eax
  ret
.Llt_c3:
  movl $33, %eax
  ret
.Llt_c4:
  movl $34, %eax
  ret
.Llt_c5:
  movl $35, %eax
  ret
.Llt_c6:
  movl $36, %eax
  ret
.Llt_other:
  movl $-1, %eax
  ret
  .size jt_lane_two_paths, .-jt_lane_two_paths

  .globl jt_lane_wide_load
  .type jt_lane_wide_load, @function
  .p2align 4
jt_lane_wide_load:
  movq (%rdi), %rax
  cmpl $9, %eax
  ja .Llw_other
  cmpb $6, %al
  ja .Llw_other
  leaq .Llw_table(%rip), %rcx
  movslq (%rcx,%rax,4), %rdx
  addq %rcx, %rdx
  notrack jmpq *%rdx
.Llw_c0:
  movl $40, %eax
  ret
.Llw_c1:
  movl $41, %eax
  ret
.Llw_c2:
  movl $42, %eax
  ret
.Llw_c3:
  movl $43, %eax
  ret
.Llw_c4:
  movl $44, %eax
  ret
.Llw_c5:
  movl $45, %eax
  ret
.Llw_c6:
  movl $46, %eax
  ret
.Llw_other:
  movl $-1, %eax
  ret
  .size jt_lane_wide_load, .-jt_lane_wide_load

  .globl jt_lane_high_byte
  .type jt_lane_high_byte, @function
  .p2align 4
jt_lane_high_byte:
  movzbl (%rdi), %eax
  cmpl $9, %eax
  ja .Llb_other
  movb 1(%rdi), %ah
  cmpb $6, %al
  ja .Llb_other
  leaq .Llb_table(%rip), %rcx
  movslq (%rcx,%rax,4), %rdx
  addq %rcx, %rdx
  notrack jmpq *%rdx
.Llb_c0:
  movl $50, %eax
  ret
.Llb_c1:
  movl $51, %eax
  ret
.Llb_c2:
  movl $52, %eax
  ret
.Llb_c3:
  movl $53, %eax
  ret
.Llb_c4:
  movl $54, %eax
  ret
.Llb_c5:
  movl $55, %eax
  ret
.Llb_c6:
  movl $56, %eax
  ret
.Llb_other:
  movl $-1, %eax
  ret
  .size jt_lane_high_byte, .-jt_lane_high_byte

  .section .rodata
  .p2align 2
.Llc_table:
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c4-.Llc_table
  .long .Llc_c5-.Llc_table
  .long .Llc_c6-.Llc_table
  .long 0
.Llh_table:
  .long .Llh_zero-.Llh_table
  .long .Llh_c1-.Llh_table
  .long .Llh_c2-.Llh_table
  .long .Llh_c3-.Llh_table
  .long .Llh_c4-.Llh_table
  .long .Llh_c5-.Llh_table
  .long .Llh_c6-.Llh_table
  .long 0
.Llt_table:
  .long .Llt_c0-.Llt_table
  .long .Llt_c1-.Llt_table
  .long .Llt_c2-.Llt_table
  .long .Llt_c3-.Llt_table
  .long .Llt_c4-.Llt_table
  .long .Llt_c5-.Llt_table
  .long .Llt_c6-.Llt_table
  .long 0
.Llw_table:
  .long .Llw_c0-.Llw_table
  .long .Llw_c1-.Llw_table
  .long .Llw_c2-.Llw_table
  .long .Llw_c3-.Llw_table
  .long .Llw_c4-.Llw_table
  .long .Llw_c5-.Llw_table
  .long .Llw_c6-.Llw_table
  .long 0
.Llb_table:
  .long .Llb_c0-.Llb_table
  .long .Llb_c1-.Llb_table
  .long .Llb_c2-.Llb_table
  .long .Llb_c3-.Llb_table
  .long .Llb_c4-.Llb_table
  .long .Llb_c5-.Llb_table
  .long .Llb_c6-.Llb_table
  .long 0

  .section .note.GNU-stack,"",@progbits
