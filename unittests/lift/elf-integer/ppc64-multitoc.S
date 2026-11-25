// Two input objects exceed a single 64 KiB TOC. GNU ld inserts the switch.
#ifdef CALLER
#define FUNCTION local_cross
#else
#define FUNCTION partner
#endif

#ifdef ELF_V2
.abiversion 2
.text
#else
.abiversion 1
.section ".opd","aw",@progbits
#endif
.p2align 3
.globl FUNCTION
#ifndef CALLER
.hidden FUNCTION
#endif
.type FUNCTION,@function
FUNCTION:
#ifdef ELF_V2
 addis 2,12,.TOC.-FUNCTION@ha
 addi 2,2,.TOC.-FUNCTION@l
.localentry FUNCTION,.-FUNCTION
#else
.quad .Lcode,.TOC.@tocbase,0
.text
.p2align 2
.Lcode:
#endif
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
#ifdef ELF_V2
.size FUNCTION,.-FUNCTION
#endif
.section ".toc","aw",@progbits
.p2align 3
.Lvalue:
#ifdef CALLER
.quad 37
#else
.quad 93
#endif
#ifndef SINGLE_TOC
.space 40000
#endif
