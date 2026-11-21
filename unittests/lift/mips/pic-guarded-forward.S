/* Forward only the word checked on the path to the next import. */
.abicalls
.option pic2
.set noreorder
.text
.globl import_probe
.type dispatch,@function
.type transform,@function
#if defined(RECHECK_FIRST) || defined(RECHECK_SECOND)
.type finish,@function
#endif
.ent import_probe
import_probe:
  daddiu $sp,$sp,-64
  sd $16,0($sp)
  sd $17,8($sp)
  sd $18,16($sp)
  sd $19,24($sp)
  sd $20,32($sp)
  sd $ra,56($sp)
  or $16,$25,$zero
  or $17,$6,$zero
  or $18,$5,$zero
#ifdef REPEAT_PRODUCER
  or $20,$4,$zero
#else
  or $20,$7,$zero
#endif
  .cpsetup $25,48,import_probe
.macro load_target name
#if __SIZEOF_POINTER__ == 8
  ld $25,%call16(\name)($gp)
#else
  lw $25,%call16(\name)($gp)
#endif
.endm
.macro restore_gp
  lui $12,%hi(%neg(%gp_rel(import_probe)))
  addiu $12,$12,%lo(%neg(%gp_rel(import_probe)))
#if __SIZEOF_POINTER__ == 8
  daddu $gp,$12,$16
#else
  addu $gp,$12,$16
#endif
.endm
#ifdef SELF_FORWARD
  or $4,$18,$zero
.Lself:
  restore_gp
  load_target dispatch
  or $6,$zero,$zero
  or $7,$zero,$zero
  jalr $25
  or $5,$20,$zero
  beq $17,$zero,.Ldone
  daddiu $17,$17,-1
  bne $2,$18,.Ldone
  or $4,$2,$zero
  b .Lself
  nop
#elif defined(REPEAT_PRODUCER) || defined(RESULT_SPILL)
  load_target dispatch
  jalr $25
  daddiu $5,$zero,37
  or $19,$2,$zero
#ifdef RESULT_SPILL
  sd $2,40($sp)
#endif
  beq $17,$zero,.Lcheck_result
  nop
.Lrepeat:
#ifdef REPEAT_PRODUCER
#ifdef RESULT_SPILL
  /* The previous unchecked word must not reach the next open call. */
  sd $zero,40($sp)
#endif
  daddiu $20,$20,1
  restore_gp
  load_target dispatch
  or $4,$20,$zero
  or $6,$zero,$zero
  or $7,$zero,$zero
  jalr $25
  daddiu $5,$zero,37
  or $19,$2,$zero
#ifdef RESULT_SPILL
  sd $2,40($sp)
#endif
#endif
  daddiu $17,$17,-1
  bne $17,$zero,.Lrepeat
  nop
.Lcheck_result:
#ifdef RESULT_SPILL
#ifdef WRITE_SPILL_BYTE
  sb $zero,44($sp)
#endif
  ld $19,40($sp)
  bne $19,$18,.Ldone
  ld $4,40($sp)
#else
  bne $19,$18,.Ldone
  or $4,$2,$zero
#endif
  or $6,$zero,$zero
  or $7,$zero,$zero
#elif defined(JOIN_BEFORE_CHECK)
  beq $17,$zero,.Lother_call
  nop
  load_target dispatch
  jalr $25
  daddiu $5,$zero,37
  b .Lcheck
  or $19,$2,$zero
.Lother_call:
  load_target dispatch
  jalr $25
  daddiu $5,$zero,41
  or $19,$2,$zero
.Lcheck:
  bne $19,$18,.Ldone
  or $4,$2,$zero
  /* Both branches clobbered these incoming arguments. */
  or $6,$zero,$zero
  or $7,$zero,$zero
#else
  load_target dispatch
  jalr $25
  daddiu $5,$zero,37

  beq $17,$zero,.Lright
  or $19,$2,$zero
  bne $19,$18,.Ldone
  or $4,$19,$zero
  b .Ljoin
  nop
.Lright:
  beq $18,$2,.Ljoin
  or $4,$2,$zero
  b .Ldone
  nop
.Ljoin:
#endif
#ifndef SELF_FORWARD
  restore_gp
  load_target transform
  jalr $25
  daddiu $5,$zero,11
#if defined(RECHECK_FIRST) || defined(RECHECK_SECOND)
#ifdef RECHECK_SECOND
  or $19,$2,$zero
#endif
  bne $19,$20,.Ldone
  nop
  restore_gp
  load_target finish
  daddiu $5,$zero,7
  jalr $25
  or $4,$19,$zero
#endif
#endif
.Ldone:
  daddiu $2,$2,1
  .cpreturn
  ld $16,0($sp)
  ld $17,8($sp)
  ld $18,16($sp)
  ld $19,24($sp)
  ld $20,32($sp)
  ld $ra,56($sp)
  jr $ra
  daddiu $sp,$sp,64
.end import_probe
