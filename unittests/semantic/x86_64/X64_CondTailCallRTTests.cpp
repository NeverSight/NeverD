//===- X64_CondTailCallRTTests.cpp - Conditional tail calls -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Clang folds a tail call that is all a branch arm does into the branch:
// `cmp $5,%esi; jbe memset; ret`.  When the object does not define the
// callee, the jump's target has an identity and no bytes.  The taken arm was
// dropped there, and the lifted function returned without calling whatever
// the comparison said.
//
// The wrapper passes its own arguments on, the third one a value only the
// caller knows, or the optimizer would fold it into the wrapper and set a
// register before an unconditional jump.  `-fno-builtin` keeps the call a
// call; the harness links the routine into both images.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"
#include "SourceRouteCases.h"

class X64CondTailCallRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64CondTailCallRT, Verify) { roundTripX64(GetParam()); }

#define COND_TAIL_CALL_FN(Name)                                                \
  "void *memset(void *, int, unsigned long);\n"                                \
  "static void *" Name "_wrap(void *p, int c, unsigned long n)\n"              \
  "    __attribute__((noinline));\n"                                           \
  "long " Name "(long a){ unsigned char b[32];\n"                              \
  "  for(int i=0;i<32;i++) b[i]=(unsigned char)(i*7+a);\n"                     \
  "  " Name "_wrap(b,(int)(a&7),8+(unsigned long)((a>>4)&7));\n"               \
  "  return (long)(b[3]+b[15]*2+b[31]*3); }\n"                                 \
  "static void *" Name "_wrap(void *p, int c, unsigned long n){\n"             \
  "  if(c>5) return p; return memset(p,c,n); }\n"

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // c = 1: the jump is taken and ten bytes are filled.
  {"x64ctc_taken", COND_TAIL_CALL_FN("x64ctc_taken"), {0x21ULL},
   "CondTailCall", 2, "-fno-builtin"},
  // c = 6: the wrapper returns and the buffer keeps what the loop wrote.
  {"x64ctc_passed", COND_TAIL_CALL_FN("x64ctc_passed"), {0x26ULL},
   "CondTailCall", 2, "-fno-builtin"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(CondTailCall, X64CondTailCallRT,
                         ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(CondTailCallHighC, X64CondTailCallRT,
                         ::testing::ValuesIn(onHighC(kX64)), rtTCName);
INSTANTIATE_TEST_SUITE_P(CondTailCallLLVMC, X64CondTailCallRT,
                         ::testing::ValuesIn(onLLVMC(kX64)), rtTCName);
