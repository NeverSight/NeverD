// r3 holds results, r4 the byte buffer, r5 the index and r6 the store value.
// RA=0 suppresses the base; RB=0 still reads r0.
#ifdef __powerpc64__
.abiversion 2
#define STREG std
#else
#define STREG stw
#endif
.text
.globl eval
.type eval,@function
eval:
 add 0,4,5
 lhbrx 7,4,5
 STREG 7,0(3)
 lwbrx 7,0,0
 STREG 7,__SIZEOF_LONG__(3)
#ifdef __powerpc64__
 ldbrx 7,4,5
 STREG 7,2*__SIZEOF_LONG__(3)
#endif
 addi 7,5,8
 sthbrx 6,4,7
 addi 0,4,16
 stwbrx 6,0,0
#ifdef __powerpc64__
 addi 7,5,24
 stdbrx 6,4,7
#endif
 mr 3,5
 blr
.size eval,.-eval
