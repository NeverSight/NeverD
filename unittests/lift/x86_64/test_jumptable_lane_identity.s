# Dispatches whose index takes the compared lane only after the guard.
# Each sits behind 24 controlling compares on a value that grows by a
# multiply and an add, so one query session over every condition runs
# out of a small match allowance and only the compare that names the
# index can bound the table:
#   jt_lane_extend_after_guard   cmpb $5, %sil; ja; movzbl %sil, %esi
#   jt_lane_changed_after_guard  the same with incb %sil after the ja
#   jt_copy_after_guard          cmpl $5, %r11d; ja; movl %r11d, %eax
#   jt_copy_changed_after_guard  the same with incl %r11d after the ja
# Each table has six entries and a trailing zero word, which is no entry.

  .text
  .globl jt_lane_extend_after_guard
  .type jt_lane_extend_after_guard, @function
  .p2align 4
jt_lane_extend_after_guard:
  movl %edx, %r8d
  cmpl $7, %r8d
  je .Lle_exit
  imull $3, %r8d, %r8d
  addl $1, %r8d
  cmpl $8, %r8d
  je .Lle_exit
  imull $3, %r8d, %r8d
  addl $2, %r8d
  cmpl $9, %r8d
  je .Lle_exit
  imull $3, %r8d, %r8d
  addl $3, %r8d
  cmpl $10, %r8d
  je .Lle_exit
  imull $3, %r8d, %r8d
  addl $4, %r8d
  cmpl $11, %r8d
  je .Lle_exit
  imull $3, %r8d, %r8d
  addl $5, %r8d
  cmpl $12, %r8d
  je .Lle_exit
  imull $3, %r8d, %r8d
  addl $6, %r8d
  cmpl $13, %r8d
  je .Lle_exit
  imull $3, %r8d, %r8d
  addl $7, %r8d
  cmpl $14, %r8d
  je .Lle_exit
  imull $3, %r8d, %r8d
  addl $8, %r8d
  cmpl $15, %r8d
  je .Lle_exit
  imull $3, %r8d, %r8d
  addl $9, %r8d
  cmpl $16, %r8d
  je .Lle_exit
  imull $3, %r8d, %r8d
  addl $10, %r8d
  cmpl $17, %r8d
  je .Lle_exit
  imull $3, %r8d, %r8d
  addl $11, %r8d
  cmpl $18, %r8d
  je .Lle_exit
  imull $3, %r8d, %r8d
  addl $12, %r8d
  cmpl $19, %r8d
  je .Lle_exit
  imull $3, %r8d, %r8d
  addl $13, %r8d
  cmpl $20, %r8d
  je .Lle_exit
  imull $3, %r8d, %r8d
  addl $14, %r8d
  cmpl $21, %r8d
  je .Lle_exit
  imull $3, %r8d, %r8d
  addl $15, %r8d
  cmpl $22, %r8d
  je .Lle_exit
  imull $3, %r8d, %r8d
  addl $16, %r8d
  cmpl $23, %r8d
  je .Lle_exit
  imull $3, %r8d, %r8d
  addl $17, %r8d
  cmpl $24, %r8d
  je .Lle_exit
  imull $3, %r8d, %r8d
  addl $18, %r8d
  cmpl $25, %r8d
  je .Lle_exit
  imull $3, %r8d, %r8d
  addl $19, %r8d
  cmpl $26, %r8d
  je .Lle_exit
  imull $3, %r8d, %r8d
  addl $20, %r8d
  cmpl $27, %r8d
  je .Lle_exit
  imull $3, %r8d, %r8d
  addl $21, %r8d
  cmpl $28, %r8d
  je .Lle_exit
  imull $3, %r8d, %r8d
  addl $22, %r8d
  cmpl $29, %r8d
  je .Lle_exit
  imull $3, %r8d, %r8d
  addl $23, %r8d
  cmpl $30, %r8d
  je .Lle_exit
  imull $3, %r8d, %r8d
  addl $24, %r8d
  movzbl (%rdi), %esi
  subl $0x44, %esi
  cmpb $5, %sil
  ja .Lle_exit
  leaq .Lle_table(%rip), %rdx
  movzbl %sil, %esi
  movslq (%rdx,%rsi,4), %rsi
  addq %rdx, %rsi
  jmpq *%rsi
.Lle_c0:
  movl $10, %eax
  ret
.Lle_c1:
  movl $11, %eax
  ret
.Lle_c2:
  movl $12, %eax
  ret
