// r3 holds results, r4 the byte buffer, r5 the index and r6 the store value.
// RA=0 suppresses the base; RB=0 still reads r0.
#include "scalar-elf-function.inc"
#ifdef __powerpc64__
#define STREG std
#else
#define STREG stw
#endif
.text
.machine power9
begin_function eval
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
 // Observe the count and CR0.EQ independently, including for a zero word
 // with nonzero bits in the upper half of a PPC64 source.
#ifdef __powerpc64__
 .set offset,3*__SIZEOF_LONG__
#else
 .set offset,2*__SIZEOF_LONG__
#endif
.macro count op
 \op 7,6
 STREG 7,offset(3)
 mfcr 8
 rlwinm 8,8,3,31,31
 STREG 8,offset+__SIZEOF_LONG__(3)
 .set offset,offset+2*__SIZEOF_LONG__
.endm
 count cnttzw.
#ifdef __powerpc64__
 count cnttzd.
#endif
 mr 3,5
 blr
end_function eval
