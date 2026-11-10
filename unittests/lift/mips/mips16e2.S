# GNU binutils MIPS16e2 encodings; no original-e2 Unicorn execution claim.
.set mips32r2
.set mips16
.set mips16e2
.set noreorder
.text
.ifdef LITTLE_ENDIAN
.equ LEFT,3
.equ RIGHT,0
.else
.equ LEFT,0
.equ RIGHT,3
.endif

.align 1
.globl m16e2_andi
.type m16e2_andi,@function
.ent m16e2_andi
m16e2_andi:
.insn
.half 0x6544 # full-destination MOVE v0,a0
andi $2,0x8001
jrc $31
.end m16e2_andi
.size m16e2_andi, .-m16e2_andi

.align 1
.globl m16e2_ori
.type m16e2_ori,@function
.ent m16e2_ori
m16e2_ori:
.insn
.half 0x6544 # full-destination MOVE v0,a0
ori $2,0x8001
jrc $31
.end m16e2_ori
.size m16e2_ori, .-m16e2_ori

.align 1
.globl m16e2_xori
.type m16e2_xori,@function
.ent m16e2_xori
m16e2_xori:
.insn
.half 0x6544 # full-destination MOVE v0,a0
xori $2,0x8001
jrc $31
.end m16e2_xori
.size m16e2_xori, .-m16e2_xori

.align 1
.globl m16e2_lui
.type m16e2_lui,@function
.ent m16e2_lui
m16e2_lui:
lui $2,0x8001
jrc $31
.end m16e2_lui
.size m16e2_lui, .-m16e2_lui

.align 1
.globl m16e2_ext
.type m16e2_ext,@function
.ent m16e2_ext
m16e2_ext:
ext $2,$4,3,9
jrc $31
.end m16e2_ext
.size m16e2_ext, .-m16e2_ext

.align 1
.globl m16e2_ins
.type m16e2_ins,@function
.ent m16e2_ins
m16e2_ins:
.insn
.half 0x6545 # full-destination MOVE v0,a1
ins $2,$4,3,9
jrc $31
.end m16e2_ins
.size m16e2_ins, .-m16e2_ins

.align 1
.globl m16e2_ins_zero
.type m16e2_ins_zero,@function
.ent m16e2_ins_zero
m16e2_ins_zero:
.insn
.half 0x6545 # full-destination MOVE v0,a1
ins $2,$0,3,9
jrc $31
.end m16e2_ins_zero
.size m16e2_ins_zero, .-m16e2_ins_zero

.align 1
.globl m16e2_movz
.type m16e2_movz,@function
.ent m16e2_movz
m16e2_movz:
li $2,0x1234
movz $2,$4,$5
jrc $31
.end m16e2_movz
.size m16e2_movz, .-m16e2_movz

.align 1
.globl m16e2_movz_zero
.type m16e2_movz_zero,@function
.ent m16e2_movz_zero
m16e2_movz_zero:
li $2,0x1234
movz $2,$0,$5
jrc $31
.end m16e2_movz_zero
.size m16e2_movz_zero, .-m16e2_movz_zero

.align 1
.globl m16e2_movn
.type m16e2_movn,@function
.ent m16e2_movn
m16e2_movn:
li $2,0x1234
movn $2,$4,$5
jrc $31
.end m16e2_movn
.size m16e2_movn, .-m16e2_movn

.align 1
.globl m16e2_movn_zero
.type m16e2_movn_zero,@function
.ent m16e2_movn_zero
m16e2_movn_zero:
li $2,0x1234
movn $2,$0,$5
jrc $31
.end m16e2_movn_zero
.size m16e2_movn_zero, .-m16e2_movn_zero

.align 1
.globl m16e2_movtz
.type m16e2_movtz,@function
.ent m16e2_movtz
m16e2_movtz:
li $2,0x1234
.insn
.half 0x651d # full-destination MOVE t8,a1
movtz $2,$4
jrc $31
.end m16e2_movtz
.size m16e2_movtz, .-m16e2_movtz

.align 1
.globl m16e2_movtz_zero
.type m16e2_movtz_zero,@function
.ent m16e2_movtz_zero
m16e2_movtz_zero:
li $2,0x1234
.insn
.half 0x651d # full-destination MOVE t8,a1
movtz $2,$0
jrc $31
.end m16e2_movtz_zero
.size m16e2_movtz_zero, .-m16e2_movtz_zero

.align 1
.globl m16e2_movtn
.type m16e2_movtn,@function
.ent m16e2_movtn
m16e2_movtn:
li $2,0x1234
.insn
.half 0x651d # full-destination MOVE t8,a1
movtn $2,$4
jrc $31
.end m16e2_movtn
.size m16e2_movtn, .-m16e2_movtn

