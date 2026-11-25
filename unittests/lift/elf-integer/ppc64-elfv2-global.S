// Establish r12 and call a global entry that computes its own private TOC.
.abiversion 2
.text
.p2align 2
.globl global_cross
.type global_cross,@function
global_cross:
 addis 2,12,.TOC.-global_cross@ha
 addi 2,2,.TOC.-global_cross@l
.localentry global_cross,.-global_cross
 mflr 0
 stdu 1,-32(1)
 std 0,48(1)
 std 2,24(1)
 addis 12,2,other_toc@toc@ha
 addi 12,12,other_toc@toc@l
 bl .Lglobal
 ld 2,24(1)
 addis 4,2,.Lcaller@toc@ha
 ld 4,.Lcaller@toc@l(4)
 add 3,3,4
 addi 1,1,32
 ld 0,16(1)
 mtlr 0
 blr
.size global_cross,.-global_cross
.globl other_toc
.hidden other_toc
.type other_toc,@function
other_toc:
.Lglobal:
 addis 2,12,.Lpartner-other_toc@ha
 addi 2,2,.Lpartner-other_toc@l
.localentry other_toc,.-other_toc
 ld 4,0(2)
 add 3,3,4
 blr
.size other_toc,.-other_toc
.data
.p2align 3
.Lpartner:
 .quad 93
.Lcaller:
 .quad 37
