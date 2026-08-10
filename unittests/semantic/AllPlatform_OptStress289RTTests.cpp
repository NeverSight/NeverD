//===- AllPlatform_OptStress289RTTests.cpp - carry/wide-arith probe ======//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// -O2 integer kernels stressing carry/borrow propagation, widening multiply
// and overflow-detection codegen paths:
//
//   * addcarry  - multi-limb add with explicit carry propagation (ADD/ADC).
//   * subborrow - multi-limb subtract with explicit borrow (SUB/SBB).
//   * wmac      - 32x32->64 widening multiply-accumulate (MUL/UMULL + ADC).
//   * ovfadd    - signed add overflow detection + saturate (branchless test).
//   * nibsum    - SWAR horizontal nibble->word reduction (shift/and/add).
//   * mixhash   - xorshift-multiply integer hash mixing.
//
// All 64-bit folds use only +/-/* and constant shifts (no variable 64-bit
// shifts -> no libcalls), abs/negate stay in the unsigned domain, and there is
// no division -- so native and lifted builds agree bit-for-bit.  Four, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress289RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress289RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress289RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress289RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress289RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress289RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress289RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress289RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress289TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // multi-limb add with explicit carry propagation (ADD/ADC).
    {p+"_addcarry",
     t+" "+p+"_addcarry("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  unsigned lo=0u, hi=0u;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned add=h; unsigned nlo=lo+add; unsigned carry=(nlo<lo)?1u:0u;\n"
     "    lo=nlo; hi=hi+carry+(h>>28);\n"
     "    acc=acc*131u+lo+hi+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress289", 2},

    // multi-limb subtract with explicit borrow (SUB/SBB).
    {p+"_subborrow",
     t+" "+p+"_subborrow("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  unsigned lo=0xFFFFFFFFu, hi=0x12345u;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned sub=h; unsigned nlo=lo-sub; unsigned borrow=(lo<sub)?1u:0u;\n"
     "    lo=nlo; hi=hi-borrow-(h>>30);\n"
     "    acc=acc*131u+lo+hi+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress289", 2},

    // 32x32->64 widening multiply-accumulate (MUL/UMULL + ADC).
    {p+"_wmac",
     t+" "+p+"_wmac("+t+" a){ unsigned h=(unsigned)a; unsigned long long acc64=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned x=h, y=h>>3; acc64 += (unsigned long long)x*(unsigned long long)y; }\n"
     "  unsigned acc=(unsigned)acc64 ^ (unsigned)(acc64>>32);\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress289", 2},

    // signed add overflow detection + saturate (branchless test).
    {p+"_ovfadd",
     t+" "+p+"_ovfadd("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0; int s=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    int v=(int)h; unsigned ur=(unsigned)s+(unsigned)v; int sr=(int)ur;\n"
     "    int ovf=((~(s^v) & (s^sr))<0)?1:0;\n"
     "    if(ovf) sr=(s<0)?(-2147483647-1):2147483647;\n"
     "    s=sr; acc=acc*131u+(unsigned)sr+(unsigned)ovf+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress289", 2},

    // SWAR horizontal nibble->word reduction (shift/and/add).
    {p+"_nibsum",
     t+" "+p+"_nibsum("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u; unsigned x=h;\n"
     "    x=(x&0x0F0F0F0Fu)+((x>>4)&0x0F0F0F0Fu);\n"
     "    x=(x&0x00FF00FFu)+((x>>8)&0x00FF00FFu);\n"
     "    x=(x&0x0000FFFFu)+((x>>16)&0x0000FFFFu);\n"
     "    acc=acc*131u+x+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress289", 2},

    // xorshift-multiply integer hash mixing.
    {p+"_mixhash",
     t+" "+p+"_mixhash("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u; unsigned x=h;\n"
     "    x^=x>>16; x*=0x7feb352du; x^=x>>15; x*=0x846ca68bu; x^=x>>16;\n"
     "    acc=acc*131u+x+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress289", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress289TC("x64o289", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress289TC("x86o289", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress289TC("a64o289", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress289TC("armo289", "int");

INSTANTIATE_TEST_SUITE_P(OptStress289, X64OptStress289RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress289, X86OptStress289RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress289, A64OptStress289RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress289, ARM32OptStress289RT, ::testing::ValuesIn(kARM), rtTCName);
