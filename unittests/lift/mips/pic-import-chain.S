/* Recover both imports when the caller recomputes GP after the first call. */
.abicalls
.option pic2
.set noreorder
.text
.globl import_probe
.type dispatch,@function
.type transform,@function
.ent import_probe
import_probe:
  daddiu $sp,$sp,-48
  sd $16,8($sp)
#if defined(REPLACE_ARGUMENT) || defined(GUARD_ZERO) || defined(GUARD_MATCH)
  sd $ra,24($sp)
#else
  sd $17,24($sp)
  or $17,$ra,$zero
#endif
#ifdef GUARD_MATCH
  sd $18,32($sp)
  or $18,$5,$zero
#endif
  or $16,$25,$zero
  .cpsetup $25,16,import_probe
#ifdef SPILL_GP
  sd $gp,32($sp)
  or $6,$gp,$zero
#endif
#if __SIZEOF_POINTER__ == 8
  ld $25,%call16(dispatch)($gp)
#else
  lw $25,%call16(dispatch)($gp)
#endif
  jalr $25
  daddiu $5,$zero,37
#ifdef GUARD_ZERO
  bne $2,$zero,.Lresult
  or $4,$2,$zero
#elif defined(GUARD_MATCH)
  beq $2,$18,.Lforward
  nop
  b .Lresult
  nop
.Lforward:
  or $4,$2,$zero
#else
  or $4,$2,$zero
#endif
#ifdef SPILL_GP
  ld $gp,32($sp)
#else
  lui $12,%hi(%neg(%gp_rel(import_probe)))
  addiu $12,$12,%lo(%neg(%gp_rel(import_probe)))
#if __SIZEOF_POINTER__ == 8
  daddu $gp,$12,$16
#else
  addu $gp,$12,$16
#endif
#endif
#if __SIZEOF_POINTER__ == 8
  ld $25,%call16(transform)($gp)
#else
  lw $25,%call16(transform)($gp)
#endif
  daddiu $5,$zero,11
  jalr $25
#ifdef REPLACE_ARGUMENT
  daddiu $4,$zero,19
#else
  nop
#endif
.Lresult:
  daddiu $2,$2,1
  .cpreturn
  ld $16,8($sp)
#ifdef GUARD_MATCH
  ld $18,32($sp)
#endif
#if defined(REPLACE_ARGUMENT) || defined(GUARD_ZERO) || defined(GUARD_MATCH)
  ld $ra,24($sp)
#else
  or $ra,$17,$zero
  ld $17,24($sp)
#endif
  jr $ra
  daddiu $sp,$sp,48
.end import_probe
