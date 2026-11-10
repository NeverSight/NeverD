// Owned MIPS32r5/o32 FP64 MSA integer leaves.
.set noreorder
.set noat
.set msa
.text

.macro lane_leaf name,index
.globl \name
.type \name,@function
.ent \name
\name:
  fill.w $w0, $a0
  insert.w $w0[1], $a1
  insert.w $w0[2], $a2
  insert.w $w0[3], $a3
  addv.w $w31, $w0, $w0
  copy_s.w $v0, $w31[\index]
  jr $ra
  nop
.end \name
.size \name, .-\name
.endm
lane_leaf msa_lane0,0
lane_leaf msa_lane1,1
lane_leaf msa_lane2,2
lane_leaf msa_lane3,3

.globl msa_mix
.type msa_mix,@function
.ent msa_mix
msa_mix:
  fill.w $w0, $a0
  fill.w $w1, $a1
  insert.w $w0[1], $a2
  insert.w $w1[2], $a3
  addv.w $w0, $w0, $w1
  copy_s.w $v0, $w0[0]
  copy_s.w $t0, $w0[1]
  xor $v0, $v0, $t0
  copy_s.w $t1, $w0[2]
  addu $v0, $v0, $t1
  copy_s.w $t2, $w0[3]
  jr $ra
  xor $v0, $v0, $t2
.end msa_mix
.size msa_mix, .-msa_mix

.globl msa_store
.type msa_store,@function
.ent msa_store
msa_store:
  fill.w $w0, $a1
  insert.w $w0[1], $a2
  insert.w $w0[2], $a3
  insert.w $w0[3], $zero
  addv.w $w1, $w0, $w0
  copy_s.w $t0, $w1[0]
  sw $t0, 0($a0)
  copy_s.w $t0, $w1[1]
  sw $t0, 4($a0)
  copy_s.w $t0, $w1[2]
  sw $t0, 8($a0)
  copy_s.w $t0, $w1[3]
  sw $t0, 12($a0)
  jr $ra
  addiu $v0, $zero, 7
.end msa_store
.size msa_store, .-msa_store

.globl msa_return_slot
.type msa_return_slot,@function
.ent msa_return_slot
msa_return_slot:
  fill.w $w0, $a0
  addiu $v0, $zero, 7
  jr $ra
  copy_s.w $ra, $w0[0]
.end msa_return_slot
.size msa_return_slot, .-msa_return_slot


// Store every bit so small-lane carry/borrow and the high half remain observed.
.macro vector_leaf name,operation, right=$w1, unary=0, destination=$w0
.globl \name
.type \name,@function
.ent \name
\name:
  fill.w $w0, $a1
  insert.w $w0[1], $a2
  insert.w $w0[2], $a3
  xor $t0, $a1, $a3
  insert.w $w0[3], $t0
  fill.w $w1, $a3
  insert.w $w1[1], $a1
  insert.w $w1[2], $a2
  xor $t1, $a2, $a3
  insert.w $w1[3], $t1
  .ifc \destination,$w2
  lui $t2,0x5a5a
  ori $t2,$t2,0x5a5a
  fill.w $w2,$t2
  .endif
  .if \unary
  \operation \destination, $w0
  .else
  \operation \destination, $w0, \right
  .endif
  copy_s.w $t0, \destination[0]
  sw $t0, 0($a0)
  copy_s.w $t0, \destination[1]
  sw $t0, 4($a0)
  copy_s.w $t0, \destination[2]
  sw $t0, 8($a0)
  copy_s.w $t0, \destination[3]
  sw $t0, 12($a0)
  jr $ra
  addiu $v0, $zero, 7
