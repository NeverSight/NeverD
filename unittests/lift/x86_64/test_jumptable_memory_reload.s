# Dispatches that compare a byte in memory and reload it as the table
# index, the shape of pcre2_dfa_match (`cmpb $0xc,0x2(%r11); ja; movzbl
# 0x2(%r11),%edx`).  Each sits behind 24 controlling compares on a value
# that grows by a multiply and an add, so one query session over every
# condition runs out of a small match allowance:
#   jt_mem_reload              cmpb $5, 2(%rdi); ja; movzbl 2(%rdi), %eax
#   jt_mem_reload_store        a store to the byte between the two loads
#   jt_mem_reload_carrier      %rdi moves between the two loads
#   jt_mem_reload_disp         the reload reads 3(%rdi)
#   jt_mem_reload_wide         the reload is movzwl
#   jt_mem_reload_signed       cmpb $0x81, then a sign-extending movsbq
#   jt_mem_reload_call         a call between the reload and the dispatch
#   jt_mem_reload_partial      orb $0x80, %al after the reload
#   jt_mem_reload_second_pred  a second path enters the reload block
#   jt_mem_reload_rooted       the reload block's address is data
# Each table has six entries and a zero word, which is no entry;
# jt_mem_reload_signed's has 130.

  .text
  .globl jt_mem_reload
  .type jt_mem_reload, @function
  .p2align 4
jt_mem_reload:
  movl %esi, %r9d
  cmpl $7, %r9d
  je .Ljt_mem_reload_default
  imull $3, %r9d, %r9d
  addl $1, %r9d
  cmpl $8, %r9d
  je .Ljt_mem_reload_default
  imull $3, %r9d, %r9d
  addl $2, %r9d
  cmpl $9, %r9d
  je .Ljt_mem_reload_default
  imull $3, %r9d, %r9d
  addl $3, %r9d
  cmpl $10, %r9d
  je .Ljt_mem_reload_default
  imull $3, %r9d, %r9d
  addl $4, %r9d
  cmpl $11, %r9d
  je .Ljt_mem_reload_default
  imull $3, %r9d, %r9d
  addl $5, %r9d
  cmpl $12, %r9d
  je .Ljt_mem_reload_default
  imull $3, %r9d, %r9d
  addl $6, %r9d
  cmpl $13, %r9d
  je .Ljt_mem_reload_default
  imull $3, %r9d, %r9d
  addl $7, %r9d
  cmpl $14, %r9d
  je .Ljt_mem_reload_default
  imull $3, %r9d, %r9d
  addl $8, %r9d
  cmpl $15, %r9d
  je .Ljt_mem_reload_default
  imull $3, %r9d, %r9d
  addl $9, %r9d
  cmpl $16, %r9d
  je .Ljt_mem_reload_default
  imull $3, %r9d, %r9d
  addl $10, %r9d
  cmpl $17, %r9d
  je .Ljt_mem_reload_default
  imull $3, %r9d, %r9d
  addl $11, %r9d
  cmpl $18, %r9d
  je .Ljt_mem_reload_default
  imull $3, %r9d, %r9d
  addl $12, %r9d
  cmpl $19, %r9d
  je .Ljt_mem_reload_default
  imull $3, %r9d, %r9d
  addl $13, %r9d
  cmpl $20, %r9d
  je .Ljt_mem_reload_default
  imull $3, %r9d, %r9d
  addl $14, %r9d
  cmpl $21, %r9d
  je .Ljt_mem_reload_default
  imull $3, %r9d, %r9d
  addl $15, %r9d
  cmpl $22, %r9d
  je .Ljt_mem_reload_default
  imull $3, %r9d, %r9d
  addl $16, %r9d
  cmpl $23, %r9d
  je .Ljt_mem_reload_default
  imull $3, %r9d, %r9d
  addl $17, %r9d
  cmpl $24, %r9d
  je .Ljt_mem_reload_default
  imull $3, %r9d, %r9d
  addl $18, %r9d
  cmpl $25, %r9d
  je .Ljt_mem_reload_default
  imull $3, %r9d, %r9d
  addl $19, %r9d
  cmpl $26, %r9d
  je .Ljt_mem_reload_default
  imull $3, %r9d, %r9d
  addl $20, %r9d
  cmpl $27, %r9d
  je .Ljt_mem_reload_default
  imull $3, %r9d, %r9d
  addl $21, %r9d
  cmpl $28, %r9d
  je .Ljt_mem_reload_default
  imull $3, %r9d, %r9d
  addl $22, %r9d
  cmpl $29, %r9d
  je .Ljt_mem_reload_default
  imull $3, %r9d, %r9d
  addl $23, %r9d
  cmpl $30, %r9d
  je .Ljt_mem_reload_default
  imull $3, %r9d, %r9d
  addl $24, %r9d
  cmpb $5, 2(%rdi)
  ja .Ljt_mem_reload_default
.Ljt_mem_reload_idx:
  movzbl 2(%rdi), %eax
  leaq .Ljt_mem_reload_table(%rip), %rcx
  movslq (%rcx,%rax,4), %rax
  addq %rcx, %rax
  jmpq *%rax
.Ljt_mem_reload_c0:
  movl $100, %eax
  ret
.Ljt_mem_reload_c1:
  movl $101, %eax
  ret
.Ljt_mem_reload_c2:
  movl $102, %eax
  ret
.Ljt_mem_reload_c3:
  movl $103, %eax
  ret
.Ljt_mem_reload_c4:
  movl $104, %eax
  ret
.Ljt_mem_reload_c5:
  movl $105, %eax
  ret
.Ljt_mem_reload_default:
  movl $-1, %eax
  ret
  .size jt_mem_reload, .-jt_mem_reload
  .globl jt_mem_reload_store
  .type jt_mem_reload_store, @function
  .p2align 4
jt_mem_reload_store:
  movl %esi, %r9d
  cmpl $7, %r9d
  je .Ljt_mem_reload_store_default
  imull $3, %r9d, %r9d
  addl $1, %r9d
  cmpl $8, %r9d
  je .Ljt_mem_reload_store_default
  imull $3, %r9d, %r9d
  addl $2, %r9d
  cmpl $9, %r9d
  je .Ljt_mem_reload_store_default
  imull $3, %r9d, %r9d
  addl $3, %r9d
  cmpl $10, %r9d
  je .Ljt_mem_reload_store_default
  imull $3, %r9d, %r9d
  addl $4, %r9d
  cmpl $11, %r9d
  je .Ljt_mem_reload_store_default
  imull $3, %r9d, %r9d
  addl $5, %r9d
  cmpl $12, %r9d
  je .Ljt_mem_reload_store_default
  imull $3, %r9d, %r9d
  addl $6, %r9d
  cmpl $13, %r9d
  je .Ljt_mem_reload_store_default
  imull $3, %r9d, %r9d
  addl $7, %r9d
  cmpl $14, %r9d
  je .Ljt_mem_reload_store_default
  imull $3, %r9d, %r9d
  addl $8, %r9d
  cmpl $15, %r9d
  je .Ljt_mem_reload_store_default
  imull $3, %r9d, %r9d
  addl $9, %r9d
  cmpl $16, %r9d
  je .Ljt_mem_reload_store_default
  imull $3, %r9d, %r9d
  addl $10, %r9d
  cmpl $17, %r9d
  je .Ljt_mem_reload_store_default
  imull $3, %r9d, %r9d
  addl $11, %r9d
  cmpl $18, %r9d
  je .Ljt_mem_reload_store_default
  imull $3, %r9d, %r9d
  addl $12, %r9d
  cmpl $19, %r9d
  je .Ljt_mem_reload_store_default
  imull $3, %r9d, %r9d
  addl $13, %r9d
  cmpl $20, %r9d
  je .Ljt_mem_reload_store_default
  imull $3, %r9d, %r9d
  addl $14, %r9d
  cmpl $21, %r9d
  je .Ljt_mem_reload_store_default
  imull $3, %r9d, %r9d
  addl $15, %r9d
  cmpl $22, %r9d
  je .Ljt_mem_reload_store_default
  imull $3, %r9d, %r9d
  addl $16, %r9d
  cmpl $23, %r9d
  je .Ljt_mem_reload_store_default
  imull $3, %r9d, %r9d
  addl $17, %r9d
  cmpl $24, %r9d
  je .Ljt_mem_reload_store_default
  imull $3, %r9d, %r9d
  addl $18, %r9d
  cmpl $25, %r9d
  je .Ljt_mem_reload_store_default
  imull $3, %r9d, %r9d
  addl $19, %r9d
  cmpl $26, %r9d
  je .Ljt_mem_reload_store_default
  imull $3, %r9d, %r9d
  addl $20, %r9d
  cmpl $27, %r9d
  je .Ljt_mem_reload_store_default
  imull $3, %r9d, %r9d
  addl $21, %r9d
  cmpl $28, %r9d
  je .Ljt_mem_reload_store_default
  imull $3, %r9d, %r9d
  addl $22, %r9d
  cmpl $29, %r9d
  je .Ljt_mem_reload_store_default
  imull $3, %r9d, %r9d
  addl $23, %r9d
  cmpl $30, %r9d
  je .Ljt_mem_reload_store_default
  imull $3, %r9d, %r9d
  addl $24, %r9d
  cmpb $5, 2(%rdi)
  ja .Ljt_mem_reload_store_default
  movb %dl, 2(%rdi)