.align 1
.globl m16e2_movtn_zero
.type m16e2_movtn_zero,@function
.ent m16e2_movtn_zero
m16e2_movtn_zero:
li $2,0x1234
.insn
.half 0x651d # full-destination MOVE t8,a1
movtn $2,$0
jrc $31
.end m16e2_movtn_zero
.size m16e2_movtn_zero, .-m16e2_movtn_zero

.align 1
.globl m16e2_lwl
.type m16e2_lwl,@function
.ent m16e2_lwl
m16e2_lwl:
.insn
.half 0x6545 # full-destination MOVE v0,a1
lwl $2,1($4)
jrc $31
.end m16e2_lwl
.size m16e2_lwl, .-m16e2_lwl

.align 1
.globl m16e2_lwr
.type m16e2_lwr,@function
.ent m16e2_lwr
m16e2_lwr:
.insn
.half 0x6545 # full-destination MOVE v0,a1
lwr $2,1($4)
jrc $31
.end m16e2_lwr
.size m16e2_lwr, .-m16e2_lwr

.align 1
.globl m16e2_swl
.type m16e2_swl,@function
.ent m16e2_swl
m16e2_swl:
.insn
.half 0x6545 # full-destination MOVE v0,a1
swl $2,1($4)
li $2,7
jrc $31
.end m16e2_swl
.size m16e2_swl, .-m16e2_swl

.align 1
.globl m16e2_swr
.type m16e2_swr,@function
.ent m16e2_swr
m16e2_swr:
.insn
.half 0x6545 # full-destination MOVE v0,a1
swr $2,1($4)
li $2,7
jrc $31
.end m16e2_swr
.size m16e2_swr, .-m16e2_swr

.align 1
.globl m16e2_load_pair
.type m16e2_load_pair,@function
.ent m16e2_load_pair
m16e2_load_pair:
lwl $2,LEFT($4)
lwr $2,RIGHT($4)
jrc $31
.end m16e2_load_pair
.size m16e2_load_pair, .-m16e2_load_pair

.align 1
.globl m16e2_store_pair
.type m16e2_store_pair,@function
.ent m16e2_store_pair
m16e2_store_pair:
.insn
.half 0x6545 # full-destination MOVE v0,a1
swl $2,LEFT($4)
swr $2,RIGHT($4)
li $2,7
jrc $31
.end m16e2_store_pair
.size m16e2_store_pair, .-m16e2_store_pair

.align 1
.globl m16e2_load_pair_reverse
.type m16e2_load_pair_reverse,@function
.ent m16e2_load_pair_reverse
m16e2_load_pair_reverse:
lwr $2,RIGHT($4)
lwl $2,LEFT($4)
jrc $31
.end m16e2_load_pair_reverse
.size m16e2_load_pair_reverse, .-m16e2_load_pair_reverse

.align 1
.globl m16e2_store_pair_reverse
.type m16e2_store_pair_reverse,@function
.ent m16e2_store_pair_reverse
m16e2_store_pair_reverse:
.insn
.half 0x6545 # full-destination MOVE v0,a1
swr $2,RIGHT($4)
swl $2,LEFT($4)
li $2,7
jrc $31
.end m16e2_store_pair_reverse
.size m16e2_store_pair_reverse, .-m16e2_store_pair_reverse

.align 1
.globl m16e2_gp_add
.type m16e2_gp_add,@function
.ent m16e2_gp_add
m16e2_gp_add:
.insn
.half 0x677c # move v1,gp
.half 0x659c # move gp,a0
addiu $2,$28,-1
.insn
.half 0x659b # restore gp from v1
jrc $31
.end m16e2_gp_add
.size m16e2_gp_add, .-m16e2_gp_add

.align 1
.globl m16e2_gp_lb
.type m16e2_gp_lb,@function
.ent m16e2_gp_lb
m16e2_gp_lb:
.insn
.half 0x677c # move v1,gp
.half 0x659c # move gp,a0
lb $2,0($28)
.insn
.half 0x659b # restore gp from v1
jrc $31
.end m16e2_gp_lb
.size m16e2_gp_lb, .-m16e2_gp_lb

.align 1
.globl m16e2_gp_lbu
.type m16e2_gp_lbu,@function
.ent m16e2_gp_lbu
m16e2_gp_lbu:
.insn
.half 0x677c # move v1,gp
.half 0x659c # move gp,a0
lbu $2,0($28)
.insn
.half 0x659b # restore gp from v1
jrc $31
.end m16e2_gp_lbu
.size m16e2_gp_lbu, .-m16e2_gp_lbu