.Lle_c3:
  movl $13, %eax
  ret
.Lle_c4:
  movl $14, %eax
  ret
.Lle_c5:
  movl $15, %eax
  ret
.Lle_exit:
  movl %r8d, %eax
  ret
  .size jt_lane_extend_after_guard, .-jt_lane_extend_after_guard

  .globl jt_lane_changed_after_guard
  .type jt_lane_changed_after_guard, @function
  .p2align 4
jt_lane_changed_after_guard:
  movl %edx, %r8d
  cmpl $7, %r8d
  je .Llc_exit
  imull $3, %r8d, %r8d
  addl $1, %r8d
  cmpl $8, %r8d
  je .Llc_exit
  imull $3, %r8d, %r8d
  addl $2, %r8d
  cmpl $9, %r8d
  je .Llc_exit
  imull $3, %r8d, %r8d
  addl $3, %r8d
  cmpl $10, %r8d
  je .Llc_exit
  imull $3, %r8d, %r8d
  addl $4, %r8d
  cmpl $11, %r8d
  je .Llc_exit
  imull $3, %r8d, %r8d
  addl $5, %r8d
  cmpl $12, %r8d
  je .Llc_exit
  imull $3, %r8d, %r8d
  addl $6, %r8d
  cmpl $13, %r8d
  je .Llc_exit
  imull $3, %r8d, %r8d
  addl $7, %r8d
  cmpl $14, %r8d
  je .Llc_exit
  imull $3, %r8d, %r8d
  addl $8, %r8d
  cmpl $15, %r8d
  je .Llc_exit
  imull $3, %r8d, %r8d
  addl $9, %r8d
  cmpl $16, %r8d
  je .Llc_exit
  imull $3, %r8d, %r8d
  addl $10, %r8d
  cmpl $17, %r8d
  je .Llc_exit
  imull $3, %r8d, %r8d
  addl $11, %r8d
  cmpl $18, %r8d
  je .Llc_exit
  imull $3, %r8d, %r8d
  addl $12, %r8d
  cmpl $19, %r8d
  je .Llc_exit
  imull $3, %r8d, %r8d
  addl $13, %r8d
  cmpl $20, %r8d
  je .Llc_exit
  imull $3, %r8d, %r8d
  addl $14, %r8d
  cmpl $21, %r8d
  je .Llc_exit
  imull $3, %r8d, %r8d
  addl $15, %r8d
  cmpl $22, %r8d
  je .Llc_exit
  imull $3, %r8d, %r8d
  addl $16, %r8d
  cmpl $23, %r8d
  je .Llc_exit
  imull $3, %r8d, %r8d
  addl $17, %r8d
  cmpl $24, %r8d
  je .Llc_exit
  imull $3, %r8d, %r8d
  addl $18, %r8d
  cmpl $25, %r8d
  je .Llc_exit
  imull $3, %r8d, %r8d
  addl $19, %r8d
  cmpl $26, %r8d
  je .Llc_exit
  imull $3, %r8d, %r8d
  addl $20, %r8d
  cmpl $27, %r8d
  je .Llc_exit
  imull $3, %r8d, %r8d
  addl $21, %r8d
  cmpl $28, %r8d
  je .Llc_exit
  imull $3, %r8d, %r8d
  addl $22, %r8d
  cmpl $29, %r8d
  je .Llc_exit
  imull $3, %r8d, %r8d
  addl $23, %r8d
  cmpl $30, %r8d
  je .Llc_exit
  imull $3, %r8d, %r8d
  addl $24, %r8d
  movzbl (%rdi), %esi
  subl $0x44, %esi
  cmpb $5, %sil
  ja .Llc_exit
  leaq .Llc_table(%rip), %rdx
  incb %sil
  movzbl %sil, %esi
  movslq (%rdx,%rsi,4), %rsi
  addq %rdx, %rsi
  jmpq *%rsi
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
.Llc_exit:
  movl %r8d, %eax
  ret
  .size jt_lane_changed_after_guard, .-jt_lane_changed_after_guard

  .globl jt_copy_after_guard
  .type jt_copy_after_guard, @function
  .p2align 4