.Ljt_mem_reload_store_idx:
  movzbl 2(%rdi), %eax
  leaq .Ljt_mem_reload_store_table(%rip), %rcx
  movslq (%rcx,%rax,4), %rax
  addq %rcx, %rax
  jmpq *%rax
.Ljt_mem_reload_store_c0:
  movl $100, %eax
  ret
.Ljt_mem_reload_store_c1:
  movl $101, %eax
  ret
.Ljt_mem_reload_store_c2:
  movl $102, %eax
  ret
.Ljt_mem_reload_store_c3:
  movl $103, %eax
  ret
.Ljt_mem_reload_store_c4:
  movl $104, %eax
  ret
.Ljt_mem_reload_store_c5:
  movl $105, %eax
  ret
.Ljt_mem_reload_store_default:
  movl $-1, %eax
  ret
  .size jt_mem_reload_store, .-jt_mem_reload_store
  .globl jt_mem_reload_carrier
  .type jt_mem_reload_carrier, @function
  .p2align 4
jt_mem_reload_carrier:
  movl %esi, %r9d
  cmpl $7, %r9d
  je .Ljt_mem_reload_carrier_default
  imull $3, %r9d, %r9d
  addl $1, %r9d
  cmpl $8, %r9d
  je .Ljt_mem_reload_carrier_default
  imull $3, %r9d, %r9d
  addl $2, %r9d
  cmpl $9, %r9d
  je .Ljt_mem_reload_carrier_default
  imull $3, %r9d, %r9d
  addl $3, %r9d
  cmpl $10, %r9d
  je .Ljt_mem_reload_carrier_default
  imull $3, %r9d, %r9d
  addl $4, %r9d
  cmpl $11, %r9d
  je .Ljt_mem_reload_carrier_default
  imull $3, %r9d, %r9d
  addl $5, %r9d
  cmpl $12, %r9d
  je .Ljt_mem_reload_carrier_default
  imull $3, %r9d, %r9d
  addl $6, %r9d
  cmpl $13, %r9d
  je .Ljt_mem_reload_carrier_default
  imull $3, %r9d, %r9d
  addl $7, %r9d
  cmpl $14, %r9d
  je .Ljt_mem_reload_carrier_default
  imull $3, %r9d, %r9d
  addl $8, %r9d
  cmpl $15, %r9d
  je .Ljt_mem_reload_carrier_default
  imull $3, %r9d, %r9d
  addl $9, %r9d
  cmpl $16, %r9d
  je .Ljt_mem_reload_carrier_default
  imull $3, %r9d, %r9d
  addl $10, %r9d
  cmpl $17, %r9d
  je .Ljt_mem_reload_carrier_default
  imull $3, %r9d, %r9d
  addl $11, %r9d
  cmpl $18, %r9d
  je .Ljt_mem_reload_carrier_default
  imull $3, %r9d, %r9d
  addl $12, %r9d
  cmpl $19, %r9d
  je .Ljt_mem_reload_carrier_default
  imull $3, %r9d, %r9d
  addl $13, %r9d
  cmpl $20, %r9d
  je .Ljt_mem_reload_carrier_default
  imull $3, %r9d, %r9d
  addl $14, %r9d
  cmpl $21, %r9d
  je .Ljt_mem_reload_carrier_default
  imull $3, %r9d, %r9d
  addl $15, %r9d
  cmpl $22, %r9d
  je .Ljt_mem_reload_carrier_default
  imull $3, %r9d, %r9d
  addl $16, %r9d
  cmpl $23, %r9d
  je .Ljt_mem_reload_carrier_default
  imull $3, %r9d, %r9d
  addl $17, %r9d
  cmpl $24, %r9d
  je .Ljt_mem_reload_carrier_default
  imull $3, %r9d, %r9d
  addl $18, %r9d
  cmpl $25, %r9d
  je .Ljt_mem_reload_carrier_default
  imull $3, %r9d, %r9d
  addl $19, %r9d
  cmpl $26, %r9d
  je .Ljt_mem_reload_carrier_default
  imull $3, %r9d, %r9d
  addl $20, %r9d
  cmpl $27, %r9d
  je .Ljt_mem_reload_carrier_default
  imull $3, %r9d, %r9d
  addl $21, %r9d
  cmpl $28, %r9d
  je .Ljt_mem_reload_carrier_default
  imull $3, %r9d, %r9d
  addl $22, %r9d
  cmpl $29, %r9d
  je .Ljt_mem_reload_carrier_default
  imull $3, %r9d, %r9d
  addl $23, %r9d
  cmpl $30, %r9d
  je .Ljt_mem_reload_carrier_default
  imull $3, %r9d, %r9d
  addl $24, %r9d
  cmpb $5, 2(%rdi)
  ja .Ljt_mem_reload_carrier_default
  addq $1, %rdi
.Ljt_mem_reload_carrier_idx:
  movzbl 2(%rdi), %eax
  leaq .Ljt_mem_reload_carrier_table(%rip), %rcx
  movslq (%rcx,%rax,4), %rax
  addq %rcx, %rax
  jmpq *%rax
.Ljt_mem_reload_carrier_c0:
  movl $100, %eax
  ret
.Ljt_mem_reload_carrier_c1:
  movl $101, %eax
  ret
.Ljt_mem_reload_carrier_c2:
  movl $102, %eax
  ret
.Ljt_mem_reload_carrier_c3:
  movl $103, %eax
  ret
.Ljt_mem_reload_carrier_c4:
  movl $104, %eax
  ret
.Ljt_mem_reload_carrier_c5:
  movl $105, %eax
  ret
.Ljt_mem_reload_carrier_default:
  movl $-1, %eax
  ret
  .size jt_mem_reload_carrier, .-jt_mem_reload_carrier
  .globl jt_mem_reload_disp
  .type jt_mem_reload_disp, @function
  .p2align 4
