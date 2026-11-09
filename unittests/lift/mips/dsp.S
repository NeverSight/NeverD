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

.globl dsp_bitrev
.type dsp_bitrev,@function
.ent dsp_bitrev
dsp_bitrev:
  bitrev $v0,$a0
  jr $ra
  nop
.end dsp_bitrev
.size dsp_bitrev, .-dsp_bitrev

.globl dsp_raddu
.type dsp_raddu,@function
.ent dsp_raddu
dsp_raddu:
  raddu.w.qb $v0,$a0
  jr $ra
  nop
.end dsp_raddu
.size dsp_raddu, .-dsp_raddu

.globl dsp_repl_qb
.type dsp_repl_qb,@function
.ent dsp_repl_qb
dsp_repl_qb:
  repl.qb $v0,165
  jr $ra
  nop
.end dsp_repl_qb
.size dsp_repl_qb, .-dsp_repl_qb

.globl dsp_repl_ph
.type dsp_repl_ph,@function
.ent dsp_repl_ph
dsp_repl_ph:
  repl.ph $v0,-512
  jr $ra
  nop
.end dsp_repl_ph
.size dsp_repl_ph, .-dsp_repl_ph

.globl dsp_replv_qb
.type dsp_replv_qb,@function
.ent dsp_replv_qb
dsp_replv_qb:
  replv.qb $v0,$a0
  jr $ra
  nop
.end dsp_replv_qb
.size dsp_replv_qb, .-dsp_replv_qb

.globl dsp_replv_ph
.type dsp_replv_ph,@function
.ent dsp_replv_ph
dsp_replv_ph:
  replv.ph $v0,$a0
  jr $ra
  nop
.end dsp_replv_ph
.size dsp_replv_ph, .-dsp_replv_ph

.globl dsp_preceq_phl
.type dsp_preceq_phl,@function
.ent dsp_preceq_phl
dsp_preceq_phl:
  preceq.w.phl $v0,$a0
  jr $ra
  nop
.end dsp_preceq_phl
.size dsp_preceq_phl, .-dsp_preceq_phl

.globl dsp_preceq_phr
.type dsp_preceq_phr,@function
.ent dsp_preceq_phr
dsp_preceq_phr:
  preceq.w.phr $v0,$a0
  jr $ra
  nop
.end dsp_preceq_phr
.size dsp_preceq_phr, .-dsp_preceq_phr

.globl dsp_precequ_qbl
.type dsp_precequ_qbl,@function
.ent dsp_precequ_qbl
dsp_precequ_qbl:
  precequ.ph.qbl $v0,$a0
  jr $ra
  nop
.end dsp_precequ_qbl
.size dsp_precequ_qbl, .-dsp_precequ_qbl

.globl dsp_precequ_qbr
.type dsp_precequ_qbr,@function
.ent dsp_precequ_qbr
dsp_precequ_qbr:
  precequ.ph.qbr $v0,$a0
  jr $ra
  nop
.end dsp_precequ_qbr
.size dsp_precequ_qbr, .-dsp_precequ_qbr

.globl dsp_precequ_qbla
.type dsp_precequ_qbla,@function
.ent dsp_precequ_qbla
dsp_precequ_qbla:
  precequ.ph.qbla $v0,$a0
  jr $ra
  nop
.end dsp_precequ_qbla
.size dsp_precequ_qbla, .-dsp_precequ_qbla

.globl dsp_precequ_qbra
.type dsp_precequ_qbra,@function
.ent dsp_precequ_qbra
dsp_precequ_qbra:
  precequ.ph.qbra $v0,$a0
  jr $ra
  nop
.end dsp_precequ_qbra
.size dsp_precequ_qbra, .-dsp_precequ_qbra

.globl dsp_preceu_qbl
.type dsp_preceu_qbl,@function
.ent dsp_preceu_qbl
dsp_preceu_qbl:
  preceu.ph.qbl $v0,$a0
  jr $ra
  nop