.end \name
.size \name, .-\name
.endm
vector_leaf msa_add_b,addv.b
vector_leaf msa_add_h,addv.h
vector_leaf msa_add_d,addv.d
vector_leaf msa_sub_b,subv.b
vector_leaf msa_sub_h,subv.h
vector_leaf msa_sub_w,subv.w
vector_leaf msa_sub_d,subv.d
vector_leaf msa_and,and.v
vector_leaf msa_or,or.v
vector_leaf msa_nor,nor.v
vector_leaf msa_xor,xor.v
vector_leaf msa_ceq_b,ceq.b
vector_leaf msa_ceq_h,ceq.h
vector_leaf msa_ceq_w,ceq.w
vector_leaf msa_ceq_d,ceq.d
vector_leaf msa_clt_s_b,clt_s.b
vector_leaf msa_clt_s_h,clt_s.h
vector_leaf msa_clt_s_w,clt_s.w
vector_leaf msa_clt_s_d,clt_s.d
vector_leaf msa_clt_u_b,clt_u.b
vector_leaf msa_clt_u_h,clt_u.h
vector_leaf msa_clt_u_w,clt_u.w
vector_leaf msa_clt_u_d,clt_u.d
vector_leaf msa_cle_s_b,cle_s.b
vector_leaf msa_cle_s_h,cle_s.h
vector_leaf msa_cle_s_w,cle_s.w
vector_leaf msa_cle_s_d,cle_s.d
vector_leaf msa_cle_u_b,cle_u.b
vector_leaf msa_cle_u_h,cle_u.h
vector_leaf msa_cle_u_w,cle_u.w
vector_leaf msa_cle_u_d,cle_u.d
vector_leaf msa_max_s_b,max_s.b
vector_leaf msa_max_s_h,max_s.h
vector_leaf msa_max_s_w,max_s.w
vector_leaf msa_max_s_d,max_s.d
vector_leaf msa_max_u_b,max_u.b
vector_leaf msa_max_u_h,max_u.h
vector_leaf msa_max_u_w,max_u.w
vector_leaf msa_max_u_d,max_u.d
vector_leaf msa_min_s_b,min_s.b
vector_leaf msa_min_s_h,min_s.h
vector_leaf msa_min_s_w,min_s.w
vector_leaf msa_min_s_d,min_s.d
vector_leaf msa_min_u_b,min_u.b
vector_leaf msa_min_u_h,min_u.h
vector_leaf msa_min_u_w,min_u.w
vector_leaf msa_min_u_d,min_u.d

vector_leaf msa_sll_b,sll.b
vector_leaf msa_sll_h,sll.h
vector_leaf msa_sll_w,sll.w
vector_leaf msa_sll_d,sll.d
vector_leaf msa_sra_b,sra.b
vector_leaf msa_sra_h,sra.h
vector_leaf msa_sra_w,sra.w
vector_leaf msa_sra_d,sra.d
vector_leaf msa_srl_b,srl.b
vector_leaf msa_srl_h,srl.h
vector_leaf msa_srl_w,srl.w
vector_leaf msa_srl_d,srl.d
vector_leaf msa_bclr_b,bclr.b
vector_leaf msa_bclr_h,bclr.h
vector_leaf msa_bclr_w,bclr.w
vector_leaf msa_bclr_d,bclr.d
vector_leaf msa_bset_b,bset.b
vector_leaf msa_bset_h,bset.h
vector_leaf msa_bset_w,bset.w
vector_leaf msa_bset_d,bset.d
vector_leaf msa_bneg_b,bneg.b
vector_leaf msa_bneg_h,bneg.h
vector_leaf msa_bneg_w,bneg.w
vector_leaf msa_bneg_d,bneg.d
vector_leaf msa_mul_b,mulv.b
vector_leaf msa_mul_h,mulv.h
vector_leaf msa_mul_w,mulv.w
vector_leaf msa_mul_d,mulv.d

vector_leaf msa_madd_b,maddv.b
vector_leaf msa_madd_h,maddv.h
vector_leaf msa_madd_w,maddv.w
vector_leaf msa_madd_d,maddv.d
vector_leaf msa_msub_b,msubv.b
vector_leaf msa_msub_h,msubv.h
vector_leaf msa_msub_w,msubv.w
vector_leaf msa_msub_d,msubv.d
vector_leaf msa_max_a_b,max_a.b
vector_leaf msa_max_a_h,max_a.h
vector_leaf msa_max_a_w,max_a.w
vector_leaf msa_max_a_d,max_a.d
vector_leaf msa_min_a_b,min_a.b
vector_leaf msa_min_a_h,min_a.h
vector_leaf msa_min_a_w,min_a.w
vector_leaf msa_min_a_d,min_a.d