jt_mem_reload_disp:
  movl %esi, %r9d
  cmpl $7, %r9d
  je .Ljt_mem_reload_disp_default
  imull $3, %r9d, %r9d
  addl $1, %r9d
  cmpl $8, %r9d
  je .Ljt_mem_reload_disp_default
  imull $3, %r9d, %r9d
  addl $2, %r9d
  cmpl $9, %r9d
  je .Ljt_mem_reload_disp_default
  imull $3, %r9d, %r9d
  addl $3, %r9d
  cmpl $10, %r9d
  je .Ljt_mem_reload_disp_default
  imull $3, %r9d, %r9d
  addl $4, %r9d
  cmpl $11, %r9d
  je .Ljt_mem_reload_disp_default
  imull $3, %r9d, %r9d
  addl $5, %r9d
  cmpl $12, %r9d
  je .Ljt_mem_reload_disp_default
  imull $3, %r9d, %r9d
  addl $6, %r9d
  cmpl $13, %r9d
  je .Ljt_mem_reload_disp_default
  imull $3, %r9d, %r9d
  addl $7, %r9d
  cmpl $14, %r9d
  je .Ljt_mem_reload_disp_default
  imull $3, %r9d, %r9d
  addl $8, %r9d
  cmpl $15, %r9d
  je .Ljt_mem_reload_disp_default
  imull $3, %r9d, %r9d
  addl $9, %r9d
  cmpl $16, %r9d
  je .Ljt_mem_reload_disp_default
  imull $3, %r9d, %r9d
  addl $10, %r9d
  cmpl $17, %r9d
  je .Ljt_mem_reload_disp_default
  imull $3, %r9d, %r9d
  addl $11, %r9d
  cmpl $18, %r9d
  je .Ljt_mem_reload_disp_default
  imull $3, %r9d, %r9d
  addl $12, %r9d
  cmpl $19, %r9d
  je .Ljt_mem_reload_disp_default
  imull $3, %r9d, %r9d
  addl $13, %r9d
  cmpl $20, %r9d
  je .Ljt_mem_reload_disp_default
  imull $3, %r9d, %r9d
  addl $14, %r9d
  cmpl $21, %r9d
  je .Ljt_mem_reload_disp_default
  imull $3, %r9d, %r9d
  addl $15, %r9d
  cmpl $22, %r9d
  je .Ljt_mem_reload_disp_default
  imull $3, %r9d, %r9d
  addl $16, %r9d
  cmpl $23, %r9d
  je .Ljt_mem_reload_disp_default
  imull $3, %r9d, %r9d
  addl $17, %r9d
  cmpl $24, %r9d
  je .Ljt_mem_reload_disp_default
  imull $3, %r9d, %r9d
  addl $18, %r9d
  cmpl $25, %r9d
  je .Ljt_mem_reload_disp_default
  imull $3, %r9d, %r9d
  addl $19, %r9d
  cmpl $26, %r9d
  je .Ljt_mem_reload_disp_default
  imull $3, %r9d, %r9d
  addl $20, %r9d
  cmpl $27, %r9d
  je .Ljt_mem_reload_disp_default
  imull $3, %r9d, %r9d
  addl $21, %r9d
  cmpl $28, %r9d
  je .Ljt_mem_reload_disp_default
  imull $3, %r9d, %r9d
  addl $22, %r9d
  cmpl $29, %r9d
  je .Ljt_mem_reload_disp_default
  imull $3, %r9d, %r9d
  addl $23, %r9d
  cmpl $30, %r9d
  je .Ljt_mem_reload_disp_default
  imull $3, %r9d, %r9d
  addl $24, %r9d
  cmpb $5, 2(%rdi)
  ja .Ljt_mem_reload_disp_default
.Ljt_mem_reload_disp_idx:
  movzbl 3(%rdi), %eax
  leaq .Ljt_mem_reload_disp_table(%rip), %rcx
  movslq (%rcx,%rax,4), %rax
  addq %rcx, %rax
  jmpq *%rax
.Ljt_mem_reload_disp_c0:
  movl $100, %eax
  ret
.Ljt_mem_reload_disp_c1:
  movl $101, %eax
  ret
.Ljt_mem_reload_disp_c2:
  movl $102, %eax
  ret
.Ljt_mem_reload_disp_c3:
  movl $103, %eax
  ret
.Ljt_mem_reload_disp_c4:
  movl $104, %eax
  ret
.Ljt_mem_reload_disp_c5:
  movl $105, %eax
  ret
.Ljt_mem_reload_disp_default:
  movl $-1, %eax
  ret
  .size jt_mem_reload_disp, .-jt_mem_reload_disp
  .globl jt_mem_reload_wide
  .type jt_mem_reload_wide, @function
  .p2align 4
jt_mem_reload_wide:
  movl %esi, %r9d
  cmpl $7, %r9d
  je .Ljt_mem_reload_wide_default
  imull $3, %r9d, %r9d
  addl $1, %r9d
  cmpl $8, %r9d
  je .Ljt_mem_reload_wide_default
  imull $3, %r9d, %r9d
  addl $2, %r9d
  cmpl $9, %r9d
  je .Ljt_mem_reload_wide_default
  imull $3, %r9d, %r9d
  addl $3, %r9d
  cmpl $10, %r9d
  je .Ljt_mem_reload_wide_default
  imull $3, %r9d, %r9d
  addl $4, %r9d
  cmpl $11, %r9d
  je .Ljt_mem_reload_wide_default
  imull $3, %r9d, %r9d
  addl $5, %r9d
  cmpl $12, %r9d
  je .Ljt_mem_reload_wide_default
  imull $3, %r9d, %r9d
  addl $6, %r9d
  cmpl $13, %r9d
  je .Ljt_mem_reload_wide_default
  imull $3, %r9d, %r9d
  addl $7, %r9d
  cmpl $14, %r9d
  je .Ljt_mem_reload_wide_default
  imull $3, %r9d, %r9d
  addl $8, %r9d
  cmpl $15, %r9d
  je .Ljt_mem_reload_wide_default
  imull $3, %r9d, %r9d
  addl $9, %r9d
  cmpl $16, %r9d
  je .Ljt_mem_reload_wide_default
  imull $3, %r9d, %r9d
  addl $10, %r9d
  cmpl $17, %r9d
  je .Ljt_mem_reload_wide_default
  imull $3, %r9d, %r9d
  addl $11, %r9d
  cmpl $18, %r9d
  je .Ljt_mem_reload_wide_default
  imull $3, %r9d, %r9d
  addl $12, %r9d
  cmpl $19, %r9d
  je .Ljt_mem_reload_wide_default
  imull $3, %r9d, %r9d
  addl $13, %r9d
  cmpl $20, %r9d
  je .Ljt_mem_reload_wide_default
  imull $3, %r9d, %r9d
  addl $14, %r9d
  cmpl $21, %r9d
  je .Ljt_mem_reload_wide_default
  imull $3, %r9d, %r9d
  addl $15, %r9d
  cmpl $22, %r9d
  je .Ljt_mem_reload_wide_default
  imull $3, %r9d, %r9d
  addl $16, %r9d
  cmpl $23, %r9d
  je .Ljt_mem_reload_wide_default
  imull $3, %r9d, %r9d
  addl $17, %r9d
  cmpl $24, %r9d
  je .Ljt_mem_reload_wide_default
  imull $3, %r9d, %r9d
  addl $18, %r9d
  cmpl $25, %r9d
  je .Ljt_mem_reload_wide_default
  imull $3, %r9d, %r9d
  addl $19, %r9d
  cmpl $26, %r9d
  je .Ljt_mem_reload_wide_default
  imull $3, %r9d, %r9d
  addl $20, %r9d
  cmpl $27, %r9d
  je .Ljt_mem_reload_wide_default
  imull $3, %r9d, %r9d
  addl $21, %r9d
  cmpl $28, %r9d
  je .Ljt_mem_reload_wide_default
  imull $3, %r9d, %r9d
  addl $22, %r9d
  cmpl $29, %r9d
  je .Ljt_mem_reload_wide_default
  imull $3, %r9d, %r9d
  addl $23, %r9d
  cmpl $30, %r9d
  je .Ljt_mem_reload_wide_default
  imull $3, %r9d, %r9d
  addl $24, %r9d
  cmpb $5, 2(%rdi)
  ja .Ljt_mem_reload_wide_default
.Ljt_mem_reload_wide_idx:
  movzwl 2(%rdi), %eax
  leaq .Ljt_mem_reload_wide_table(%rip), %rcx
  movslq (%rcx,%rax,4), %rax
  addq %rcx, %rax
  jmpq *%rax
.Ljt_mem_reload_wide_c0:
  movl $100, %eax
  ret
.Ljt_mem_reload_wide_c1:
  movl $101, %eax
  ret
.Ljt_mem_reload_wide_c2:
  movl $102, %eax
  ret
.Ljt_mem_reload_wide_c3:
  movl $103, %eax
  ret
.Ljt_mem_reload_wide_c4:
  movl $104, %eax
  ret
.Ljt_mem_reload_wide_c5:
  movl $105, %eax
  ret
.Ljt_mem_reload_wide_default:
  movl $-1, %eax
  ret
  .size jt_mem_reload_wide, .-jt_mem_reload_wide
  .globl jt_mem_reload_signed
  .type jt_mem_reload_signed, @function
  .p2align 4
