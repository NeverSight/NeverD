//===- AllPlatform_MultiWordAbsRTTests.cpp - 64-bit abs/neg/sbb probes ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Isolates the two-word (i386/ARM32) 64-bit abs / negate / borrow-subtract
// idioms that WideHash64 surfaced as an i386-only divergence.  Each kernel is
// a few iterations so a failure points at one multi-word op rather than a hash
// avalanche, and each is instantiated at both opt levels (NeverD optimizer on
// and off) to separate a lift bug from an optimizer miscompile.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64MultiWordAbsRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64MultiWordAbsRT, Verify) { roundTripX64(GetParam()); }

class X86MultiWordAbsRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86MultiWordAbsRT, Verify) { roundTripX86(GetParam()); }

class A64MultiWordAbsRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64MultiWordAbsRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32MultiWordAbsRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32MultiWordAbsRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeMWATC(const char *prefix, const char *T,
                                          int opt, bool noopt) {
  std::string p = prefix, t = T;
  auto sfx = noopt ? std::string("_no") : std::string("");
  return {
    // 64-bit branchless abs: sign-mask (sar #63) + xor/sub/sbb across words.
    {p+"_abs"+sfx,
     t+" "+p+"_abs"+sfx+"("+t+" a){\n"
     "  unsigned long long acc=0;\n"
     "  for(int i=0;i<6;i++){ long long v=((long long)(a+i)<<32)|(unsigned)((a+i)*2654435761u);\n"
     "    unsigned long long d=(unsigned long long)(v<0?-v:v);\n"
     "    acc+=d; acc^=d>>32; }\n"
     "  return ("+t+")(acc^(acc>>32));\n"
     "}\n",
     {0x9ULL}, "MultiWordAbs", opt, "", noopt},

    // 64-bit negate (two-word neg = 0 - v with borrow).
    {p+"_neg"+sfx,
     t+" "+p+"_neg"+sfx+"("+t+" a){\n"
     "  unsigned long long acc=0;\n"
     "  for(int i=0;i<6;i++){ long long v=((long long)(a*3-i)<<32)|(unsigned)(a+i*7);\n"
     "    long long n=-v; acc+=(unsigned long long)n; acc^=(unsigned long long)n>>32; }\n"
     "  return ("+t+")(acc^(acc>>32));\n"
     "}\n",
     {0x11ULL}, "MultiWordAbs", opt, "", noopt},

    // 64-bit signed min/max kept separate (forces a real two-word compare).
    {p+"_minmax"+sfx,
     t+" "+p+"_minmax"+sfx+"("+t+" a){\n"
     "  unsigned long long acc=0;\n"
     "  for(int i=0;i<6;i++){ long long x=((long long)(a+i)<<32)|(unsigned)(a*5+i);\n"
     "    long long y=((long long)(a*2-i)<<32)|(unsigned)(a+i*9);\n"
     "    long long mn=x<y?x:y, mx=x<y?y:x;\n"
     "    acc+=(unsigned long long)mn*7u; acc^=(unsigned long long)mx; acc+=acc>>32; }\n"
     "  return ("+t+")(acc^(acc>>32));\n"
     "}\n",
     {0x19ULL}, "MultiWordAbs", opt, "", noopt},

    // Explicit borrow-subtract chain (a-b across two words, signed compare use).
    {p+"_sbb"+sfx,
     t+" "+p+"_sbb"+sfx+"("+t+" a){\n"
     "  unsigned long long acc=0;\n"
     "  for(int i=0;i<6;i++){ unsigned long long x=((unsigned long long)(a+i)<<32)|(unsigned)(a*11+i);\n"
     "    unsigned long long y=((unsigned long long)(a-i)<<32)|(unsigned)(a*13-i);\n"
     "    unsigned long long d=x-y; acc+=d; acc^=d>>32; acc+=acc<<3; }\n"
     "  return ("+t+")(acc^(acc>>32));\n"
     "}\n",
     {0x21ULL}, "MultiWordAbs", opt, "", noopt},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeMWATC("x64mwa", "long", 2, false);
static const std::vector<RoundTripTC> kX64N = makeMWATC("x64mwa", "long", 0, true);
static const std::vector<RoundTripTC> kX86 = makeMWATC("x86mwa", "int", 2, false);
static const std::vector<RoundTripTC> kX86N = makeMWATC("x86mwa", "int", 0, true);
static const std::vector<RoundTripTC> kA64 = makeMWATC("a64mwa", "long", 2, false);
static const std::vector<RoundTripTC> kARM = makeMWATC("armmwa", "int", 2, false);

static std::vector<RoundTripTC> join(std::vector<RoundTripTC> A,
                                     const std::vector<RoundTripTC> &B) {
  A.insert(A.end(), B.begin(), B.end());
  return A;
}
static const std::vector<RoundTripTC> kX64All = join(kX64, kX64N);
static const std::vector<RoundTripTC> kX86All = join(kX86, kX86N);

INSTANTIATE_TEST_SUITE_P(MultiWordAbs, X64MultiWordAbsRT,
                         ::testing::ValuesIn(kX64All), rtTCName);
INSTANTIATE_TEST_SUITE_P(MultiWordAbs, X86MultiWordAbsRT,
                         ::testing::ValuesIn(kX86All), rtTCName);
INSTANTIATE_TEST_SUITE_P(MultiWordAbs, A64MultiWordAbsRT,
                         ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(MultiWordAbs, ARM32MultiWordAbsRT,
                         ::testing::ValuesIn(kARM), rtTCName);