.end dsp_preceu_qbl
.size dsp_preceu_qbl, .-dsp_preceu_qbl

.globl dsp_preceu_qbr
.type dsp_preceu_qbr,@function
.ent dsp_preceu_qbr
dsp_preceu_qbr:
  preceu.ph.qbr $v0,$a0
  jr $ra
  nop
.end dsp_preceu_qbr
.size dsp_preceu_qbr, .-dsp_preceu_qbr

.globl dsp_preceu_qbla
.type dsp_preceu_qbla,@function
.ent dsp_preceu_qbla
dsp_preceu_qbla:
  preceu.ph.qbla $v0,$a0
  jr $ra
  nop
.end dsp_preceu_qbla
.size dsp_preceu_qbla, .-dsp_preceu_qbla

.globl dsp_preceu_qbra
.type dsp_preceu_qbra,@function
.ent dsp_preceu_qbra
dsp_preceu_qbra:
  preceu.ph.qbra $v0,$a0
  jr $ra
  nop
.end dsp_preceu_qbra
.size dsp_preceu_qbra, .-dsp_preceu_qbra

.globl dsp_addq_w
.type dsp_addq_w,@function
.ent dsp_addq_w
dsp_addq_w:
  addq_s.w $v0,$a0,$a1
  jr $ra
  nop
.end dsp_addq_w
.size dsp_addq_w, .-dsp_addq_w

.globl dsp_subq_w
.type dsp_subq_w,@function
.ent dsp_subq_w
dsp_subq_w:
  subq_s.w $v0,$a0,$a1
  jr $ra
  nop
.end dsp_subq_w
.size dsp_subq_w, .-dsp_subq_w

.globl dsp_addsc
.type dsp_addsc,@function
.ent dsp_addsc
dsp_addsc:
  addsc $v0,$a0,$a1
  jr $ra
  nop
.end dsp_addsc
.size dsp_addsc, .-dsp_addsc

.globl dsp_addwc
.type dsp_addwc,@function
.ent dsp_addwc
dsp_addwc:
  addwc $v0,$a0,$a1
  jr $ra
  nop
.end dsp_addwc
.size dsp_addwc, .-dsp_addwc

.globl dsp_modsub
.type dsp_modsub,@function
.ent dsp_modsub
dsp_modsub:
  modsub $v0,$a0,$a1
  jr $ra
  nop
.end dsp_modsub
.size dsp_modsub, .-dsp_modsub

.globl dsp_cmpeq
.type dsp_cmpeq,@function
.ent dsp_cmpeq
dsp_cmpeq:
  cmpu.eq.qb $a0,$a1
  rddsp $v0,63
  jr $ra
  nop
.end dsp_cmpeq
.size dsp_cmpeq, .-dsp_cmpeq

.globl dsp_cmplt
.type dsp_cmplt,@function
.ent dsp_cmplt
dsp_cmplt:
  cmpu.lt.qb $a0,$a1
  rddsp $v0,63
  jr $ra
  nop
.end dsp_cmplt
.size dsp_cmplt, .-dsp_cmplt

.globl dsp_cmple
.type dsp_cmple,@function
.ent dsp_cmple
dsp_cmple:
  cmpu.le.qb $a0,$a1
  rddsp $v0,63
  jr $ra
  nop
.end dsp_cmple
.size dsp_cmple, .-dsp_cmple

.globl dsp_pick_b
.type dsp_pick_b,@function
.ent dsp_pick_b
dsp_pick_b:
  pick.qb $v0,$a0,$a1
  jr $ra
  nop
.end dsp_pick_b
.size dsp_pick_b, .-dsp_pick_b

.globl dsp_pick_h
.type dsp_pick_h,@function
.ent dsp_pick_h
dsp_pick_h:
  pick.ph $v0,$a0,$a1
  jr $ra
  nop
.end dsp_pick_h
.size dsp_pick_h, .-dsp_pick_h
