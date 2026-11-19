.globl mips_dsp_branch_slot
.type mips_dsp_branch_slot,@function
.ent mips_dsp_branch_slot
mips_dsp_branch_slot:
  wrdsp $4,63
  bposge32 1f
  wrdsp $5,1
  jr $31
  addiu $2,$0,7
1:
  jr $31
  addiu $2,$0,11
.end mips_dsp_branch_slot
.size mips_dsp_branch_slot,.-mips_dsp_branch_slot
