.syntax unified
.thumb

.section .text$A,"xr"
.globl pe_tiny
.def pe_tiny; .scl 2; .type 32; .endef
.thumb_func
pe_tiny:
  bx lr

.section .text$B,"xr"
.globl pe_after_tiny
.def pe_after_tiny; .scl 2; .type 32; .endef
.thumb_func
pe_after_tiny:
  movs r0, #7
  bx lr
