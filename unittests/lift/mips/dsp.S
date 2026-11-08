// Owned classic MIPS32r2 DSP leaves. DSPControl remains observable by callers.
.set noreorder
.set noat
.set dsp
.text

.globl dsp_read
.type dsp_read,@function
.ent dsp_read
dsp_read:
  rddsp $v0, 63
  jr $ra
  nop
.end dsp_read
.size dsp_read, .-dsp_read

.globl dsp_write
.type dsp_write,@function
.ent dsp_write
dsp_write:
  wrdsp $a0, 63
  jr $ra
  addiu $v0, $zero, 7
.end dsp_write
.size dsp_write, .-dsp_write

.globl dsp_partial
.type dsp_partial,@function
.ent dsp_partial
dsp_partial:
  wrdsp $a0, 21
  rddsp $v0, 63
  jr $ra
  nop
.end dsp_partial
.size dsp_partial, .-dsp_partial

.globl dsp_add
.type dsp_add,@function
.ent dsp_add
dsp_add:
  addu.qb $v0, $a0, $a1
  jr $ra
  nop
.end dsp_add
.size dsp_add, .-dsp_add

.globl dsp_saturate
.type dsp_saturate,@function
.ent dsp_saturate
dsp_saturate:
  addu_s.qb $v0, $a0, $a1
  jr $ra
  nop
.end dsp_saturate
.size dsp_saturate, .-dsp_saturate

.globl dsp_sub
.type dsp_sub,@function
.ent dsp_sub
dsp_sub:
  subu.qb $v0, $a0, $a1
  jr $ra
  nop
.end dsp_sub
.size dsp_sub, .-dsp_sub

.globl dsp_sub_saturate
.type dsp_sub_saturate,@function
.ent dsp_sub_saturate
dsp_sub_saturate:
  subu_s.qb $v0, $a0, $a1
  jr $ra
  nop
.end dsp_sub_saturate
.size dsp_sub_saturate, .-dsp_sub_saturate

.globl dsp_sub_zero
.type dsp_sub_zero,@function
.ent dsp_sub_zero
dsp_sub_zero:
  subu.qb $zero, $a0, $a1
  jr $ra
  addiu $v0, $zero, 7
.end dsp_sub_zero
.size dsp_sub_zero, .-dsp_sub_zero

.globl dsp_zero
.type dsp_zero,@function
.ent dsp_zero
dsp_zero:
  addu.qb $zero, $a0, $a1
  jr $ra
  addiu $v0, $zero, 7
.end dsp_zero
.size dsp_zero, .-dsp_zero

.globl dsp_read_then_write
.type dsp_read_then_write,@function
.ent dsp_read_then_write
dsp_read_then_write:
  rddsp $v0, 63
  wrdsp $a0, 63
  jr $ra
  nop
.end dsp_read_then_write
.size dsp_read_then_write, .-dsp_read_then_write

.globl dsp_return_slot
.type dsp_return_slot,@function
.ent dsp_return_slot
dsp_return_slot:
  addiu $v0, $zero, 7
  jr $ra
  rddsp $ra, 63
.end dsp_return_slot
.size dsp_return_slot, .-dsp_return_slot

.globl dsp_plain_add
.type dsp_plain_add,@function
.ent dsp_plain_add
dsp_plain_add:
  addu $v0, $a0, $a1
  jr $ra
  nop
.end dsp_plain_add
.size dsp_plain_add, .-dsp_plain_add