jt_copy_after_guard:
  movl %edx, %r8d
  cmpl $7, %r8d
  je .Lce_exit
  imull $3, %r8d, %r8d
  addl $1, %r8d
  cmpl $8, %r8d
  je .Lce_exit
  imull $3, %r8d, %r8d
  addl $2, %r8d
  cmpl $9, %r8d
  je .Lce_exit
  imull $3, %r8d, %r8d
  addl $3, %r8d
  cmpl $10, %r8d
  je .Lce_exit
  imull $3, %r8d, %r8d
  addl $4, %r8d
  cmpl $11, %r8d
  je .Lce_exit
  imull $3, %r8d, %r8d
  addl $5, %r8d
  cmpl $12, %r8d
  je .Lce_exit
  imull $3, %r8d, %r8d
  addl $6, %r8d
  cmpl $13, %r8d
  je .Lce_exit
  imull $3, %r8d, %r8d
  addl $7, %r8d
  cmpl $14, %r8d
  je .Lce_exit
  imull $3, %r8d, %r8d
  addl $8, %r8d
  cmpl $15, %r8d
  je .Lce_exit
  imull $3, %r8d, %r8d
  addl $9, %r8d
  cmpl $16, %r8d
  je .Lce_exit
  imull $3, %r8d, %r8d
  addl $10, %r8d
  cmpl $17, %r8d
  je .Lce_exit
  imull $3, %r8d, %r8d
  addl $11, %r8d
  cmpl $18, %r8d
  je .Lce_exit
  imull $3, %r8d, %r8d
  addl $12, %r8d
  cmpl $19, %r8d
  je .Lce_exit
  imull $3, %r8d, %r8d
  addl $13, %r8d
  cmpl $20, %r8d
  je .Lce_exit
  imull $3, %r8d, %r8d
  addl $14, %r8d
  cmpl $21, %r8d
  je .Lce_exit
  imull $3, %r8d, %r8d
  addl $15, %r8d
  cmpl $22, %r8d
  je .Lce_exit
  imull $3, %r8d, %r8d
  addl $16, %r8d
  cmpl $23, %r8d
  je .Lce_exit
  imull $3, %r8d, %r8d
  addl $17, %r8d
  cmpl $24, %r8d
  je .Lce_exit
  imull $3, %r8d, %r8d
  addl $18, %r8d
  cmpl $25, %r8d
  je .Lce_exit
  imull $3, %r8d, %r8d
  addl $19, %r8d
  cmpl $26, %r8d
  je .Lce_exit
  imull $3, %r8d, %r8d
  addl $20, %r8d
  cmpl $27, %r8d
  je .Lce_exit
  imull $3, %r8d, %r8d
  addl $21, %r8d
  cmpl $28, %r8d
  je .Lce_exit
  imull $3, %r8d, %r8d
  addl $22, %r8d
  cmpl $29, %r8d
  je .Lce_exit
  imull $3, %r8d, %r8d
  addl $23, %r8d
  cmpl $30, %r8d
  je .Lce_exit
  imull $3, %r8d, %r8d
  addl $24, %r8d
  movzbl (%rdi), %r11d
  cmpl $5, %r11d
  ja .Lce_exit
  leaq .Lce_table(%rip), %rdx
  movl %r11d, %eax
  movslq (%rdx,%rax,4), %rax
  addq %rdx, %rax
  jmpq *%rax
.Lce_c0:
  movl $10, %eax
  ret
.Lce_c1:
  movl $11, %eax
  ret
.Lce_c2:
  movl $12, %eax
  ret
.Lce_c3:
  movl $13, %eax
  ret
.Lce_c4:
  movl $14, %eax
  ret
.Lce_c5:
  movl $15, %eax
  ret
.Lce_exit:
  movl %r8d, %eax
  ret
  .size jt_copy_after_guard, .-jt_copy_after_guard

  .globl jt_copy_changed_after_guard
  .type jt_copy_changed_after_guard, @function
  .p2align 4
