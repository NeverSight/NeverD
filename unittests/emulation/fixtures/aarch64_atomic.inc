//===- aarch64_atomic.inc - Original LSE observation assembly -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Each instruction uses X0[,X1], X2[,X3], [X4]. Record the exact instruction
// site and resume after it when Windows supplies an exception context.
#define NEVERD_ATOMIC_CASE(Name, Kind, Width, Count, Word, Expected)           \
  static void atomic##Name(AtomicWord *Input, AtomicWord *Output) {            \
    __asm__ volatile(                                                          \
        "adr x5, 1f\n\tstr x5, [%[site]]\n\t"                                  \
        "adr x5, 2f\n\tstr x5, [%[resume]]\n\t"                                \
        "ldp x0, x1, [%[input]]\n\tldp x2, x3, [%[input], #16]\n\t"            \
        "ldr x4, [%[input], #32]\n\tldr x5, [%[input], #40]\n\tmsr nzcv, "     \
        "x5\n\t"                                                               \
        "1:\n\t.inst " #Word "\n\t"                                            \
        "2:\n\tstp x0, x1, [%[output]]\n\tstp x2, x3, [%[output], #16]\n\t"    \
        "str x4, [%[output], #32]\n\tmrs x5, nzcv\n\tstr x5, [%[output], "     \
        "#40]\n\t"                                                             \
        :                                                                      \
        : [input] "r"(Input), [output] "r"(Output), [site] "r"(&AtomicSite),   \
          [resume] "r"(&ResumeSite)                                            \
        : "x0", "x1", "x2", "x3", "x4", "x5", "memory", "cc");                 \
  }
#include "../aarch64/AArch64AtomicCases.def"
#undef NEVERD_ATOMIC_CASE