jt_mem_reload_signed:
  movl %esi, %r9d
  cmpl $7, %r9d
  je .Ljt_mem_reload_signed_default
  imull $3, %r9d, %r9d
  addl $1, %r9d
  cmpl $8, %r9d
  je .Ljt_mem_reload_signed_default
  imull $3, %r9d, %r9d
  addl $2, %r9d
  cmpl $9, %r9d
  je .Ljt_mem_reload_signed_default
  imull $3, %r9d, %r9d
  addl $3, %r9d
  cmpl $10, %r9d
  je .Ljt_mem_reload_signed_default
  imull $3, %r9d, %r9d
  addl $4, %r9d
  cmpl $11, %r9d
  je .Ljt_mem_reload_signed_default
  imull $3, %r9d, %r9d
  addl $5, %r9d
  cmpl $12, %r9d
  je .Ljt_mem_reload_signed_default
  imull $3, %r9d, %r9d
  addl $6, %r9d
  cmpl $13, %r9d
  je .Ljt_mem_reload_signed_default
  imull $3, %r9d, %r9d
  addl $7, %r9d
  cmpl $14, %r9d
  je .Ljt_mem_reload_signed_default
  imull $3, %r9d, %r9d
  addl $8, %r9d
  cmpl $15, %r9d
  je .Ljt_mem_reload_signed_default
  imull $3, %r9d, %r9d
  addl $9, %r9d
  cmpl $16, %r9d
  je .Ljt_mem_reload_signed_default
  imull $3, %r9d, %r9d
  addl $10, %r9d
  cmpl $17, %r9d
  je .Ljt_mem_reload_signed_default
  imull $3, %r9d, %r9d
  addl $11, %r9d
  cmpl $18, %r9d
  je .Ljt_mem_reload_signed_default
  imull $3, %r9d, %r9d
  addl $12, %r9d
  cmpl $19, %r9d
  je .Ljt_mem_reload_signed_default
  imull $3, %r9d, %r9d
  addl $13, %r9d
  cmpl $20, %r9d
  je .Ljt_mem_reload_signed_default
  imull $3, %r9d, %r9d
  addl $14, %r9d
  cmpl $21, %r9d
  je .Ljt_mem_reload_signed_default
  imull $3, %r9d, %r9d
  addl $15, %r9d
  cmpl $22, %r9d
  je .Ljt_mem_reload_signed_default
  imull $3, %r9d, %r9d
  addl $16, %r9d
  cmpl $23, %r9d
  je .Ljt_mem_reload_signed_default
  imull $3, %r9d, %r9d
  addl $17, %r9d
  cmpl $24, %r9d
  je .Ljt_mem_reload_signed_default
  imull $3, %r9d, %r9d
  addl $18, %r9d
  cmpl $25, %r9d
  je .Ljt_mem_reload_signed_default
  imull $3, %r9d, %r9d
  addl $19, %r9d
  cmpl $26, %r9d
  je .Ljt_mem_reload_signed_default
  imull $3, %r9d, %r9d
  addl $20, %r9d
  cmpl $27, %r9d
  je .Ljt_mem_reload_signed_default
  imull $3, %r9d, %r9d
  addl $21, %r9d
  cmpl $28, %r9d
  je .Ljt_mem_reload_signed_default
  imull $3, %r9d, %r9d
  addl $22, %r9d
  cmpl $29, %r9d
  je .Ljt_mem_reload_signed_default
  imull $3, %r9d, %r9d
  addl $23, %r9d
  cmpl $30, %r9d
  je .Ljt_mem_reload_signed_default
  imull $3, %r9d, %r9d
  addl $24, %r9d
  cmpb $0x81, 2(%rdi)
  ja .Ljt_mem_reload_signed_default
.Ljt_mem_reload_signed_idx:
  movsbq 2(%rdi), %rax
  leaq .Ljt_mem_reload_signed_table(%rip), %rcx
  movslq (%rcx,%rax,4), %rax
  addq %rcx, %rax
  jmpq *%rax
.Ljt_mem_reload_signed_c0:
  movl $100, %eax
  ret
.Ljt_mem_reload_signed_c1:
  movl $101, %eax
  ret
.Ljt_mem_reload_signed_c2:
  movl $102, %eax
  ret
.Ljt_mem_reload_signed_c3:
  movl $103, %eax
  ret
.Ljt_mem_reload_signed_c4:
  movl $104, %eax
  ret
.Ljt_mem_reload_signed_c5:
  movl $105, %eax
  ret
.Ljt_mem_reload_signed_c6:
  movl $106, %eax
  ret
.Ljt_mem_reload_signed_c7:
  movl $107, %eax
  ret
.Ljt_mem_reload_signed_c8:
  movl $108, %eax
  ret
.Ljt_mem_reload_signed_c9:
  movl $109, %eax
  ret
.Ljt_mem_reload_signed_c10:
  movl $110, %eax
  ret
.Ljt_mem_reload_signed_c11:
  movl $111, %eax
  ret
.Ljt_mem_reload_signed_c12:
  movl $112, %eax
  ret
.Ljt_mem_reload_signed_c13:
  movl $113, %eax
  ret
.Ljt_mem_reload_signed_c14:
  movl $114, %eax
  ret
.Ljt_mem_reload_signed_c15:
  movl $115, %eax
  ret
.Ljt_mem_reload_signed_c16:
  movl $116, %eax
  ret
.Ljt_mem_reload_signed_c17:
  movl $117, %eax
  ret
.Ljt_mem_reload_signed_c18:
  movl $118, %eax
  ret
.Ljt_mem_reload_signed_c19:
  movl $119, %eax
  ret
.Ljt_mem_reload_signed_c20:
  movl $120, %eax
  ret
.Ljt_mem_reload_signed_c21:
  movl $121, %eax
  ret
.Ljt_mem_reload_signed_c22:
  movl $122, %eax
  ret
.Ljt_mem_reload_signed_c23:
  movl $123, %eax
  ret
.Ljt_mem_reload_signed_c24:
  movl $124, %eax
  ret
.Ljt_mem_reload_signed_c25:
  movl $125, %eax
  ret
.Ljt_mem_reload_signed_c26:
  movl $126, %eax
  ret
.Ljt_mem_reload_signed_c27:
  movl $127, %eax
  ret
.Ljt_mem_reload_signed_c28:
  movl $128, %eax
  ret
.Ljt_mem_reload_signed_c29:
  movl $129, %eax
  ret
.Ljt_mem_reload_signed_c30:
  movl $130, %eax
  ret
.Ljt_mem_reload_signed_c31:
  movl $131, %eax
  ret
.Ljt_mem_reload_signed_c32:
  movl $132, %eax
  ret
.Ljt_mem_reload_signed_c33:
  movl $133, %eax
  ret
.Ljt_mem_reload_signed_c34:
  movl $134, %eax
  ret
.Ljt_mem_reload_signed_c35:
  movl $135, %eax
  ret
.Ljt_mem_reload_signed_c36:
  movl $136, %eax
  ret
.Ljt_mem_reload_signed_c37:
  movl $137, %eax
  ret
.Ljt_mem_reload_signed_c38:
  movl $138, %eax
  ret
.Ljt_mem_reload_signed_c39:
  movl $139, %eax
  ret
.Ljt_mem_reload_signed_c40:
  movl $140, %eax
  ret
.Ljt_mem_reload_signed_c41:
  movl $141, %eax
  ret
.Ljt_mem_reload_signed_c42:
  movl $142, %eax
  ret
.Ljt_mem_reload_signed_c43:
  movl $143, %eax
  ret
.Ljt_mem_reload_signed_c44:
  movl $144, %eax
  ret
.Ljt_mem_reload_signed_c45:
  movl $145, %eax
  ret
.Ljt_mem_reload_signed_c46:
  movl $146, %eax
  ret
.Ljt_mem_reload_signed_c47:
  movl $147, %eax
  ret
.Ljt_mem_reload_signed_c48:
  movl $148, %eax
  ret
.Ljt_mem_reload_signed_c49:
  movl $149, %eax
  ret
.Ljt_mem_reload_signed_c50:
  movl $150, %eax
  ret
.Ljt_mem_reload_signed_c51:
  movl $151, %eax
  ret
.Ljt_mem_reload_signed_c52:
  movl $152, %eax
  ret
.Ljt_mem_reload_signed_c53:
  movl $153, %eax
  ret
.Ljt_mem_reload_signed_c54:
  movl $154, %eax
  ret
