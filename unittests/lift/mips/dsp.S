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

.globl dsp_half_add
.type dsp_half_add,@function
.ent dsp_half_add
dsp_half_add:
  addq.ph $v0, $a0, $a1
  jr $ra
  nop
.end dsp_half_add
.size dsp_half_add, .-dsp_half_add

.globl dsp_half_add_saturate
.type dsp_half_add_saturate,@function
.ent dsp_half_add_saturate
dsp_half_add_saturate:
  addq_s.ph $v0, $a0, $a1
  jr $ra
  nop
.end dsp_half_add_saturate
.size dsp_half_add_saturate, .-dsp_half_add_saturate

.globl dsp_half_sub
.type dsp_half_sub,@function
.ent dsp_half_sub
dsp_half_sub:
  subq.ph $v0, $a0, $a1
  jr $ra
  nop
.end dsp_half_sub
.size dsp_half_sub, .-dsp_half_sub

.globl dsp_half_sub_saturate
.type dsp_half_sub_saturate,@function
.ent dsp_half_sub_saturate
dsp_half_sub_saturate:
  subq_s.ph $v0, $a0, $a1
  jr $ra
  nop
.end dsp_half_sub_saturate
.size dsp_half_sub_saturate, .-dsp_half_sub_saturate

.globl dsp_shrl
.type dsp_shrl,@function
.ent dsp_shrl
dsp_shrl:
  shrl.qb $v0,$a0,7
  jr $ra
  nop
.end dsp_shrl
.size dsp_shrl, .-dsp_shrl

.globl dsp_shrlv
.type dsp_shrlv,@function
.ent dsp_shrlv
dsp_shrlv:
  shrlv.qb $v0,$a0,$a1
  jr $ra
  nop
.end dsp_shrlv
.size dsp_shrlv, .-dsp_shrlv

.globl dsp_shra
.type dsp_shra,@function
.ent dsp_shra
dsp_shra:
  shra.ph $v0,$a0,15
  jr $ra
  nop
.end dsp_shra
.size dsp_shra, .-dsp_shra

.globl dsp_shrav
.type dsp_shrav,@function
.ent dsp_shrav
dsp_shrav:
  shrav.ph $v0,$a0,$a1
  jr $ra
  nop
.end dsp_shrav
.size dsp_shrav, .-dsp_shrav

.globl dsp_shra_r
.type dsp_shra_r,@function
.ent dsp_shra_r
dsp_shra_r:
  shra_r.ph $v0,$a0,1
  jr $ra
  nop
.end dsp_shra_r
.size dsp_shra_r, .-dsp_shra_r

.globl dsp_shrav_r
.type dsp_shrav_r,@function
.ent dsp_shrav_r
dsp_shrav_r:
  shrav_r.ph $v0,$a0,$a1
  jr $ra
  nop
.end dsp_shrav_r
.size dsp_shrav_r, .-dsp_shrav_r

.globl dsp_shra_rw
.type dsp_shra_rw,@function
.ent dsp_shra_rw
dsp_shra_rw:
  shra_r.w $v0,$a0,1
  jr $ra
  nop
.end dsp_shra_rw
.size dsp_shra_rw, .-dsp_shra_rw

.globl dsp_shrav_rw
.type dsp_shrav_rw,@function
.ent dsp_shrav_rw
dsp_shrav_rw:
  shrav_r.w $v0,$a0,$a1
  jr $ra
  nop
.end dsp_shrav_rw
.size dsp_shrav_rw, .-dsp_shrav_rw

.globl dsp_pack_h
.type dsp_pack_h,@function
.ent dsp_pack_h
dsp_pack_h:
  precrq.ph.w $v0,$a0,$a1
  jr $ra
  nop
.end dsp_pack_h
.size dsp_pack_h, .-dsp_pack_h

.globl dsp_pack_b
.type dsp_pack_b,@function
.ent dsp_pack_b
dsp_pack_b:
  precrq.qb.ph $v0,$a0,$a1
  jr $ra
  nop
.end dsp_pack_b
.size dsp_pack_b, .-dsp_pack_b