vector_leaf msa_add_a_b,add_a.b
vector_leaf msa_add_a_h,add_a.h
vector_leaf msa_add_a_w,add_a.w
vector_leaf msa_add_a_d,add_a.d
vector_leaf msa_adds_a_b,adds_a.b
vector_leaf msa_adds_a_h,adds_a.h
vector_leaf msa_adds_a_w,adds_a.w
vector_leaf msa_adds_a_d,adds_a.d
vector_leaf msa_adds_s_b,adds_s.b
vector_leaf msa_adds_s_h,adds_s.h
vector_leaf msa_adds_s_w,adds_s.w
vector_leaf msa_adds_s_d,adds_s.d
vector_leaf msa_adds_u_b,adds_u.b
vector_leaf msa_adds_u_h,adds_u.h
vector_leaf msa_adds_u_w,adds_u.w
vector_leaf msa_adds_u_d,adds_u.d
vector_leaf msa_subs_s_b,subs_s.b
vector_leaf msa_subs_s_h,subs_s.h
vector_leaf msa_subs_s_w,subs_s.w
vector_leaf msa_subs_s_d,subs_s.d
vector_leaf msa_subs_u_b,subs_u.b
vector_leaf msa_subs_u_h,subs_u.h
vector_leaf msa_subs_u_w,subs_u.w
vector_leaf msa_subs_u_d,subs_u.d
vector_leaf msa_ave_s_b,ave_s.b
vector_leaf msa_ave_s_h,ave_s.h
vector_leaf msa_ave_s_w,ave_s.w
vector_leaf msa_ave_s_d,ave_s.d
vector_leaf msa_ave_u_b,ave_u.b
vector_leaf msa_ave_u_h,ave_u.h
vector_leaf msa_ave_u_w,ave_u.w
vector_leaf msa_ave_u_d,ave_u.d
vector_leaf msa_aver_s_b,aver_s.b
vector_leaf msa_aver_s_h,aver_s.h
vector_leaf msa_aver_s_w,aver_s.w
vector_leaf msa_aver_s_d,aver_s.d
vector_leaf msa_aver_u_b,aver_u.b
vector_leaf msa_aver_u_h,aver_u.h
vector_leaf msa_aver_u_w,aver_u.w
vector_leaf msa_aver_u_d,aver_u.d

vector_leaf msa_add_a_same_b,add_a.b,$w0
vector_leaf msa_add_a_same_h,add_a.h,$w0
vector_leaf msa_add_a_same_w,add_a.w,$w0
vector_leaf msa_add_a_same_d,add_a.d,$w0
vector_leaf msa_adds_a_same_b,adds_a.b,$w0
vector_leaf msa_adds_a_same_h,adds_a.h,$w0
vector_leaf msa_adds_a_same_w,adds_a.w,$w0
vector_leaf msa_adds_a_same_d,adds_a.d,$w0