.Ljt_mem_reload_signed_c55:
  movl $155, %eax
  ret
.Ljt_mem_reload_signed_c56:
  movl $156, %eax
  ret
.Ljt_mem_reload_signed_c57:
  movl $157, %eax
  ret
.Ljt_mem_reload_signed_c58:
  movl $158, %eax
  ret
.Ljt_mem_reload_signed_c59:
  movl $159, %eax
  ret
.Ljt_mem_reload_signed_c60:
  movl $160, %eax
  ret
.Ljt_mem_reload_signed_c61:
  movl $161, %eax
  ret
.Ljt_mem_reload_signed_c62:
  movl $162, %eax
  ret
.Ljt_mem_reload_signed_c63:
  movl $163, %eax
  ret
.Ljt_mem_reload_signed_c64:
  movl $164, %eax
  ret
.Ljt_mem_reload_signed_c65:
  movl $165, %eax
  ret
.Ljt_mem_reload_signed_c66:
  movl $166, %eax
  ret
.Ljt_mem_reload_signed_c67:
  movl $167, %eax
  ret
.Ljt_mem_reload_signed_c68:
  movl $168, %eax
  ret
.Ljt_mem_reload_signed_c69:
  movl $169, %eax
  ret
.Ljt_mem_reload_signed_c70:
  movl $170, %eax
  ret
.Ljt_mem_reload_signed_c71:
  movl $171, %eax
  ret
.Ljt_mem_reload_signed_c72:
  movl $172, %eax
  ret
.Ljt_mem_reload_signed_c73:
  movl $173, %eax
  ret
.Ljt_mem_reload_signed_c74:
  movl $174, %eax
  ret
.Ljt_mem_reload_signed_c75:
  movl $175, %eax
  ret
.Ljt_mem_reload_signed_c76:
  movl $176, %eax
  ret
.Ljt_mem_reload_signed_c77:
  movl $177, %eax
  ret
.Ljt_mem_reload_signed_c78:
  movl $178, %eax
  ret
.Ljt_mem_reload_signed_c79:
  movl $179, %eax
  ret
.Ljt_mem_reload_signed_c80:
  movl $180, %eax
  ret
.Ljt_mem_reload_signed_c81:
  movl $181, %eax
  ret
.Ljt_mem_reload_signed_c82:
  movl $182, %eax
  ret
.Ljt_mem_reload_signed_c83:
  movl $183, %eax
  ret
.Ljt_mem_reload_signed_c84:
  movl $184, %eax
  ret
.Ljt_mem_reload_signed_c85:
  movl $185, %eax
  ret
.Ljt_mem_reload_signed_c86:
  movl $186, %eax
  ret
.Ljt_mem_reload_signed_c87:
  movl $187, %eax
  ret
.Ljt_mem_reload_signed_c88:
  movl $188, %eax
  ret
.Ljt_mem_reload_signed_c89:
  movl $189, %eax
  ret
.Ljt_mem_reload_signed_c90:
  movl $190, %eax
  ret
.Ljt_mem_reload_signed_c91:
  movl $191, %eax
  ret
.Ljt_mem_reload_signed_c92:
  movl $192, %eax
  ret
.Ljt_mem_reload_signed_c93:
  movl $193, %eax
  ret
.Ljt_mem_reload_signed_c94:
  movl $194, %eax
  ret
.Ljt_mem_reload_signed_c95:
  movl $195, %eax
  ret
.Ljt_mem_reload_signed_c96:
  movl $196, %eax
  ret
.Ljt_mem_reload_signed_c97:
  movl $197, %eax
  ret
.Ljt_mem_reload_signed_c98:
  movl $198, %eax
  ret
.Ljt_mem_reload_signed_c99:
  movl $199, %eax
  ret
.Ljt_mem_reload_signed_c100:
  movl $200, %eax
  ret
.Ljt_mem_reload_signed_c101:
  movl $201, %eax
  ret
.Ljt_mem_reload_signed_c102:
  movl $202, %eax
  ret
.Ljt_mem_reload_signed_c103:
  movl $203, %eax
  ret
.Ljt_mem_reload_signed_c104:
  movl $204, %eax
  ret
.Ljt_mem_reload_signed_c105:
  movl $205, %eax
  ret
.Ljt_mem_reload_signed_c106:
  movl $206, %eax
  ret
.Ljt_mem_reload_signed_c107:
  movl $207, %eax
  ret
.Ljt_mem_reload_signed_c108:
  movl $208, %eax
  ret
.Ljt_mem_reload_signed_c109:
  movl $209, %eax
  ret
.Ljt_mem_reload_signed_c110:
  movl $210, %eax
  ret
.Ljt_mem_reload_signed_c111:
  movl $211, %eax
  ret
.Ljt_mem_reload_signed_c112:
  movl $212, %eax
  ret
.Ljt_mem_reload_signed_c113:
  movl $213, %eax
  ret
.Ljt_mem_reload_signed_c114:
  movl $214, %eax
  ret
.Ljt_mem_reload_signed_c115:
  movl $215, %eax
  ret
.Ljt_mem_reload_signed_c116:
  movl $216, %eax
  ret
.Ljt_mem_reload_signed_c117:
  movl $217, %eax
  ret
.Ljt_mem_reload_signed_c118:
  movl $218, %eax
  ret
.Ljt_mem_reload_signed_c119:
  movl $219, %eax
  ret
.Ljt_mem_reload_signed_c120:
  movl $220, %eax
  ret
.Ljt_mem_reload_signed_c121:
  movl $221, %eax
  ret
.Ljt_mem_reload_signed_c122:
  movl $222, %eax
  ret
.Ljt_mem_reload_signed_c123:
  movl $223, %eax
  ret
.Ljt_mem_reload_signed_c124:
  movl $224, %eax
  ret
.Ljt_mem_reload_signed_c125:
  movl $225, %eax
  ret
.Ljt_mem_reload_signed_c126:
  movl $226, %eax
  ret
.Ljt_mem_reload_signed_c127:
  movl $227, %eax
  ret
.Ljt_mem_reload_signed_c128:
  movl $228, %eax
  ret
.Ljt_mem_reload_signed_c129:
  movl $229, %eax
  ret
.Ljt_mem_reload_signed_default:
  movl $-1, %eax
  ret
  .size jt_mem_reload_signed, .-jt_mem_reload_signed
  .globl jt_mem_reload_call
  .type jt_mem_reload_call, @function
  .p2align 4
jt_mem_reload_call:
  movl %esi, %r9d
  cmpl $7, %r9d
  je .Ljt_mem_reload_call_default
  imull $3, %r9d, %r9d
  addl $1, %r9d
  cmpl $8, %r9d
  je .Ljt_mem_reload_call_default
  imull $3, %r9d, %r9d
  addl $2, %r9d
  cmpl $9, %r9d
  je .Ljt_mem_reload_call_default
  imull $3, %r9d, %r9d
  addl $3, %r9d
  cmpl $10, %r9d
  je .Ljt_mem_reload_call_default
  imull $3, %r9d, %r9d
  addl $4, %r9d
  cmpl $11, %r9d
  je .Ljt_mem_reload_call_default
  imull $3, %r9d, %r9d
  addl $5, %r9d
  cmpl $12, %r9d
  je .Ljt_mem_reload_call_default
  imull $3, %r9d, %r9d
  addl $6, %r9d
  cmpl $13, %r9d
  je .Ljt_mem_reload_call_default
  imull $3, %r9d, %r9d
  addl $7, %r9d
  cmpl $14, %r9d
  je .Ljt_mem_reload_call_default
  imull $3, %r9d, %r9d
  addl $8, %r9d
  cmpl $15, %r9d
  je .Ljt_mem_reload_call_default
  imull $3, %r9d, %r9d
  addl $9, %r9d
  cmpl $16, %r9d
  je .Ljt_mem_reload_call_default
  imull $3, %r9d, %r9d
  addl $10, %r9d
  cmpl $17, %r9d
  je .Ljt_mem_reload_call_default
  imull $3, %r9d, %r9d
  addl $11, %r9d
  cmpl $18, %r9d
  je .Ljt_mem_reload_call_default
  imull $3, %r9d, %r9d
  addl $12, %r9d
  cmpl $19, %r9d
  je .Ljt_mem_reload_call_default
  imull $3, %r9d, %r9d
  addl $13, %r9d
  cmpl $20, %r9d
  je .Ljt_mem_reload_call_default
  imull $3, %r9d, %r9d
  addl $14, %r9d
  cmpl $21, %r9d
  je .Ljt_mem_reload_call_default
  imull $3, %r9d, %r9d
  addl $15, %r9d
  cmpl $22, %r9d
  je .Ljt_mem_reload_call_default
  imull $3, %r9d, %r9d
  addl $16, %r9d
  cmpl $23, %r9d
  je .Ljt_mem_reload_call_default
  imull $3, %r9d, %r9d
  addl $17, %r9d
  cmpl $24, %r9d
  je .Ljt_mem_reload_call_default
  imull $3, %r9d, %r9d
  addl $18, %r9d
  cmpl $25, %r9d
  je .Ljt_mem_reload_call_default
  imull $3, %r9d, %r9d
  addl $19, %r9d
  cmpl $26, %r9d
  je .Ljt_mem_reload_call_default
  imull $3, %r9d, %r9d
  addl $20, %r9d
  cmpl $27, %r9d
  je .Ljt_mem_reload_call_default
  imull $3, %r9d, %r9d
  addl $21, %r9d
  cmpl $28, %r9d
  je .Ljt_mem_reload_call_default
  imull $3, %r9d, %r9d
  addl $22, %r9d
  cmpl $29, %r9d
  je .Ljt_mem_reload_call_default
  imull $3, %r9d, %r9d
  addl $23, %r9d
  cmpl $30, %r9d
  je .Ljt_mem_reload_call_default
  imull $3, %r9d, %r9d
  addl $24, %r9d
  cmpb $5, 2(%rdi)
  ja .Ljt_mem_reload_call_default
