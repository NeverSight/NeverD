//===- AllPlatform_CondExecRTTests.cpp - conditional/predicated exec -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// High-yield roundtrip probing of condition-code-driven code: clang lowers these
// kernels to predicated ARM instructions (movCC/addCC/...), x86 CMOV/SETcc, and
// AArch64 CSEL/CSINC/CSINV/CSNEG, all of which read flags the lift must keep
// paired with the right comparison.  ARM predicated execution in particular has
// been a recurring source of bugs (#263 predicated store), so the kernels mix
// conditional accumulation, multi-condition chains, conditional negate/abs and
// carry-driven selects in tight loops.  All four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64CeRT : public SemanticRoundTripFixture,
                public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64CeRT, Verify) { roundTripX64(GetParam()); }
class X86CeRT : public SemanticRoundTripFixture,
                public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86CeRT, Verify) { roundTripX86(GetParam()); }
class A64CeRT : public SemanticRoundTripFixture,
                public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64CeRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32CeRT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32CeRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeCeTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Conditional accumulation with several independent predicates per iteration
    // (clang emits predicated adds / cmov sequences sharing live flags).
    {p+"_condacc",
     t+" "+p+"_condacc("+t+" a){\n"
     "  unsigned acc=(unsigned)a, hi=0, lo=0xFFFFFFFFu;\n"
     "  for(int i=0;i<60;i++){ unsigned x=acc*2654435761u+(unsigned)i;\n"
     "    if((int)x<0) acc+=x; else acc-=x;\n"
     "    if(x>hi) hi=x; if(x<lo) lo=x;\n"
     "    acc+=(x&1u)?13u:7u; acc^=(x>0x80000000u)?0xAAu:0x55u; }\n"
     "  return ("+t+")(unsigned long)(acc^hi^lo); }\n",
     {0x33ULL}, "Ce", 2},

    // Multi-condition logical chains (a<b && c>=d) || (...): short-circuit and
    // flag reuse across the &&/|| lowering.
    {p+"_multicond",
     t+" "+p+"_multicond("+t+" a){\n"
     "  unsigned acc=(unsigned)a;\n"
     "  for(int i=0;i<56;i++){ int x=(int)acc, y=(int)(acc>>3)+i, z=(int)(acc*3u);\n"
     "    unsigned r=0;\n"
     "    if(x<y && z>=x) r=1; else if(x>=y || z<0) r=2; else r=3;\n"
     "    if((x^y)<0 && (unsigned)z>(unsigned)x) r+=4;\n"
     "    acc=acc*31u+r+(unsigned)(x<0?-x:x); }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x9ULL}, "Ce", 2},

    // Conditional negate / abs / sign chains (CSNEG / predicated rsb / cmov).
    {p+"_condneg",
     t+" "+p+"_condneg("+t+" a){\n"
     "  int acc=(int)a;\n"
     "  for(int i=0;i<56;i++){ int x=acc*7+i*3;\n"
     "    int s=(x<0)?-x:x;            \n"
     "    int g=(acc>x)?acc:x;          \n"
     "    int sgn=(x>0)-(x<0);          \n"
     "    acc=acc*3+s-g*sgn+((acc&1)?-i:i); }\n"
     "  return ("+t+")(long)acc; }\n",
     {0x5ULL}, "Ce", 2},

    // Carry/borrow-driven selects: add-with-carry style accumulation where the
    // carry of one op selects the next operand.
    {p+"_carrysel",
     t+" "+p+"_carrysel("+t+" a){\n"
     "  unsigned acc=(unsigned)a, c=0;\n"
     "  for(int i=0;i<60;i++){ unsigned x=acc+0x9E3779B1u+(unsigned)i;\n"
     "    unsigned s=acc+x+c; c=(s<acc)?1u:0u;\n"
     "    unsigned d=acc-x-c; c^=(acc<x)?1u:0u;\n"
     "    acc=s^d^(c?0xDEADu:0xBEEFu); }\n"
     "  return ("+t+")(unsigned long)(acc+c); }\n",
     {0x7ULL}, "Ce", 2},

    // Saturating arithmetic built from compares (clang may emit cmov/csel or
    // predicated moves to clamp).
    {p+"_satclamp",
     t+" "+p+"_satclamp("+t+" a){\n"
     "  int acc=(int)a;\n"
     "  for(int i=0;i<56;i++){ int x=acc+i*1000-500;\n"
     "    if(x>30000) x=30000; if(x<-30000) x=-30000;\n"
     "    unsigned u=(unsigned)acc*7u; if(u>0xFFFF0000u) u=0xFFFF0000u;\n"
     "    acc=(acc*3+x)^(int)(u>>8); }\n"
     "  return ("+t+")(long)acc; }\n",
     {0x2ULL}, "Ce", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeCeTC("x64ce", "long");
static const std::vector<RoundTripTC> kX86 = makeCeTC("x86ce", "int");
static const std::vector<RoundTripTC> kA64 = makeCeTC("a64ce", "long");
static const std::vector<RoundTripTC> kARM = makeCeTC("armce", "int");

INSTANTIATE_TEST_SUITE_P(Ce, X64CeRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Ce, X86CeRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(Ce, A64CeRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Ce, ARM32CeRT, ::testing::ValuesIn(kARM), rtTCName);