// Immediate, unary and non-degenerate three-input masks.
vector_leaf msa_andi_b,andi.b,165
vector_leaf msa_ori_b,ori.b,165
vector_leaf msa_nori_b,nori.b,165
vector_leaf msa_xori_b,xori.b,165
vector_leaf msa_bsel,bsel.v,$w1,0,$w2
vector_leaf msa_bmnz,bmnz.v,$w1,0,$w2
vector_leaf msa_bmz,bmz.v,$w1,0,$w2
vector_leaf msa_pcnt_b,pcnt.b,$w1,1
vector_leaf msa_pcnt_h,pcnt.h,$w1,1
vector_leaf msa_pcnt_w,pcnt.w,$w1,1
vector_leaf msa_pcnt_d,pcnt.d,$w1,1
vector_leaf msa_nlzc_b,nlzc.b,$w1,1
vector_leaf msa_nlzc_h,nlzc.h,$w1,1
vector_leaf msa_nlzc_w,nlzc.w,$w1,1
vector_leaf msa_nlzc_d,nlzc.d,$w1,1
vector_leaf msa_nloc_b,nloc.b,$w1,1
vector_leaf msa_nloc_h,nloc.h,$w1,1
vector_leaf msa_nloc_w,nloc.w,$w1,1
vector_leaf msa_nloc_d,nloc.d,$w1,1
vector_leaf msa_slli_b,slli.b,7
vector_leaf msa_slli_h,slli.h,15
vector_leaf msa_slli_w,slli.w,31
vector_leaf msa_slli_d,slli.d,63
vector_leaf msa_srli_b,srli.b,7
vector_leaf msa_srli_h,srli.h,15
vector_leaf msa_srli_w,srli.w,31
vector_leaf msa_srli_d,srli.d,63
vector_leaf msa_srai_b,srai.b,7
vector_leaf msa_srai_h,srai.h,15
vector_leaf msa_srai_w,srai.w,31
vector_leaf msa_srai_d,srai.d,63
vector_leaf msa_bclri_b,bclri.b,7
vector_leaf msa_bclri_h,bclri.h,15
vector_leaf msa_bclri_w,bclri.w,31
vector_leaf msa_bclri_d,bclri.d,63
vector_leaf msa_bseti_b,bseti.b,7
vector_leaf msa_bseti_h,bseti.h,15
vector_leaf msa_bseti_w,bseti.w,31
vector_leaf msa_bseti_d,bseti.d,63
vector_leaf msa_bnegi_b,bnegi.b,7
vector_leaf msa_bnegi_h,bnegi.h,15
vector_leaf msa_bnegi_w,bnegi.w,31
vector_leaf msa_bnegi_d,bnegi.d,63

vector_leaf msa_asub_s_b,asub_s.b
vector_leaf msa_asub_s_h,asub_s.h
vector_leaf msa_asub_s_w,asub_s.w
vector_leaf msa_asub_s_d,asub_s.d
vector_leaf msa_asub_u_b,asub_u.b
vector_leaf msa_asub_u_h,asub_u.h
vector_leaf msa_asub_u_w,asub_u.w
vector_leaf msa_asub_u_d,asub_u.d
vector_leaf msa_sat_s_0_b,sat_s.b,0
vector_leaf msa_sat_s_4_b,sat_s.b,4
vector_leaf msa_sat_s_7_b,sat_s.b,7
vector_leaf msa_sat_s_0_h,sat_s.h,0
vector_leaf msa_sat_s_8_h,sat_s.h,8
vector_leaf msa_sat_s_15_h,sat_s.h,15
vector_leaf msa_sat_s_0_w,sat_s.w,0
vector_leaf msa_sat_s_16_w,sat_s.w,16
vector_leaf msa_sat_s_31_w,sat_s.w,31
vector_leaf msa_sat_s_0_d,sat_s.d,0
vector_leaf msa_sat_s_32_d,sat_s.d,32
vector_leaf msa_sat_s_63_d,sat_s.d,63
vector_leaf msa_sat_u_0_b,sat_u.b,0
vector_leaf msa_sat_u_4_b,sat_u.b,4
vector_leaf msa_sat_u_7_b,sat_u.b,7
vector_leaf msa_sat_u_0_h,sat_u.h,0
vector_leaf msa_sat_u_8_h,sat_u.h,8
vector_leaf msa_sat_u_15_h,sat_u.h,15
vector_leaf msa_sat_u_0_w,sat_u.w,0
vector_leaf msa_sat_u_16_w,sat_u.w,16
vector_leaf msa_sat_u_31_w,sat_u.w,31
vector_leaf msa_sat_u_0_d,sat_u.d,0
vector_leaf msa_sat_u_32_d,sat_u.d,32
vector_leaf msa_sat_u_63_d,sat_u.d,63

