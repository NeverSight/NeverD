// A freestanding o32 C environment supplies memcpy, including compiler-emitted
// aggregate copies. Execute every byte in the guest; no emulator interception.
// Exact source/destination equality is supported as required by GCC.
.set noreorder
.set nomips16
.set nomicromips
.text
.align 2
.globl memcpy
.type memcpy,@function
.ent memcpy
memcpy:
  move $2,$4
  beq $6,$0,2f
  nop
1:
  lbu $8,0($5)
  sb $8,0($4)
  addiu $5,$5,1
  addiu $4,$4,1
  addiu $6,$6,-1
  bne $6,$0,1b
  nop
2:
  jr $31
  nop
.end memcpy
.size memcpy,.-memcpy
