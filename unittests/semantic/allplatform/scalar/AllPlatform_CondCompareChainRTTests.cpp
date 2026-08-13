//===- AllPlatform_CondCompareChainRTTests.cpp - fused cond compares -*-C++-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Short-circuit boolean conditions over edge integer values.  clang -O2 fuses
// these into conditional-compare chains: AArch64 `cmp; ccmp`, ARM `cmp; it;
// cmp`, x86 paired `cmp; setcc`/branch sequences.  These stress the MedFlags
// folds (a ccmp redefines NZCV through a SELECT after the guard CMP -- the
// consumer must read the conditional flag, not the stale guard CMP).  Edge
// values (INT_MIN/MAX, -1, 0, overflow-prone deltas) make any signed/unsigned
// or NZCV mismatch observable.  All four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64CCChainRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64CCChainRT, Verify) { roundTripX64(GetParam()); }

class X86CCChainRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86CCChainRT, Verify) { roundTripX86(GetParam()); }

class A64CCChainRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64CCChainRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32CCChainRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32CCChainRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeCCTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Two-term AND/OR chains, signed and unsigned, over an edge-value table.
    {p+"_chain",
     t+" "+p+"_chain("+t+" a){\n"
     "  int v[8]={0,-1,1,(int)0x80000000,0x7FFFFFFF,2,-2,(int)0xFFFFFFFF};\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<8;i++){ int b=v[i]+(int)(a&0);\n"
     "    for(int j=0;j<8;j++){ int c=v[j];\n"
     "      unsigned u=(unsigned)b, w=(unsigned)c;\n"
     "      acc=acc*131u+((b<c && c<0)?1u:0u);\n"
     "      acc=acc*131u+((b>=c || c==0)?1u:0u);\n"
     "      acc=acc*131u+((u<w && b!=0)?1u:0u);\n"
     "      acc=acc*131u+((b<=c && u>=w)?1u:0u);\n"
     "      acc=acc*131u+((b==c || u>w)?1u:0u); } }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0ULL}, "CCChain", 2},

    // Three-term chains forcing longer ccmp sequences.
    {p+"_triple",
     t+" "+p+"_triple("+t+" a){\n"
     "  long long v[6]={0,-1,1,(long long)0x8000000000000000ULL,\n"
     "    0x7FFFFFFFFFFFFFFFLL,42};\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<6;i++){ long long b=v[i]+(long long)(a&0);\n"
     "    for(int j=0;j<6;j++){ long long c=v[j];\n"
     "      unsigned long long u=(unsigned long long)b,w=(unsigned long long)c;\n"
     "      acc=acc*131u+((b<c && c<0 && b!=-1)?1u:0u);\n"
     "      acc=acc*131u+((u>w || b==0 || c==-1)?1u:0u);\n"
     "      acc=acc*131u+((b<=c && u<w && b>0)?1u:0u); } }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0ULL}, "CCChain", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeCCTC("x64cc", "long");
static const std::vector<RoundTripTC> kX86 = makeCCTC("x86cc", "int");
static const std::vector<RoundTripTC> kA64 = makeCCTC("a64cc", "long");
static const std::vector<RoundTripTC> kARM = makeCCTC("armcc", "int");

INSTANTIATE_TEST_SUITE_P(CCChain, X64CCChainRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(CCChain, X86CCChainRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(CCChain, A64CCChainRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(CCChain, ARM32CCChainRT, ::testing::ValuesIn(kARM), rtTCName);