vector_leaf msa_addvi_0_b,addvi.b,0
vector_leaf msa_addvi_0_h,addvi.h,0
vector_leaf msa_addvi_0_w,addvi.w,0
vector_leaf msa_addvi_0_d,addvi.d,0
vector_leaf msa_addvi_16_b,addvi.b,16
vector_leaf msa_addvi_16_h,addvi.h,16
vector_leaf msa_addvi_16_w,addvi.w,16
vector_leaf msa_addvi_16_d,addvi.d,16
vector_leaf msa_addvi_31_b,addvi.b,31
vector_leaf msa_addvi_31_h,addvi.h,31
vector_leaf msa_addvi_31_w,addvi.w,31
vector_leaf msa_addvi_31_d,addvi.d,31
vector_leaf msa_subvi_0_b,subvi.b,0
vector_leaf msa_subvi_0_h,subvi.h,0
vector_leaf msa_subvi_0_w,subvi.w,0
vector_leaf msa_subvi_0_d,subvi.d,0
vector_leaf msa_subvi_16_b,subvi.b,16
vector_leaf msa_subvi_16_h,subvi.h,16
vector_leaf msa_subvi_16_w,subvi.w,16
vector_leaf msa_subvi_16_d,subvi.d,16
vector_leaf msa_subvi_31_b,subvi.b,31
vector_leaf msa_subvi_31_h,subvi.h,31
vector_leaf msa_subvi_31_w,subvi.w,31
vector_leaf msa_subvi_31_d,subvi.d,31
vector_leaf msa_ceqi_0_b,ceqi.b,0
vector_leaf msa_ceqi_0_h,ceqi.h,0
vector_leaf msa_ceqi_0_w,ceqi.w,0
vector_leaf msa_ceqi_0_d,ceqi.d,0
vector_leaf msa_ceqi_16_b,ceqi.b,-16
vector_leaf msa_ceqi_16_h,ceqi.h,-16
vector_leaf msa_ceqi_16_w,ceqi.w,-16
vector_leaf msa_ceqi_16_d,ceqi.d,-16
vector_leaf msa_ceqi_31_b,ceqi.b,-1
vector_leaf msa_ceqi_31_h,ceqi.h,-1
vector_leaf msa_ceqi_31_w,ceqi.w,-1
vector_leaf msa_ceqi_31_d,ceqi.d,-1
vector_leaf msa_clti_s_0_b,clti_s.b,0
vector_leaf msa_clti_s_0_h,clti_s.h,0
vector_leaf msa_clti_s_0_w,clti_s.w,0
vector_leaf msa_clti_s_0_d,clti_s.d,0
vector_leaf msa_clti_s_16_b,clti_s.b,-16
vector_leaf msa_clti_s_16_h,clti_s.h,-16
vector_leaf msa_clti_s_16_w,clti_s.w,-16
vector_leaf msa_clti_s_16_d,clti_s.d,-16
vector_leaf msa_clti_s_31_b,clti_s.b,-1
vector_leaf msa_clti_s_31_h,clti_s.h,-1
vector_leaf msa_clti_s_31_w,clti_s.w,-1
vector_leaf msa_clti_s_31_d,clti_s.d,-1
vector_leaf msa_clti_u_0_b,clti_u.b,0
vector_leaf msa_clti_u_0_h,clti_u.h,0
vector_leaf msa_clti_u_0_w,clti_u.w,0
vector_leaf msa_clti_u_0_d,clti_u.d,0
vector_leaf msa_clti_u_16_b,clti_u.b,16
vector_leaf msa_clti_u_16_h,clti_u.h,16
vector_leaf msa_clti_u_16_w,clti_u.w,16
vector_leaf msa_clti_u_16_d,clti_u.d,16
vector_leaf msa_clti_u_31_b,clti_u.b,31
vector_leaf msa_clti_u_31_h,clti_u.h,31
vector_leaf msa_clti_u_31_w,clti_u.w,31
vector_leaf msa_clti_u_31_d,clti_u.d,31
vector_leaf msa_clei_s_0_b,clei_s.b,0
vector_leaf msa_clei_s_0_h,clei_s.h,0
vector_leaf msa_clei_s_0_w,clei_s.w,0
vector_leaf msa_clei_s_0_d,clei_s.d,0
vector_leaf msa_clei_s_16_b,clei_s.b,-16
vector_leaf msa_clei_s_16_h,clei_s.h,-16
vector_leaf msa_clei_s_16_w,clei_s.w,-16
vector_leaf msa_clei_s_16_d,clei_s.d,-16
vector_leaf msa_clei_s_31_b,clei_s.b,-1
vector_leaf msa_clei_s_31_h,clei_s.h,-1
vector_leaf msa_clei_s_31_w,clei_s.w,-1
vector_leaf msa_clei_s_31_d,clei_s.d,-1
vector_leaf msa_clei_u_0_b,clei_u.b,0
vector_leaf msa_clei_u_0_h,clei_u.h,0
vector_leaf msa_clei_u_0_w,clei_u.w,0
vector_leaf msa_clei_u_0_d,clei_u.d,0
vector_leaf msa_clei_u_16_b,clei_u.b,16
vector_leaf msa_clei_u_16_h,clei_u.h,16
vector_leaf msa_clei_u_16_w,clei_u.w,16
vector_leaf msa_clei_u_16_d,clei_u.d,16
vector_leaf msa_clei_u_31_b,clei_u.b,31
vector_leaf msa_clei_u_31_h,clei_u.h,31
vector_leaf msa_clei_u_31_w,clei_u.w,31
vector_leaf msa_clei_u_31_d,clei_u.d,31
vector_leaf msa_maxi_s_0_b,maxi_s.b,0
vector_leaf msa_maxi_s_0_h,maxi_s.h,0
vector_leaf msa_maxi_s_0_w,maxi_s.w,0
vector_leaf msa_maxi_s_0_d,maxi_s.d,0
vector_leaf msa_maxi_s_16_b,maxi_s.b,-16
vector_leaf msa_maxi_s_16_h,maxi_s.h,-16
vector_leaf msa_maxi_s_16_w,maxi_s.w,-16
vector_leaf msa_maxi_s_16_d,maxi_s.d,-16
vector_leaf msa_maxi_s_31_b,maxi_s.b,-1
vector_leaf msa_maxi_s_31_h,maxi_s.h,-1
vector_leaf msa_maxi_s_31_w,maxi_s.w,-1
vector_leaf msa_maxi_s_31_d,maxi_s.d,-1
vector_leaf msa_maxi_u_0_b,maxi_u.b,0
vector_leaf msa_maxi_u_0_h,maxi_u.h,0
vector_leaf msa_maxi_u_0_w,maxi_u.w,0
vector_leaf msa_maxi_u_0_d,maxi_u.d,0
vector_leaf msa_maxi_u_16_b,maxi_u.b,16
vector_leaf msa_maxi_u_16_h,maxi_u.h,16
vector_leaf msa_maxi_u_16_w,maxi_u.w,16
vector_leaf msa_maxi_u_16_d,maxi_u.d,16
vector_leaf msa_maxi_u_31_b,maxi_u.b,31
vector_leaf msa_maxi_u_31_h,maxi_u.h,31
vector_leaf msa_maxi_u_31_w,maxi_u.w,31
vector_leaf msa_maxi_u_31_d,maxi_u.d,31
vector_leaf msa_mini_s_0_b,mini_s.b,0
vector_leaf msa_mini_s_0_h,mini_s.h,0
vector_leaf msa_mini_s_0_w,mini_s.w,0
vector_leaf msa_mini_s_0_d,mini_s.d,0
vector_leaf msa_mini_s_16_b,mini_s.b,-16
vector_leaf msa_mini_s_16_h,mini_s.h,-16
vector_leaf msa_mini_s_16_w,mini_s.w,-16
vector_leaf msa_mini_s_16_d,mini_s.d,-16
vector_leaf msa_mini_s_31_b,mini_s.b,-1
vector_leaf msa_mini_s_31_h,mini_s.h,-1
vector_leaf msa_mini_s_31_w,mini_s.w,-1
vector_leaf msa_mini_s_31_d,mini_s.d,-1
vector_leaf msa_mini_u_0_b,mini_u.b,0
vector_leaf msa_mini_u_0_h,mini_u.h,0
vector_leaf msa_mini_u_0_w,mini_u.w,0
vector_leaf msa_mini_u_0_d,mini_u.d,0
vector_leaf msa_mini_u_16_b,mini_u.b,16
vector_leaf msa_mini_u_16_h,mini_u.h,16
vector_leaf msa_mini_u_16_w,mini_u.w,16
vector_leaf msa_mini_u_16_d,mini_u.d,16
vector_leaf msa_mini_u_31_b,mini_u.b,31
vector_leaf msa_mini_u_31_h,mini_u.h,31
vector_leaf msa_mini_u_31_w,mini_u.w,31
vector_leaf msa_mini_u_31_d,mini_u.d,31
