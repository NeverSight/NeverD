/* $zero selects the physical two-input form in GNU as and Clang. */
.macro doubleword_division name,op,result
.globl \name
.type \name,@function
\name:
  \op $0,$4,$5
  jr $31
  \result $2
.size \name,.-\name
.endm
doubleword_division mips_ddiv_lo,ddiv,mflo
doubleword_division mips_ddiv_hi,ddiv,mfhi
doubleword_division mips_ddivu_lo,ddivu,mflo
doubleword_division mips_ddivu_hi,ddivu,mfhi
