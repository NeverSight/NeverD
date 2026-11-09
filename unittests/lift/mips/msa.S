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
.macro vector_leaf name,operation
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
  \operation $w0, $w0, $w1
  copy_s.w $t0, $w0[0]
  sw $t0, 0($a0)
  copy_s.w $t0, $w0[1]
  sw $t0, 4($a0)
  copy_s.w $t0, $w0[2]
  sw $t0, 8($a0)
  copy_s.w $t0, $w0[3]
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
