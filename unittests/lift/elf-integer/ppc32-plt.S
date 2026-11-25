// A tail transfer needs the caller's original LR. The small-PIC caller
// addresses the PLT from the module GOT, independently of Clang's .got2.
.text
#ifndef SMALL_PIC
.globl tail_other
.type tail_other,@function
tail_other:
 addi 3,3,1
 b remote_other
.size tail_other,.-tail_other
#else
.globl got_cross
.type got_cross,@function
got_cross:
 mflr 0
 stwu 1,-16(1)
 stw 0,20(1)
 stw 30,8(1)
 bcl 20,31,.Lpc
.Lpc:
 mflr 30
 addis 30,30,_GLOBAL_OFFSET_TABLE_-.Lpc@ha
 addi 30,30,_GLOBAL_OFFSET_TABLE_-.Lpc@l
 addi 3,3,1
 bl remote_other@plt
 lwz 30,8(1)
 lwz 0,20(1)
 addi 1,1,16
 mtlr 0
 blr
.size got_cross,.-got_cross
#endif
.section .note.GNU-stack,"",@progbits
