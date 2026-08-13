//===- AllPlatform_OptStress11RTTests.cpp - opt-stress probes ---*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Optimizer-stress roundtrip probes in shapes the OptStress1-10 series did not
// reach, picked to hit flag-recovery / select-materialization paths that have
// historically hidden optimizer / lift semantic bugs:
//
//   * cmp3way  - three-way signed compare (-1/0/1) folded into an accumulator
//                (one compare feeding two condition codes -> nested csel/cmov).
//   * ovfladd  - unsigned add carry detected as a value (`sum < old`) inside a
//                running accumulator (carry flag consumed both as result and as
//                a boolean value).
//   * clznorm  - hand-written leading-zero normalization loop (shift until the
//                top bit is set), avoiding __builtin_clz so the lifter sees the
//                raw shift/branch idiom rather than a CLZ/BSR intrinsic.
//   * parity   - parity by xor-folding a word down to one bit (chained logical
//                shift-right + xor; the final `&1` narrows to a sub-register).
//   * boolmix  - boolean algebra `(b1&&b2)||(b3&&!b4)||(!b1&&b4)` plus an xor,
//                stressing short-circuit -> branchless boolean lowering.
//   * selwidth - data-dependent select between sub-word lanes, then sign- vs
//                zero-extend (sub-register aliasing through a select).
//
// Every kernel is integer-only, folds to a single integer return, and lowers to
// no runtime helper, so all four targets are checked native vs lifted at -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress11RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress11RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress11RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress11RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress11RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress11RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress11RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress11RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress11TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Three-way signed compare folded into a base-3 accumulator.
    {p+"_cmp3way",
     t+" "+p+"_cmp3way("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; int acc=0;\n"
     "  for(int i=0;i<40;i++){ x=x*1103515245u+12345u;\n"
     "    int u=(int)(x>>8), v=(int)((x*2654435761u)>>8);\n"
     "    int c=(u<v)?-1:((u>v)?1:0);\n"
     "    acc=acc*3+c; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x4cULL}, "OptStress11", 2},

    // Unsigned add carry detected as a value within a running accumulator.
    {p+"_ovfladd",
     t+" "+p+"_ovfladd("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u, s=0, h=0;\n"
     "  for(int i=0;i<48;i++){ x=x*1103515245u+12345u;\n"
     "    unsigned old=s; s+=(x>>11);\n"
     "    unsigned carry=(s<old)?1u:0u;\n"
     "    h=h*131u+s+carry*7u; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x9bULL}, "OptStress11", 2},

    // Hand-written leading-zero normalization (shift until top bit set).
    {p+"_clznorm",
     t+" "+p+"_clznorm("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<32;i++){ x=x*1103515245u+12345u;\n"
     "    unsigned v=(x>>3)|1u; int n=0;\n"
     "    while(!(v&0x80000000u)){ v<<=1; n++; }\n"
     "    h=h*131u+(unsigned)n+(v>>24); }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0xa7ULL}, "OptStress11", 2},

    // Parity via chained xor-fold, final &1 narrows to a sub-register.
    {p+"_parity",
     t+" "+p+"_parity("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<32;i++){ x=x*1103515245u+12345u;\n"
     "    unsigned v=x>>5;\n"
     "    v^=v>>16; v^=v>>8; v^=v>>4; v^=v>>2; v^=v>>1;\n"
     "    h=h*131u+(v&1u)+((x>>7)&2u); }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x35ULL}, "OptStress11", 2},

    // Boolean algebra over four extracted bits plus an xor combination.
    {p+"_boolmix",
     t+" "+p+"_boolmix("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<40;i++){ x=x*1103515245u+12345u;\n"
     "    int b1=((x>>3)&1u)!=0,b2=((x>>9)&1u)!=0,\n"
     "        b3=((x>>15)&1u)!=0,b4=((x>>21)&1u)!=0;\n"
     "    int r=(b1&&b2)||(b3&&!b4)||(!b1&&b4);\n"
     "    h=h*131u+(unsigned)r+(unsigned)((b1^b3)<<1); }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x6dULL}, "OptStress11", 2},

    // Data-dependent select between sub-word lanes, then sign/zero extend.
    {p+"_selwidth",
     t+" "+p+"_selwidth("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; int acc=0;\n"
     "  for(int i=0;i<32;i++){ x=x*1103515245u+12345u;\n"
     "    signed char sb=(x&0x80u)?(signed char)x:(signed char)(x>>8);\n"
     "    unsigned char ub=(x&0x4000u)?(unsigned char)(x>>16):(unsigned char)(x>>24);\n"
     "    acc+=(int)sb-(int)(unsigned)ub; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x13ULL}, "OptStress11", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress11TC("x64o11", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress11TC("x86o11", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress11TC("a64o11", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress11TC("armo11", "int");

INSTANTIATE_TEST_SUITE_P(OptStress11, X64OptStress11RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress11, X86OptStress11RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress11, A64OptStress11RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress11, ARM32OptStress11RT, ::testing::ValuesIn(kARM), rtTCName);
