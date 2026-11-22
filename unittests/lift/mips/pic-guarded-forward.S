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
#ifdef TRANSFER_SPILL
#define FRAME_BYTES 80
#else
#define FRAME_BYTES 64
#endif
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define LOW_AT(base) base
#define HIGH_AT(base) base+4
#else
#define LOW_AT(base) base+4
#define HIGH_AT(base) base
#endif
#ifndef COMBINE_WORDS
#define COMBINE_WORDS or
#endif
.ent import_probe
import_probe:
  daddiu $sp,$sp,-FRAME_BYTES
  sd $16,0($sp)
  sd $17,8($sp)
  sd $18,16($sp)
  sd $19,24($sp)
  sd $20,32($sp)
  sd $ra,56($sp)
  or $16,$25,$zero
  or $17,$6,$zero
  or $18,$5,$zero
#if defined(CROSS_JOIN) || defined(CROSS_LOOP)
  or $20,$6,$zero
#elif defined(REPEAT_PRODUCER)
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
.macro spill_result
#ifdef SEPARATE_HALVES
  dsrl32 $14,$2,0
  sw $14,64($sp)
  sw $2,40($sp)
#else
  sd $2,40($sp)
#endif
#if defined(SWAPPED_SAMPLE)
  lw $14,LOW_AT(40)($sp)
#elif defined(LOAD_HALVES)
  lw $14,HIGH_AT(40)($sp)
#elif defined(STORE_HALVES)
  ld $13,40($sp)
  dsrl32 $14,$13,0
  sw $14,HIGH_AT(64)($sp)
  sw $13,LOW_AT(64)($sp)
#elif defined(TRANSFER_SPILL) && !defined(SEPARATE_HALVES)
  ld $13,40($sp)
  sd $13,64($sp)
#endif
.endm
.macro check_spill
#ifdef GUARD_BEFORE_PACK
  bne $2,$18,.Ldone
  nop
#endif
#ifdef WRITE_SPILL_BYTE
  sb $zero,44($sp)
#endif
#if defined(SEPARATE_HALVES)
  lw $14,64($sp)
  lwu $13,40($sp)
  sd $zero,64($sp)
#elif defined(SWAPPED_SAMPLE)
  lwu $13,HIGH_AT(40)($sp)
#elif defined(LOAD_HALVES)
  lwu $13,LOW_AT(40)($sp)
#endif
#if defined(INSERT_FIELD)
  dins $13,$14,32,32
  or $19,$13,$zero
#elif defined(LOAD_HALVES) || defined(SEPARATE_HALVES)
  dsll32 $14,$14,0
  COMBINE_WORDS $19,$13,$14
#elif defined(TRANSFER_SPILL)
  ld $19,64($sp)
#else
  ld $19,40($sp)
#endif
#ifdef TRANSFER_SPILL
  sd $zero,40($sp)
#endif
#if defined(GUARD_BEFORE_PACK)
  or $4,$19,$zero
#elif defined(SWAPPED_SAMPLE)
  bne $19,$18,.Ldone
  or $4,$19,$zero
#elif defined(TRANSFER_SPILL)
  bne $19,$18,.Ldone
  or $4,$2,$zero
#else
  bne $19,$18,.Ldone
  ld $4,40($sp)
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
  spill_result
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
  spill_result
#endif
#endif
  daddiu $17,$17,-1
  bne $17,$zero,.Lrepeat
  nop
.Lcheck_result:
#if defined(CLEAR_BYTES)
#if CLEAR_BYTES == 1
  .irp offset,40,41,42,43,44,45,46,47
    sb $zero,\offset($sp)
  .endr
#else
  .irp offset,40,42,44,46
    sh $zero,\offset($sp)
  .endr
#endif
  bne $2,$18,.Ldone
  or $4,$2,$zero
#elif defined(GUARD_HALVES) || defined(RECHECK_LOW)
  lwu $13,LOW_AT(40)($sp)
  lwu $14,HIGH_AT(40)($sp)
  sd $zero,40($sp)
#ifdef RECHECK_LOW
  bne $2,$18,.Ldone
  nop
  bne $13,$20,.Ldone
  nop
#else
  dsrl32 $19,$18,0
  dsll32 $24,$18,0
  dsrl32 $24,$24,0
#if defined(OR_DIFFERENCES) || defined(RECOMPUTE_DIFFERENCE)
#ifdef RECOMPUTE_DIFFERENCE
  xor $24,$2,$18
#else
  xor $24,$13,$24
  xor $19,$14,$19
  or $24,$24,$19
#endif
#if SPILL_DIFFERENCE == 4
  sw $24,40($sp)
#elif SPILL_DIFFERENCE == 8
  sd $24,40($sp)
#endif
#ifdef SPILL_DIFFERENCE
  or $24,$zero,$zero
#endif
#ifdef CROSS_JOIN
  beq $20,$zero,.Lcopy_difference
  or $12,$24,$zero
  b .Ltest_difference
  nop
.Lcopy_difference:
  dsll32 $12,$24,0
  dsrl32 $12,$12,0
.Ltest_difference:
  or $24,$12,$zero
#elif defined(CROSS_LOOP)
  beq $20,$zero,.Ltest_difference
  nop
.Lcarry_difference:
  daddiu $20,$20,-1
  bne $20,$zero,.Lcarry_difference
#ifdef RECOMPUTE_DIFFERENCE
  dsubu $24,$18,$2
#else
  nop
#endif
.Ltest_difference:
#endif
#if SPILL_DIFFERENCE == 4
  lw $24,40($sp)
#elif SPILL_DIFFERENCE == 8
  ld $24,40($sp)
#endif
  bne $24,$zero,.Ldone
  nop
#elif defined(HIGH_FIRST)
  bne $14,$19,.Ldone
  nop
  bne $13,$24,.Ldone
  nop
#else
  bne $13,$24,.Ldone
  nop
  bne $14,$19,.Ldone
  nop
#endif
#endif
  dsll32 $14,$14,0
  or $4,$14,$13
#elif defined(RESULT_SPILL)
  check_spill
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
  daddiu $sp,$sp,FRAME_BYTES
.end import_probe
