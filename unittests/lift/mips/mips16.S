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
