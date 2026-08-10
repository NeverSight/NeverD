//===- X64_BsrPreserveParamRTTests.cpp - bsr-preserve param probe --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Adversarial probe for the #500 register-parameter scratch heuristic
// (`liveInOnlyFeedsScratch` / `isBsrBsfPreserve`).  That fix drops a leading
// register parameter whose only use is the BSR/BSF zero-source preserve idiom
// `SELECT(src==0, old_dst, (bits-1)-clz(src))`, because such a "live-in" is the
// instruction reading its own undefined destination, not an argument.  But a
// GENUINE second register argument that is used ONLY as that preserve value
// (`bsr dst, src` where dst held the argument) has the exact same shape — the
// heuristic must not mistake it for scratch and drop the real parameter.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64BsrPreserveParamRT : public SemanticRoundTripFixture,
                              public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64BsrPreserveParamRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // b (RSI) is the bsr destination: r=b; bsr r,a -> r = a? 63-clz(a) : b.  When
  // a==0 the result IS b, so dropping b as a phantom scratch reg returns garbage.
  {"bsr_param_zero",
   "long f(long a,long b){ unsigned long r=(unsigned long)b;\n"
   "  __asm__(\"bsrq %1,%0\":\"+r\"(r):\"r\"((unsigned long)a):\"cc\");\n"
   "  return (long)r; }\n",
   {0, 0x123456789AULL}, "BsrPreserveParam", 2},

  // Same, source non-zero: result is the bsr index (b unused) — negative control.
  {"bsr_param_nonzero",
   "long f(long a,long b){ unsigned long r=(unsigned long)b;\n"
   "  __asm__(\"bsrq %1,%0\":\"+r\"(r):\"r\"((unsigned long)a):\"cc\");\n"
   "  return (long)r; }\n",
   {0x100, 0x123456789AULL}, "BsrPreserveParam", 2},

  // bsf variant (trailing-zero scan), zero source preserves b.
  {"bsf_param_zero",
   "long f(long a,long b){ unsigned long r=(unsigned long)b;\n"
   "  __asm__(\"bsfq %1,%0\":\"+r\"(r):\"r\"((unsigned long)a):\"cc\");\n"
   "  return (long)r; }\n",
   {0, 0x55AA55AA55ULL}, "BsrPreserveParam", 2},

  // Loop so a==0 is hit repeatedly and b is genuinely live every other step.
  {"bsr_param_loop",
   "long f(long a,long b){ unsigned long acc=(unsigned long)a|1UL, keep=(unsigned long)b;\n"
   "  for(int i=0;i<40;i++){ unsigned long x=((i&3)==0)?0UL:(acc^(unsigned long)i);\n"
   "    unsigned long r=keep;\n"
   "    __asm__(\"bsrq %1,%0\":\"+r\"(r):\"r\"(x):\"cc\");\n"
   "    acc=r*131UL+(unsigned long)i; keep=acc^0x9E3779B97F4A7C15UL; }\n"
   "  return (long)acc; }\n",
   {0x10, 0x777}, "BsrPreserveParam", 2},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(BsrPreserveParam, X64BsrPreserveParamRT,
                         ::testing::ValuesIn(kX64), rtTCName);
