// Two input objects exceed a single 64 KiB TOC. GNU ld inserts the switch.
.abiversion 1
.section ".opd","aw",@progbits
.p2align 3
#ifdef CALLER
.globl local_cross
.type local_cross,@function
local_cross:
#else
.globl partner
.hidden partner
.type partner,@function
partner:
#endif
.quad .Lcode,.TOC.@tocbase,0
.text
.p2align 2
.Lcode:
#ifdef CALLER
 mflr 0
 stdu 1,-112(1)
 std 0,128(1)
 bl partner
 nop
 ld 4,.Lvalue@toc(2)
 add 3,3,4
 addi 1,1,112
 ld 0,16(1)
 mtlr 0
#else
 ld 4,.Lvalue@toc(2)
 add 3,3,4
#endif
 blr
.section ".toc","aw",@progbits
.p2align 3
.Lvalue:
#ifdef CALLER
.quad 37
#else
.quad 93
#endif
.space 40000
