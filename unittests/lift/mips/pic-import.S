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
  daddiu $sp,$sp,-16
  sd $ra,8($sp)
#ifdef SAVED_ENTRY
  daddu $12,$25,$zero
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
  .cpreturn
  ld $ra,8($sp)
  jr $ra
  daddiu $sp,$sp,16
.end import_probe