.align 1
.globl m16e2_gp_lh
.type m16e2_gp_lh,@function
.ent m16e2_gp_lh
m16e2_gp_lh:
.insn
.half 0x677c # move v1,gp
.half 0x659c # move gp,a0
lh $2,0($28)
.insn
.half 0x659b # restore gp from v1
jrc $31
.end m16e2_gp_lh
.size m16e2_gp_lh, .-m16e2_gp_lh

.align 1
.globl m16e2_gp_lhu
.type m16e2_gp_lhu,@function
.ent m16e2_gp_lhu
m16e2_gp_lhu:
.insn
.half 0x677c # move v1,gp
.half 0x659c # move gp,a0
lhu $2,0($28)
.insn
.half 0x659b # restore gp from v1
jrc $31
.end m16e2_gp_lhu
.size m16e2_gp_lhu, .-m16e2_gp_lhu

.align 1
.globl m16e2_gp_lw
.type m16e2_gp_lw,@function
.ent m16e2_gp_lw
m16e2_gp_lw:
.insn
.half 0x677c # move v1,gp
.half 0x659c # move gp,a0
lw $2,0($28)
.insn
.half 0x659b # restore gp from v1
jrc $31
.end m16e2_gp_lw
.size m16e2_gp_lw, .-m16e2_gp_lw

.align 1
.globl m16e2_gp_sb
.type m16e2_gp_sb,@function
.ent m16e2_gp_sb
m16e2_gp_sb:
.insn
.half 0x677c # move v1,gp
.half 0x659c # move gp,a0
.insn
.half 0x6545
sb $2,0($28)
li $2,7
.insn
.half 0x659b # restore gp from v1
jrc $31
.end m16e2_gp_sb
.size m16e2_gp_sb, .-m16e2_gp_sb

.align 1
.globl m16e2_gp_sh
.type m16e2_gp_sh,@function
.ent m16e2_gp_sh
m16e2_gp_sh:
.insn
.half 0x677c # move v1,gp
.half 0x659c # move gp,a0
.insn
.half 0x6545
sh $2,0($28)
li $2,7
.insn
.half 0x659b # restore gp from v1
jrc $31
.end m16e2_gp_sh
.size m16e2_gp_sh, .-m16e2_gp_sh

.align 1
.globl m16e2_gp_sw
.type m16e2_gp_sw,@function
.ent m16e2_gp_sw
m16e2_gp_sw:
.insn
.half 0x677c # move v1,gp
.half 0x659c # move gp,a0
.insn
.half 0x6545
sw $2,0($28)
li $2,7
.insn
.half 0x659b # restore gp from v1
jrc $31
.end m16e2_gp_sw
.size m16e2_gp_sw, .-m16e2_gp_sw

# Barrier leaves retain a saved incoming link across the ordering operation.
.macro barrier_leaf name,stype=-1
.align 1
.globl \name
.type \name,@function
.ent \name
\name:
.insn
.half 0x64c4 # SAVE ra,32
.if \stype < 0
  ehb
.else
  sync \stype
.endif
li $2,7
.half 0x6444 # RESTORE ra,32
jrc $31
.end \name
.size \name, .-\name
.endm
barrier_leaf m16e2_ehb
barrier_leaf m16e2_sync0,0
barrier_leaf m16e2_sync1,1
barrier_leaf m16e2_sync2,2
barrier_leaf m16e2_sync3,3
barrier_leaf m16e2_sync4,4
barrier_leaf m16e2_sync5,5
barrier_leaf m16e2_sync6,6
barrier_leaf m16e2_sync7,7
barrier_leaf m16e2_sync8,8
barrier_leaf m16e2_sync9,9
barrier_leaf m16e2_sync10,10
barrier_leaf m16e2_sync11,11
barrier_leaf m16e2_sync12,12
barrier_leaf m16e2_sync13,13
barrier_leaf m16e2_sync14,14
barrier_leaf m16e2_sync15,15
barrier_leaf m16e2_sync16,16
barrier_leaf m16e2_sync17,17
barrier_leaf m16e2_sync18,18
barrier_leaf m16e2_sync19,19
barrier_leaf m16e2_sync20,20
barrier_leaf m16e2_sync21,21
barrier_leaf m16e2_sync22,22
barrier_leaf m16e2_sync23,23
barrier_leaf m16e2_sync24,24
barrier_leaf m16e2_sync25,25
barrier_leaf m16e2_sync26,26
barrier_leaf m16e2_sync27,27
barrier_leaf m16e2_sync28,28
barrier_leaf m16e2_sync29,29
barrier_leaf m16e2_sync30,30
barrier_leaf m16e2_sync31,31

