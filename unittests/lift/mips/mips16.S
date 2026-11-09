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
