/* $zero selects the physical two-input form in GNU as and Clang. */
.macro division name,op,result
.globl \name
.type \name,@function
\name:
  \op $0,$4,$5
  jr $31
  \result $2
.size \name,.-\name
.endm
division mips_div_lo,div,mflo
division mips_div_hi,div,mfhi
division mips_divu_lo,divu,mflo
division mips_divu_hi,divu,mfhi
#ifdef WIDE
division mips_ddiv_lo,ddiv,mflo
division mips_ddiv_hi,ddiv,mfhi
division mips_ddivu_lo,ddivu,mflo
division mips_ddivu_hi,ddivu,mfhi
#endif
