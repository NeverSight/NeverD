# MIPS16e32 scalar/control source qualification. GNU as/ld own encoding and
# linking; .half chooses the qualified full-destination MOVE alternative.
.set mips16
.set noreorder
.text

.macro begin name
.align 1
.globl \name
.type \name, @function
.ent \name
\name:
.endm
.macro end name
.end \name
.size \name, .-\name
.endm

begin m16_add
addu $2,$4,$5
jrc $31
end m16_add

begin m16_unsigned
li $2,0x8000
addu $2,$2,$4
jrc $31
end m16_unsigned

begin m16_logic
.insn
.half 0x6544 # move v0,a0: full GPR destination, compact source
xor $2,$5
jrc $31
end m16_logic

begin m16_branch
beqz $4,1f
li $2,9
jrc $31
1:
li $2,7
jrc $31
end m16_branch

begin m16_test
cmpi $4,0xffff
btnez 1f
li $2,1
jrc $31
1:
li $2,0
jrc $31
end m16_test

begin m16_slot
.insn
.half 0x6544 # move v0,a0
jr $31
addiu $2,1
end m16_slot

begin m16_skip_data
b 1f
.half 0xffff
1:
addu $2,$4,$5
jrc $31
end m16_skip_data

begin m16_pc_load
lw $2,1f
jrc $31
.align 2
1:
.word 0x89abcdef
end m16_pc_load

# Exercise signed byte/halfword loads, exact stores and base capture.
begin m16_memory
sw $5,0($4)
lb $2,1($4)
lhu $3,2($4)
addu $2,$2,$3
jrc $31
end m16_memory

begin m16_stack
addiu $sp,-24
sw $4,16($sp)
lw $2,16($sp)
addiu $sp,24
jrc $31
end m16_stack

# Keep the authenticated entry return in a callee-saved GPR across the call.
# The stack slot preserves the caller's s0, outside o32's outgoing home area.
.macro call_frame name,transfer,target
begin \name
addiu $sp,-24
sw $16,16($sp)
move $16,$31
\transfer \target
addiu $4,1
move $31,$16
lw $16,16($sp)
addiu $sp,24
jrc $31
end \name
.endm
call_frame m16_call,jal,m16_add
call_frame m16_callx,jalx,m16_classic_add

.set nomips16
.align 2
.globl m16_classic_add
.type m16_classic_add,@function
.ent m16_classic_add
m16_classic_add:
addu $2,$4,$5
jr $31
nop
.end m16_classic_add
.size m16_classic_add, .-m16_classic_add
.set mips16

begin m16_sllv
move $2,$4
sllv $2,$5
jrc $31
end m16_sllv

begin m16_srlv
move $2,$4
srlv $2,$5
jrc $31
end m16_srlv

begin m16_srav
move $2,$4
srav $2,$5
jrc $31
end m16_srav

begin m16_shift_imm
sll $2,$4,8
srl $3,$5,31
xor $2,$3
sra $2,$2,7
jrc $31
end m16_shift_imm

begin m16_cmp
cmp $4,$5
move $2,$24
jrc $31
end m16_cmp

begin m16_slt
slt $4,$5
move $2,$24
jrc $31
end m16_slt

begin m16_sltu
sltu $4,$5
move $2,$24
jrc $31
end m16_sltu

begin m16_neg
neg $2,$4
jrc $31
end m16_neg

begin m16_mult
mult $4,$5
mfhi $2
mflo $3
xor $2,$3
jrc $31
end m16_mult

begin m16_multu
multu $4,$5
mfhi $2
mflo $3
xor $2,$3
jrc $31
end m16_multu

begin m16_mult_hi
mult $4,$5
mfhi $2
jrc $31
end m16_mult_hi

begin m16_mult_lo
mult $4,$5
mflo $2
jrc $31
end m16_mult_lo

begin m16_multu_hi
multu $4,$5
mfhi $2
jrc $31
end m16_multu_hi

begin m16_multu_lo
multu $4,$5
mflo $2
jrc $31
end m16_multu_lo

# Physical SVRS and DIV encodings avoid assembler macros and hidden checks.
begin m16_save
.insn
.half 0x64f8 # save ra,s1,s0; frame=64
li $16,9
li $17,11
addu $2,$4,$5
.half 0x6478
jrc $31
end m16_save

begin m16_savex
.insn
.half 0xf7f3,0x64ff # all saved GPRs and a1-a3 as statics, frame=2040
li $2,7
.half 0x6552,0x6572,0x6592,0x65b2,0x65d2,0x65f2,0x65da # s2-s7/fp clobbers
li $16,9
li $17,11
addu $2,$4,$5
.half 0xf7f3,0x647f
jrc $31
end m16_savex

begin m16_save_args
.insn
.half 0xf00e,0x64f8 # a0-a3 incoming homes; frame=64
addu $2,$4,$5
.half 0xf00e,0x6478
jrc $31
end m16_save_args

begin m16_save_call
.insn
.half 0x64a3 # save s0,24
move $16,$31
jal m16_add
addiu $4,1
move $31,$16
.half 0x6423
jrc $31
end m16_save_call

.macro division_leaf name,word,result
begin \name
.insn
.half 0x64c4 # save ra,32
.half \word
mflo $2
mfhi $3
.if \result == 0
xor $2,$3
.elseif \result == 1
move $2,$3
.endif
.half 0x6444
jrc $31
end \name
.endm
division_leaf m16_div,0xecba,0
division_leaf m16_divu,0xecbb,0
division_leaf m16_div_hi,0xecba,1
division_leaf m16_div_lo,0xecba,2
division_leaf m16_divu_hi,0xecbb,1
division_leaf m16_divu_lo,0xecbb,2

begin m16_div_dead
.insn
.half 0xecba
li $2,7
jrc $31
end m16_div_dead

begin m16_div_checked
beqz $5,1f
.insn
.half 0xecba
mflo $2
jrc $31
1:
li $2,42
jrc $31
end m16_div_checked

# A defined zero path in the guest lies outside the retained-division source domain.
begin m16_div_after_checked
.insn
.half 0xecba
beqz $5,1f
mflo $2
jrc $31
1:
li $2,42
jrc $31
end m16_div_after_checked
