/* Owned microMIPS32r2/o32 source-roundtrip kernels. Both byte orders use the
 * same assembly, including 16-bit slots and 32-bit heads at halfword alignment.
 * These leaves do not depend on a numeric native function-pointer contract. */
.set noreorder
.set micromips
.text

.globl mips_add
.type mips_add,@function
.ent mips_add
mips_add:
  jr16 $31
  addu16 $2,$4,$5
.end mips_add
.size mips_add,.-mips_add

.globl mips_load
.type mips_load,@function
.ent mips_load
mips_load:
  lw16 $2,0($4)
  jrc $31
.end mips_load
.size mips_load,.-mips_load

.globl mips_store
.type mips_store,@function
.ent mips_store
mips_store:
  sw16 $5,0($4)
  jr16 $31
  lw16 $2,0($4)
.end mips_store
.size mips_store,.-mips_store

.globl mips_stack
.type mips_stack,@function
.ent mips_stack
mips_stack:
  addu16 $2,$4,$5
  addu16 $2,$2,$6
  addu16 $2,$2,$7
  lw $8,16($29)
  jr16 $31
  addu $2,$2,$8
.end mips_stack
.size mips_stack,.-mips_stack

.globl mips_branch_capture
.type mips_branch_capture,@function
.ent mips_branch_capture
mips_branch_capture:
  /* BEQZ16 a0,+6: target is PC+2+3*2. Clang 22's local-label form
   * emitted a zero displacement in the qualification probe. Keep these
   * independently specified bytes and execute both branch outcomes. */
  .short 0x8e03
  li16 $4,99
  li16 $2,7
  jrc $31
1:
  li16 $2,11
  jrc $31
.end mips_branch_capture
.size mips_branch_capture,.-mips_branch_capture

.globl mips_merge_word
.type mips_merge_word,@function
.ent mips_merge_word
mips_merge_word:
  move $2,$5
#ifdef __MIPSEL__
  lwl $2,3($4)
  lwr $2,0($4)
#else
  lwl $2,0($4)
  lwr $2,3($4)
#endif
  jrc $31
.end mips_merge_word
.size mips_merge_word,.-mips_merge_word

#include "merge-store-word.S"
