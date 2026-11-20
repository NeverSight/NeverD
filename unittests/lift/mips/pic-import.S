/* The assembler emits the n32/n64 GP-displacement relocations and saves
   the caller's GP. The dynamic linker supplies dispatch through CALL16. */
.abicalls
.option pic2
.set noreorder
.text
.globl import_probe
.type dispatch,@function
.ent import_probe
import_probe:
#if defined(LIKELY_TAKEN)
  bnel $4,$zero,1f
  daddu $12,$25,$zero
  jr $ra
  daddiu $2,$zero,11
1:
#elif defined(LIKELY_SKIPPED)
  daddu $12,$25,$zero
  beql $4,$zero,2f
#ifdef SLOT_BARRIER
  sync
#else
  or $12,$zero,$zero
#endif
#elif defined(LIKELY_JOIN)
  daddu $12,$25,$zero
  bnel $4,$zero,1f
  daddiu $4,$4,1
1:
#elif defined(ENTRY_HEADER)
3:
  daddiu $4,$4,1
  andi $13,$4,3
  bne $13,$zero,3b
  nop
#elif defined(ENTRY_LOOP)
  daddu $12,$25,$zero
  andi $13,$4,3
  beq $13,$zero,1f
  nop
3:
  daddiu $13,$13,-1
  bne $13,$zero,3b
  daddiu $4,$4,1
1:
#elif defined(SAVED_ENTRY)
  daddu $12,$25,$zero
#endif
  daddiu $sp,$sp,-16
  sd $ra,8($sp)
#ifdef SLOT_SETUP
  sd $gp,0($sp)
  lui $12,%hi(%neg(%gp_rel(import_probe)))
  addiu $12,$12,%lo(%neg(%gp_rel(import_probe)))
  bnel $4,$zero,1f
#if __SIZEOF_POINTER__ == 8
  daddu $gp,$12,$25
#else
  addu $gp,$12,$25
#endif
  daddiu $2,$zero,11
  b 3f
  nop
1:
#elif defined(SAVED_ENTRY)
  or $25,$4,$zero
  .cpsetup $12,0,import_probe
#else
  .cpsetup $25,0,import_probe
#endif
#if __SIZEOF_POINTER__ == 8
  ld $25,%call16(dispatch)($gp)
#else
  lw $25,%call16(dispatch)($gp)
#endif
  jalr $25
  daddiu $5,$zero,37
  daddiu $2,$2,1
#ifdef SLOT_SETUP
3:
  ld $gp,0($sp)
#else
  .cpreturn
#endif
  ld $ra,8($sp)
  jr $ra
  daddiu $sp,$sp,16
#ifdef LIKELY_SKIPPED
2:
  jr $ra
  daddiu $2,$zero,11
#endif
.end import_probe