.Ljt_mem_reload_call_idx:
  movzbl 2(%rdi), %edx
  call mem_reload_helper
  leaq .Ljt_mem_reload_call_table(%rip), %rcx
  movslq (%rcx,%rdx,4), %rdx
  addq %rcx, %rdx
  jmpq *%rdx
.Ljt_mem_reload_call_c0:
  movl $100, %eax
  ret
.Ljt_mem_reload_call_c1:
  movl $101, %eax
  ret
.Ljt_mem_reload_call_c2:
  movl $102, %eax
  ret
.Ljt_mem_reload_call_c3:
  movl $103, %eax
  ret
.Ljt_mem_reload_call_c4:
  movl $104, %eax
  ret
.Ljt_mem_reload_call_c5:
  movl $105, %eax
  ret
.Ljt_mem_reload_call_default:
  movl $-1, %eax
  ret
  .size jt_mem_reload_call, .-jt_mem_reload_call
  .globl jt_mem_reload_partial
  .type jt_mem_reload_partial, @function
  .p2align 4
jt_mem_reload_partial:
  movl %esi, %r9d
  cmpl $7, %r9d
  je .Ljt_mem_reload_partial_default
  imull $3, %r9d, %r9d
  addl $1, %r9d
  cmpl $8, %r9d
  je .Ljt_mem_reload_partial_default
  imull $3, %r9d, %r9d
  addl $2, %r9d
  cmpl $9, %r9d
  je .Ljt_mem_reload_partial_default
  imull $3, %r9d, %r9d
  addl $3, %r9d
  cmpl $10, %r9d
  je .Ljt_mem_reload_partial_default
  imull $3, %r9d, %r9d
  addl $4, %r9d
  cmpl $11, %r9d
  je .Ljt_mem_reload_partial_default
  imull $3, %r9d, %r9d
  addl $5, %r9d
  cmpl $12, %r9d
  je .Ljt_mem_reload_partial_default
  imull $3, %r9d, %r9d
  addl $6, %r9d
  cmpl $13, %r9d
  je .Ljt_mem_reload_partial_default
  imull $3, %r9d, %r9d
  addl $7, %r9d
  cmpl $14, %r9d
  je .Ljt_mem_reload_partial_default
  imull $3, %r9d, %r9d
  addl $8, %r9d
  cmpl $15, %r9d
  je .Ljt_mem_reload_partial_default
  imull $3, %r9d, %r9d
  addl $9, %r9d
  cmpl $16, %r9d
  je .Ljt_mem_reload_partial_default
  imull $3, %r9d, %r9d
  addl $10, %r9d
  cmpl $17, %r9d
  je .Ljt_mem_reload_partial_default
  imull $3, %r9d, %r9d
  addl $11, %r9d
  cmpl $18, %r9d
  je .Ljt_mem_reload_partial_default
  imull $3, %r9d, %r9d
  addl $12, %r9d
  cmpl $19, %r9d
  je .Ljt_mem_reload_partial_default
  imull $3, %r9d, %r9d
  addl $13, %r9d
  cmpl $20, %r9d
  je .Ljt_mem_reload_partial_default
  imull $3, %r9d, %r9d
  addl $14, %r9d
  cmpl $21, %r9d
  je .Ljt_mem_reload_partial_default
  imull $3, %r9d, %r9d
  addl $15, %r9d
  cmpl $22, %r9d
  je .Ljt_mem_reload_partial_default
  imull $3, %r9d, %r9d
  addl $16, %r9d
  cmpl $23, %r9d
  je .Ljt_mem_reload_partial_default
  imull $3, %r9d, %r9d
  addl $17, %r9d
  cmpl $24, %r9d
  je .Ljt_mem_reload_partial_default
  imull $3, %r9d, %r9d
  addl $18, %r9d
  cmpl $25, %r9d
  je .Ljt_mem_reload_partial_default
  imull $3, %r9d, %r9d
  addl $19, %r9d
  cmpl $26, %r9d
  je .Ljt_mem_reload_partial_default
  imull $3, %r9d, %r9d
  addl $20, %r9d
  cmpl $27, %r9d
  je .Ljt_mem_reload_partial_default
  imull $3, %r9d, %r9d
  addl $21, %r9d
  cmpl $28, %r9d
  je .Ljt_mem_reload_partial_default
  imull $3, %r9d, %r9d
  addl $22, %r9d
  cmpl $29, %r9d
  je .Ljt_mem_reload_partial_default
  imull $3, %r9d, %r9d
  addl $23, %r9d
  cmpl $30, %r9d
  je .Ljt_mem_reload_partial_default
  imull $3, %r9d, %r9d
  addl $24, %r9d
  cmpb $5, 2(%rdi)
  ja .Ljt_mem_reload_partial_default
.Ljt_mem_reload_partial_idx:
  movzbl 2(%rdi), %eax
  orb $0x80, %al
  leaq .Ljt_mem_reload_partial_table(%rip), %rcx
  movslq (%rcx,%rax,4), %rax
  addq %rcx, %rax
  jmpq *%rax
.Ljt_mem_reload_partial_c0:
  movl $100, %eax
  ret
.Ljt_mem_reload_partial_c1:
  movl $101, %eax
  ret
.Ljt_mem_reload_partial_c2:
  movl $102, %eax
  ret
.Ljt_mem_reload_partial_c3:
  movl $103, %eax
  ret
.Ljt_mem_reload_partial_c4:
  movl $104, %eax
  ret
.Ljt_mem_reload_partial_c5:
  movl $105, %eax
  ret
.Ljt_mem_reload_partial_default:
  movl $-1, %eax
  ret
  .size jt_mem_reload_partial, .-jt_mem_reload_partial
  .globl jt_mem_reload_second_pred
  .type jt_mem_reload_second_pred, @function
  .p2align 4
