// r3 is the input, r4 points at an observable call count and r5 is a
// function pointer. Direct and indirect calls share the same callee.
#include "scalar-elf-function.inc"
.text
begin_function bump
.Lbump:
 lwz 6,0(4)
 addi 6,6,1
 stw 6,0(4)
 addi 3,3,9
 blr
end_function bump
begin_function bump_last
 lwz 11,0(4)
 addi 11,11,2
 stw 11,0(4)
 add 3,3,6
 add 3,3,7
 add 3,3,8
 add 3,3,9
 add 3,3,10
 blr
end_function bump_last
#ifndef HELPER_ONLY
.macro caller name, form
begin_function \name
 mflr 0
#ifdef __powerpc64__
 std 0,16(1)
 stdu 1,-112(1)
#else
 stw 0,4(1)
 stwu 1,-32(1)
#endif
 .ifnc \form,direct
#if defined(__powerpc64__) && _CALL_ELF == 1
 std 2,40(1)
 ld 12,0(5)
 ld 2,8(5)
 ld 11,16(5)
#else
 mr 12,5
#endif
 .ifc \form,bclrl
 mtlr 12
 .else
 mtctr 12
 .endif
 .endif
 cmpwi 3,0
 .ifc \form,direct
 bcl 12,2,.Lbump
 .else
 \form 12,2,0
 .endif
#if defined(__powerpc64__) && _CALL_ELF == 1
 .ifnc \form,direct
 ld 2,40(1)
 .endif
#endif
#ifdef __powerpc64__
 addi 1,1,112
 ld 0,16(1)
#else
 addi 1,1,32
 lwz 0,4(1)
#endif
 mtlr 0
 blr
end_function \name
.endm
caller direct_call,direct
caller lr_call,bclrl
caller ctr_call,bcctrl

// Position-independent label addresses. r5 selects the taken destination of
// ctr_join, including its fallthrough. Low CTR bits are deliberately set.
.macro jump name, multiple
begin_function \name
 mflr 0
 bcl 20,31,1f
1:
 mflr 12
 mtlr 0
 .if \multiple
 cmpwi 5,0
 beq 2f
 addi 12,12,4f-1b+3
 b 3f
2:
 .endif
 addi 12,12,5f-1b+1
3:
 mtctr 12
 cmpwi 3,0
 bcctr 12,2,0
4:
 addi 3,3,1
 blr
5:
 addi 3,3,7
 blr
end_function \name
.endm
jump ctr_jump,0
jump ctr_join,1

// CR1 picks a tagged address. CR0 independently guards the transfer.
begin_function ctr_select
 mflr 0
 bcl 20,31,1f
1:
 mflr 12
 mtlr 0
 addi 8,12,4f-1b+1
 addi 9,12,5f-1b+3
 cmpwi 1,5,0
#ifdef UNKNOWN_CTR_ARM
 isel 12,8,4,6
#else
 isel 12,8,9,6
#endif
 mtctr 12
 cmpwi 3,0
 bcctr 12,2,0
 addi 3,3,1
 blr
4:
 addi 3,3,7
 blr
5:
 addi 3,3,19
 blr
end_function ctr_select
#endif
