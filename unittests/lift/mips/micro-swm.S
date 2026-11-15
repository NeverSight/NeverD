/* Execute every microMIPS SWM16 list length and offset. The classic driver
 * checks all 24 surrounding words, including the inactive bytes. Linux o32. */
.set noreorder
.set noat
.set nomicromips
.text
.globl _start
_start:
  lui   $sp,%hi(stack_words)
  addiu $sp,$sp,%lo(stack_words)
  li    $16,0x80401234
  li    $17,0x81411235
  li    $18,0x82421236
  li    $19,0x83431237
  li    $ra,0xdeadface

.macro check_store list, off
  move $8,$sp
  li $9,24
1:
  sw $zero,0($8)
  addiu $8,$8,4
  addiu $9,$9,-1
  bne $9,$zero,1b
  nop

  lui $25,%hi(2f)
  addiu $25,$25,%lo(2f)
  ori $25,$25,1
  jalr $7,$25
  nop
  b 3f
  nop
2:
#if __mips_isa_rev >= 6
  .2byte (0x440a | (\list << 8) | (\off << 4)), 0x44e3 /* SWM16; JRC16 a3 */
#else
  .2byte (0x4540 | (\list << 4) | \off), 0x45a7 /* SWM16; JRC a3 */
#endif
3:
  .set word_index,0
  .rept 24
    lw $8,(word_index * 4)($sp)
    .if (word_index < \off) || (word_index > (\off + \list + 1))
      move $9,$zero
    .elseif word_index == (\off + \list + 1)
      move $9,$ra
    .else
      li $9,(0x80401234 + (word_index - \off) * 0x01010001)
    .endif
    bne $8,$9,failed
    nop
    .set word_index,(word_index + 1)
  .endr
.endm

.irp list,0,1,2,3
.irp off,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
check_store \list,\off
.endr
.endr
  li $4,0
  b finish
  nop
failed:
  li $4,1
finish:
  li $2,4001
  syscall

.bss
.balign 16
stack_words:
  .space 96