jt_mem_reload_second_pred:
  movl %esi, %r9d
  cmpl $7, %r9d
  je .Ljt_mem_reload_second_pred_default
  imull $3, %r9d, %r9d
  addl $1, %r9d
  cmpl $8, %r9d
  je .Ljt_mem_reload_second_pred_default
  imull $3, %r9d, %r9d
  addl $2, %r9d
  cmpl $9, %r9d
  je .Ljt_mem_reload_second_pred_default
  imull $3, %r9d, %r9d
  addl $3, %r9d
  cmpl $10, %r9d
  je .Ljt_mem_reload_second_pred_default
  imull $3, %r9d, %r9d
  addl $4, %r9d
  cmpl $11, %r9d
  je .Ljt_mem_reload_second_pred_default
  imull $3, %r9d, %r9d
  addl $5, %r9d
  cmpl $12, %r9d
  je .Ljt_mem_reload_second_pred_default
  imull $3, %r9d, %r9d
  addl $6, %r9d
  cmpl $13, %r9d
  je .Ljt_mem_reload_second_pred_default
  imull $3, %r9d, %r9d
  addl $7, %r9d
  cmpl $14, %r9d
  je .Ljt_mem_reload_second_pred_default
  imull $3, %r9d, %r9d
  addl $8, %r9d
  cmpl $15, %r9d
  je .Ljt_mem_reload_second_pred_default
  imull $3, %r9d, %r9d
  addl $9, %r9d
  cmpl $16, %r9d
  je .Ljt_mem_reload_second_pred_default
  imull $3, %r9d, %r9d
  addl $10, %r9d
  cmpl $17, %r9d
  je .Ljt_mem_reload_second_pred_default
  imull $3, %r9d, %r9d
  addl $11, %r9d
  cmpl $18, %r9d
  je .Ljt_mem_reload_second_pred_default
  imull $3, %r9d, %r9d
  addl $12, %r9d
  cmpl $19, %r9d
  je .Ljt_mem_reload_second_pred_default
  imull $3, %r9d, %r9d
  addl $13, %r9d
  cmpl $20, %r9d
  je .Ljt_mem_reload_second_pred_default
  imull $3, %r9d, %r9d
  addl $14, %r9d
  cmpl $21, %r9d
  je .Ljt_mem_reload_second_pred_default
  imull $3, %r9d, %r9d
  addl $15, %r9d
  cmpl $22, %r9d
  je .Ljt_mem_reload_second_pred_default
  imull $3, %r9d, %r9d
  addl $16, %r9d
  cmpl $23, %r9d
  je .Ljt_mem_reload_second_pred_default
  imull $3, %r9d, %r9d
  addl $17, %r9d
  cmpl $24, %r9d
  je .Ljt_mem_reload_second_pred_default
  imull $3, %r9d, %r9d
  addl $18, %r9d
  cmpl $25, %r9d
  je .Ljt_mem_reload_second_pred_default
  imull $3, %r9d, %r9d
  addl $19, %r9d
  cmpl $26, %r9d
  je .Ljt_mem_reload_second_pred_default
  imull $3, %r9d, %r9d
  addl $20, %r9d
  cmpl $27, %r9d
  je .Ljt_mem_reload_second_pred_default
  imull $3, %r9d, %r9d
  addl $21, %r9d
  cmpl $28, %r9d
  je .Ljt_mem_reload_second_pred_default
  imull $3, %r9d, %r9d
  addl $22, %r9d
  cmpl $29, %r9d
  je .Ljt_mem_reload_second_pred_default
  imull $3, %r9d, %r9d
  addl $23, %r9d
  cmpl $30, %r9d
  je .Ljt_mem_reload_second_pred_default
  imull $3, %r9d, %r9d
  addl $24, %r9d
  testl %esi, %esi
  jne .Ljt_mem_reload_second_pred_idx
  cmpb $5, 2(%rdi)
  ja .Ljt_mem_reload_second_pred_default
.Ljt_mem_reload_second_pred_idx:
  movzbl 2(%rdi), %eax
  leaq .Ljt_mem_reload_second_pred_table(%rip), %rcx
  movslq (%rcx,%rax,4), %rax
  addq %rcx, %rax
  jmpq *%rax
.Ljt_mem_reload_second_pred_c0:
  movl $100, %eax
  ret
.Ljt_mem_reload_second_pred_c1:
  movl $101, %eax
  ret
.Ljt_mem_reload_second_pred_c2:
  movl $102, %eax
  ret
.Ljt_mem_reload_second_pred_c3:
  movl $103, %eax
  ret
.Ljt_mem_reload_second_pred_c4:
  movl $104, %eax
  ret
.Ljt_mem_reload_second_pred_c5:
  movl $105, %eax
  ret
.Ljt_mem_reload_second_pred_default:
  movl $-1, %eax
  ret
  .size jt_mem_reload_second_pred, .-jt_mem_reload_second_pred
  .globl jt_mem_reload_rooted
  .type jt_mem_reload_rooted, @function
  .p2align 4
jt_mem_reload_rooted:
  movl %esi, %r9d
  cmpl $7, %r9d
  je .Ljt_mem_reload_rooted_default
  imull $3, %r9d, %r9d
  addl $1, %r9d
  cmpl $8, %r9d
  je .Ljt_mem_reload_rooted_default
  imull $3, %r9d, %r9d
  addl $2, %r9d
  cmpl $9, %r9d
  je .Ljt_mem_reload_rooted_default
  imull $3, %r9d, %r9d
  addl $3, %r9d
  cmpl $10, %r9d
  je .Ljt_mem_reload_rooted_default
  imull $3, %r9d, %r9d
  addl $4, %r9d
  cmpl $11, %r9d
  je .Ljt_mem_reload_rooted_default
  imull $3, %r9d, %r9d
  addl $5, %r9d
  cmpl $12, %r9d
  je .Ljt_mem_reload_rooted_default
  imull $3, %r9d, %r9d
  addl $6, %r9d
  cmpl $13, %r9d
  je .Ljt_mem_reload_rooted_default
  imull $3, %r9d, %r9d
  addl $7, %r9d
  cmpl $14, %r9d
  je .Ljt_mem_reload_rooted_default
  imull $3, %r9d, %r9d
  addl $8, %r9d
  cmpl $15, %r9d
  je .Ljt_mem_reload_rooted_default
  imull $3, %r9d, %r9d
  addl $9, %r9d
  cmpl $16, %r9d
  je .Ljt_mem_reload_rooted_default
  imull $3, %r9d, %r9d
  addl $10, %r9d
  cmpl $17, %r9d
  je .Ljt_mem_reload_rooted_default
  imull $3, %r9d, %r9d
  addl $11, %r9d
  cmpl $18, %r9d
  je .Ljt_mem_reload_rooted_default
  imull $3, %r9d, %r9d
  addl $12, %r9d
  cmpl $19, %r9d
  je .Ljt_mem_reload_rooted_default
  imull $3, %r9d, %r9d
  addl $13, %r9d
  cmpl $20, %r9d
  je .Ljt_mem_reload_rooted_default
  imull $3, %r9d, %r9d
  addl $14, %r9d
  cmpl $21, %r9d
  je .Ljt_mem_reload_rooted_default
  imull $3, %r9d, %r9d
  addl $15, %r9d
  cmpl $22, %r9d
  je .Ljt_mem_reload_rooted_default
  imull $3, %r9d, %r9d
  addl $16, %r9d
  cmpl $23, %r9d
  je .Ljt_mem_reload_rooted_default
  imull $3, %r9d, %r9d
  addl $17, %r9d
  cmpl $24, %r9d
  je .Ljt_mem_reload_rooted_default
  imull $3, %r9d, %r9d
  addl $18, %r9d
  cmpl $25, %r9d
  je .Ljt_mem_reload_rooted_default
  imull $3, %r9d, %r9d
  addl $19, %r9d
  cmpl $26, %r9d
  je .Ljt_mem_reload_rooted_default
  imull $3, %r9d, %r9d
  addl $20, %r9d
  cmpl $27, %r9d
  je .Ljt_mem_reload_rooted_default
  imull $3, %r9d, %r9d
  addl $21, %r9d
  cmpl $28, %r9d
  je .Ljt_mem_reload_rooted_default
  imull $3, %r9d, %r9d
  addl $22, %r9d
  cmpl $29, %r9d
  je .Ljt_mem_reload_rooted_default
  imull $3, %r9d, %r9d
  addl $23, %r9d
  cmpl $30, %r9d
  je .Ljt_mem_reload_rooted_default
  imull $3, %r9d, %r9d
  addl $24, %r9d
  cmpb $5, 2(%rdi)
  ja .Ljt_mem_reload_rooted_default
.Ljt_mem_reload_rooted_idx:
  movzbl 2(%rdi), %eax
  leaq .Ljt_mem_reload_rooted_table(%rip), %rcx
  movslq (%rcx,%rax,4), %rax
  addq %rcx, %rax
  jmpq *%rax