.align 1
.globl m16e2_state_ll
.type m16e2_state_ll,@function
.ent m16e2_state_ll
m16e2_state_ll:
.insn
ll $v0,0($a0)
li $v0,7
jrc $ra
.end m16e2_state_ll
.size m16e2_state_ll, .-m16e2_state_ll

.align 1
.globl m16e2_state_sc
.type m16e2_state_sc,@function
.ent m16e2_state_sc
m16e2_state_sc:
.insn
sc $v0,0($a0)
li $v0,7
jrc $ra
.end m16e2_state_sc
.size m16e2_state_sc, .-m16e2_state_sc

.align 1
.globl m16e2_state_rdhwr
.type m16e2_state_rdhwr,@function
.ent m16e2_state_rdhwr
m16e2_state_rdhwr:
.insn
rdhwr $v0,$29
li $v0,7
jrc $ra
.end m16e2_state_rdhwr
.size m16e2_state_rdhwr, .-m16e2_state_rdhwr

.align 1
.globl m16e2_state_di
.type m16e2_state_di,@function
.ent m16e2_state_di
m16e2_state_di:
.insn
di $v0
li $v0,7
jrc $ra
.end m16e2_state_di
.size m16e2_state_di, .-m16e2_state_di

.align 1
.globl m16e2_state_ei
.type m16e2_state_ei,@function
.ent m16e2_state_ei
m16e2_state_ei:
.insn
ei $v0
li $v0,7
jrc $ra
.end m16e2_state_ei
.size m16e2_state_ei, .-m16e2_state_ei

.align 1
.globl m16e2_state_di_discard
.type m16e2_state_di_discard,@function
.ent m16e2_state_di_discard
m16e2_state_di_discard:
.insn
di
li $v0,7
jrc $ra
.end m16e2_state_di_discard
.size m16e2_state_di_discard, .-m16e2_state_di_discard

.align 1
.globl m16e2_state_ei_discard
.type m16e2_state_ei_discard,@function
.ent m16e2_state_ei_discard
m16e2_state_ei_discard:
.insn
ei
li $v0,7
jrc $ra
.end m16e2_state_ei_discard
.size m16e2_state_ei_discard, .-m16e2_state_ei_discard

.align 1
.globl m16e2_state_mfc0_status
.type m16e2_state_mfc0_status,@function
.ent m16e2_state_mfc0_status
m16e2_state_mfc0_status:
.insn
mfc0 $v0,$12,0
li $v0,7
jrc $ra
.end m16e2_state_mfc0_status
.size m16e2_state_mfc0_status, .-m16e2_state_mfc0_status

.align 1
.globl m16e2_state_mfc0_epc
.type m16e2_state_mfc0_epc,@function
.ent m16e2_state_mfc0_epc
m16e2_state_mfc0_epc:
.insn
mfc0 $v0,$14,0
li $v0,7
jrc $ra
.end m16e2_state_mfc0_epc
.size m16e2_state_mfc0_epc, .-m16e2_state_mfc0_epc

.align 1
.globl m16e2_state_mfc0_error_epc
.type m16e2_state_mfc0_error_epc,@function
.ent m16e2_state_mfc0_error_epc
m16e2_state_mfc0_error_epc:
.insn
mfc0 $v0,$30,0
li $v0,7
jrc $ra
.end m16e2_state_mfc0_error_epc
.size m16e2_state_mfc0_error_epc, .-m16e2_state_mfc0_error_epc

.align 1
.globl m16e2_state_mtc0_epc
.type m16e2_state_mtc0_epc,@function
.ent m16e2_state_mtc0_epc
m16e2_state_mtc0_epc:
.insn
mtc0 $v0,$14,0
li $v0,7
jrc $ra
.end m16e2_state_mtc0_epc
.size m16e2_state_mtc0_epc, .-m16e2_state_mtc0_epc

.align 1
.globl m16e2_state_mtc0_error_epc
.type m16e2_state_mtc0_error_epc,@function
.ent m16e2_state_mtc0_error_epc
m16e2_state_mtc0_error_epc:
.insn
mtc0 $v0,$30,0
li $v0,7
jrc $ra
.end m16e2_state_mtc0_error_epc
.size m16e2_state_mtc0_error_epc, .-m16e2_state_mtc0_error_epc

.align 1
.globl m16e2_state_pause
.type m16e2_state_pause,@function
.ent m16e2_state_pause
m16e2_state_pause:
.insn
pause
li $v0,7
jrc $ra
.end m16e2_state_pause
.size m16e2_state_pause, .-m16e2_state_pause