jt_copy_changed_after_guard:
  movl %edx, %r8d
  cmpl $7, %r8d
  je .Lcc_exit
  imull $3, %r8d, %r8d
  addl $1, %r8d
  cmpl $8, %r8d
  je .Lcc_exit
  imull $3, %r8d, %r8d
  addl $2, %r8d
  cmpl $9, %r8d
  je .Lcc_exit
  imull $3, %r8d, %r8d
  addl $3, %r8d
  cmpl $10, %r8d
  je .Lcc_exit
  imull $3, %r8d, %r8d
  addl $4, %r8d
  cmpl $11, %r8d
  je .Lcc_exit
  imull $3, %r8d, %r8d
  addl $5, %r8d
  cmpl $12, %r8d
  je .Lcc_exit
  imull $3, %r8d, %r8d
  addl $6, %r8d
  cmpl $13, %r8d
  je .Lcc_exit
  imull $3, %r8d, %r8d
  addl $7, %r8d
  cmpl $14, %r8d
  je .Lcc_exit
  imull $3, %r8d, %r8d
  addl $8, %r8d
  cmpl $15, %r8d
  je .Lcc_exit
  imull $3, %r8d, %r8d
  addl $9, %r8d
  cmpl $16, %r8d
  je .Lcc_exit
  imull $3, %r8d, %r8d
  addl $10, %r8d
  cmpl $17, %r8d
  je .Lcc_exit
  imull $3, %r8d, %r8d
  addl $11, %r8d
  cmpl $18, %r8d
  je .Lcc_exit
  imull $3, %r8d, %r8d
  addl $12, %r8d
  cmpl $19, %r8d
  je .Lcc_exit
  imull $3, %r8d, %r8d
  addl $13, %r8d
  cmpl $20, %r8d
  je .Lcc_exit
  imull $3, %r8d, %r8d
  addl $14, %r8d
  cmpl $21, %r8d
  je .Lcc_exit
  imull $3, %r8d, %r8d
  addl $15, %r8d
  cmpl $22, %r8d
  je .Lcc_exit
  imull $3, %r8d, %r8d
  addl $16, %r8d
  cmpl $23, %r8d
  je .Lcc_exit
  imull $3, %r8d, %r8d
  addl $17, %r8d
  cmpl $24, %r8d
  je .Lcc_exit
  imull $3, %r8d, %r8d
  addl $18, %r8d
  cmpl $25, %r8d
  je .Lcc_exit
  imull $3, %r8d, %r8d
  addl $19, %r8d
  cmpl $26, %r8d
  je .Lcc_exit
  imull $3, %r8d, %r8d
  addl $20, %r8d
  cmpl $27, %r8d
  je .Lcc_exit
  imull $3, %r8d, %r8d
  addl $21, %r8d
  cmpl $28, %r8d
  je .Lcc_exit
  imull $3, %r8d, %r8d
  addl $22, %r8d
  cmpl $29, %r8d
  je .Lcc_exit
  imull $3, %r8d, %r8d
  addl $23, %r8d
  cmpl $30, %r8d
  je .Lcc_exit
  imull $3, %r8d, %r8d
  addl $24, %r8d
  movzbl (%rdi), %r11d
  cmpl $5, %r11d
  ja .Lcc_exit
  leaq .Lcc_table(%rip), %rdx
  incl %r11d
  movl %r11d, %eax
  movslq (%rdx,%rax,4), %rax
  addq %rdx, %rax
  jmpq *%rax
.Lcc_c0:
  movl $10, %eax
  ret
.Lcc_c1:
  movl $11, %eax
  ret
.Lcc_c2:
  movl $12, %eax
  ret
.Lcc_c3:
  movl $13, %eax
  ret
.Lcc_c4:
  movl $14, %eax
  ret
.Lcc_c5:
  movl $15, %eax
  ret
.Lcc_exit:
  movl %r8d, %eax
  ret
  .size jt_copy_changed_after_guard, .-jt_copy_changed_after_guard

  .section .rodata
  .p2align 2
.Lle_table:
  .long .Lle_c0-.Lle_table
  .long .Lle_c1-.Lle_table
  .long .Lle_c2-.Lle_table
  .long .Lle_c3-.Lle_table
  .long .Lle_c4-.Lle_table
  .long .Lle_c5-.Lle_table
  .long 0
  .p2align 2
.Llc_table:
  .long .Llc_c0-.Llc_table
  .long .Llc_c1-.Llc_table
  .long .Llc_c2-.Llc_table
  .long .Llc_c3-.Llc_table
  .long .Llc_c4-.Llc_table
  .long .Llc_c5-.Llc_table
  .long 0
  .p2align 2
.Lce_table:
  .long .Lce_c0-.Lce_table
  .long .Lce_c1-.Lce_table
  .long .Lce_c2-.Lce_table
  .long .Lce_c3-.Lce_table
  .long .Lce_c4-.Lce_table
  .long .Lce_c5-.Lce_table
  .long 0
  .p2align 2
.Lcc_table:
  .long .Lcc_c0-.Lcc_table
  .long .Lcc_c1-.Lcc_table
  .long .Lcc_c2-.Lcc_table
  .long .Lcc_c3-.Lcc_table
  .long .Lcc_c4-.Lcc_table
  .long .Lcc_c5-.Lcc_table
  .long 0

  .section .note.GNU-stack,"",@progbits