.Ljt_mem_reload_rooted_c0:
  movl $100, %eax
  ret
.Ljt_mem_reload_rooted_c1:
  movl $101, %eax
  ret
.Ljt_mem_reload_rooted_c2:
  movl $102, %eax
  ret
.Ljt_mem_reload_rooted_c3:
  movl $103, %eax
  ret
.Ljt_mem_reload_rooted_c4:
  movl $104, %eax
  ret
.Ljt_mem_reload_rooted_c5:
  movl $105, %eax
  ret
.Ljt_mem_reload_rooted_default:
  movl $-1, %eax
  ret
  .size jt_mem_reload_rooted, .-jt_mem_reload_rooted
  .globl mem_reload_helper
  .type mem_reload_helper, @function
mem_reload_helper:
  xorl %edx, %edx
  ret
  .size mem_reload_helper, .-mem_reload_helper

  .section .rodata
  .p2align 2
.Ljt_mem_reload_table:
  .long .Ljt_mem_reload_c0-.Ljt_mem_reload_table
  .long .Ljt_mem_reload_c1-.Ljt_mem_reload_table
  .long .Ljt_mem_reload_c2-.Ljt_mem_reload_table
  .long .Ljt_mem_reload_c3-.Ljt_mem_reload_table
  .long .Ljt_mem_reload_c4-.Ljt_mem_reload_table
  .long .Ljt_mem_reload_c5-.Ljt_mem_reload_table
  .long 0
.Ljt_mem_reload_store_table:
  .long .Ljt_mem_reload_store_c0-.Ljt_mem_reload_store_table
  .long .Ljt_mem_reload_store_c1-.Ljt_mem_reload_store_table
  .long .Ljt_mem_reload_store_c2-.Ljt_mem_reload_store_table
  .long .Ljt_mem_reload_store_c3-.Ljt_mem_reload_store_table
  .long .Ljt_mem_reload_store_c4-.Ljt_mem_reload_store_table
  .long .Ljt_mem_reload_store_c5-.Ljt_mem_reload_store_table
  .long 0
.Ljt_mem_reload_carrier_table:
  .long .Ljt_mem_reload_carrier_c0-.Ljt_mem_reload_carrier_table
  .long .Ljt_mem_reload_carrier_c1-.Ljt_mem_reload_carrier_table
  .long .Ljt_mem_reload_carrier_c2-.Ljt_mem_reload_carrier_table
  .long .Ljt_mem_reload_carrier_c3-.Ljt_mem_reload_carrier_table
  .long .Ljt_mem_reload_carrier_c4-.Ljt_mem_reload_carrier_table
  .long .Ljt_mem_reload_carrier_c5-.Ljt_mem_reload_carrier_table
  .long 0
.Ljt_mem_reload_disp_table:
  .long .Ljt_mem_reload_disp_c0-.Ljt_mem_reload_disp_table
  .long .Ljt_mem_reload_disp_c1-.Ljt_mem_reload_disp_table
  .long .Ljt_mem_reload_disp_c2-.Ljt_mem_reload_disp_table
  .long .Ljt_mem_reload_disp_c3-.Ljt_mem_reload_disp_table
  .long .Ljt_mem_reload_disp_c4-.Ljt_mem_reload_disp_table
  .long .Ljt_mem_reload_disp_c5-.Ljt_mem_reload_disp_table
  .long 0
.Ljt_mem_reload_wide_table:
  .long .Ljt_mem_reload_wide_c0-.Ljt_mem_reload_wide_table
  .long .Ljt_mem_reload_wide_c1-.Ljt_mem_reload_wide_table
  .long .Ljt_mem_reload_wide_c2-.Ljt_mem_reload_wide_table
  .long .Ljt_mem_reload_wide_c3-.Ljt_mem_reload_wide_table
  .long .Ljt_mem_reload_wide_c4-.Ljt_mem_reload_wide_table
  .long .Ljt_mem_reload_wide_c5-.Ljt_mem_reload_wide_table
  .long 0
.Ljt_mem_reload_signed_table:
  .long .Ljt_mem_reload_signed_c0-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c1-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c2-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c3-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c4-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c5-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c6-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c7-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c8-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c9-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c10-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c11-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c12-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c13-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c14-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c15-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c16-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c17-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c18-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c19-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c20-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c21-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c22-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c23-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c24-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c25-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c26-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c27-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c28-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c29-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c30-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c31-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c32-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c33-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c34-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c35-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c36-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c37-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c38-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c39-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c40-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c41-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c42-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c43-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c44-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c45-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c46-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c47-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c48-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c49-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c50-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c51-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c52-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c53-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c54-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c55-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c56-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c57-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c58-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c59-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c60-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c61-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c62-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c63-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c64-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c65-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c66-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c67-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c68-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c69-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c70-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c71-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c72-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c73-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c74-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c75-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c76-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c77-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c78-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c79-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c80-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c81-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c82-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c83-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c84-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c85-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c86-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c87-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c88-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c89-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c90-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c91-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c92-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c93-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c94-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c95-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c96-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c97-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c98-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c99-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c100-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c101-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c102-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c103-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c104-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c105-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c106-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c107-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c108-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c109-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c110-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c111-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c112-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c113-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c114-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c115-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c116-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c117-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c118-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c119-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c120-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c121-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c122-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c123-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c124-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c125-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c126-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c127-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c128-.Ljt_mem_reload_signed_table
  .long .Ljt_mem_reload_signed_c129-.Ljt_mem_reload_signed_table
  .long 0
.Ljt_mem_reload_call_table:
  .long .Ljt_mem_reload_call_c0-.Ljt_mem_reload_call_table
  .long .Ljt_mem_reload_call_c1-.Ljt_mem_reload_call_table
  .long .Ljt_mem_reload_call_c2-.Ljt_mem_reload_call_table
  .long .Ljt_mem_reload_call_c3-.Ljt_mem_reload_call_table
  .long .Ljt_mem_reload_call_c4-.Ljt_mem_reload_call_table
  .long .Ljt_mem_reload_call_c5-.Ljt_mem_reload_call_table
  .long 0
.Ljt_mem_reload_partial_table:
  .long .Ljt_mem_reload_partial_c0-.Ljt_mem_reload_partial_table
  .long .Ljt_mem_reload_partial_c1-.Ljt_mem_reload_partial_table
  .long .Ljt_mem_reload_partial_c2-.Ljt_mem_reload_partial_table
  .long .Ljt_mem_reload_partial_c3-.Ljt_mem_reload_partial_table
  .long .Ljt_mem_reload_partial_c4-.Ljt_mem_reload_partial_table
  .long .Ljt_mem_reload_partial_c5-.Ljt_mem_reload_partial_table
  .long 0
.Ljt_mem_reload_second_pred_table:
  .long .Ljt_mem_reload_second_pred_c0-.Ljt_mem_reload_second_pred_table
  .long .Ljt_mem_reload_second_pred_c1-.Ljt_mem_reload_second_pred_table
  .long .Ljt_mem_reload_second_pred_c2-.Ljt_mem_reload_second_pred_table
  .long .Ljt_mem_reload_second_pred_c3-.Ljt_mem_reload_second_pred_table
  .long .Ljt_mem_reload_second_pred_c4-.Ljt_mem_reload_second_pred_table
  .long .Ljt_mem_reload_second_pred_c5-.Ljt_mem_reload_second_pred_table
  .long 0
.Ljt_mem_reload_rooted_table:
  .long .Ljt_mem_reload_rooted_c0-.Ljt_mem_reload_rooted_table
  .long .Ljt_mem_reload_rooted_c1-.Ljt_mem_reload_rooted_table
  .long .Ljt_mem_reload_rooted_c2-.Ljt_mem_reload_rooted_table
  .long .Ljt_mem_reload_rooted_c3-.Ljt_mem_reload_rooted_table
  .long .Ljt_mem_reload_rooted_c4-.Ljt_mem_reload_rooted_table
  .long .Ljt_mem_reload_rooted_c5-.Ljt_mem_reload_rooted_table
  .long 0

  .data
  .p2align 3
  .globl mem_reload_rooted_entry
mem_reload_rooted_entry:
  .quad .Ljt_mem_reload_rooted_idx
  .section .note.GNU-stack,"",@progbits
