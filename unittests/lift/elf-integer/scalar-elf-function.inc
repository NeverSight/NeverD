// ELFv1 names a three-word descriptor. The other scalar ABIs name the code.
#ifdef __powerpc64__
.abiversion _CALL_ELF
#endif
.macro begin_function name
 .globl \name
 .type \name,@function
#if defined(__powerpc64__) && _CALL_ELF == 1
 .pushsection ".opd","aw",@progbits
 .p2align 3
\name: .quad .Lentry_\name,.TOC.@tocbase,0
 .size \name,.-\name
 .popsection
.Lentry_\name:
#else
\name:
#endif
.endm

.macro end_function name
#if !defined(__powerpc64__) || _CALL_ELF != 1
 .size \name,.-\name
#endif
.endm
