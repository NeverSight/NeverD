//===- AllPlatform_OptStress32RTTests.cpp - overflow/carry flags -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Targets the overflow / carry / saturation flag idioms that the self-written
// MedFlags recovery pass handles specially but the comparison-heavy kernels
// elsewhere barely exercise.  Each kernel forces a distinct OF/CF-producing
// lowering per architecture (x86 `add;seto`/`mul` EDX-test, AArch64 `adds`/
// `umulh`, ARM32 `adds`/`umull`) and reuses the resulting flags across several
// consumers, so any divergence in how the lifter models OF/CF or how MedFlags
// rebuilds the flag chain surfaces as a return-value mismatch.
//
//   * addovf   - signed __builtin_add_overflow (OF after add).
//   * umulovf  - unsigned __builtin_mul_overflow (CF/OF after multiply).
//   * carrybig - 4-limb 16-bit big-integer add with explicit carry ripple.
//   * satu     - saturating unsigned add then unsigned sub (carry-driven cmov).
//   * sats     - loop-carried saturating signed add (overflow-driven select).
//   * cmpmulti - one subtract feeding <, <=, ==, >, >= flags in distinct arms.
//
// Integer-only, single integer return, bounded, no 64-bit divide; all four
// targets at -O2, native vs lifted.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress32RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress32RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress32RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress32RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress32RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress32RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress32RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress32RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress32TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Signed add-overflow detection: clang emits `add; seto`/`adds;b.vs`.
    {p+"_addovf",
     t+" "+p+"_addovf("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    int x=(int)s, y=(int)(s^0x9e3779b9u);\n"
     "    int sum; int ov=__builtin_add_overflow(x,y,&sum);\n"
     "    h=h*131u+(unsigned)sum+(unsigned)ov*7u; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x41ULL}, "OptStress32", 2},

    // Unsigned multiply-overflow: clang checks the high product (EDX/umulh).
    {p+"_umulovf",
     t+" "+p+"_umulovf("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned x=s, y=(s>>3)|1u, pr; int ov=__builtin_mul_overflow(x,y,&pr);\n"
     "    h=h*131u+pr+(unsigned)ov*131u; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x53ULL}, "OptStress32", 2},

    // 4-limb 16-bit big-integer add with an explicit carry ripple (carry is the
    // 17th bit of each 16-bit lane sum: sub-register + carry extraction).
    {p+"_carrybig",
     t+" "+p+"_carrybig("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<150;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned short A[4],B[4]; unsigned short R[4];\n"
     "    for(int j=0;j<4;j++){ A[j]=(unsigned short)(s>>(j*4)); B[j]=(unsigned short)((s>>(j*3))^0x55u); }\n"
     "    unsigned carry=0;\n"
     "    for(int j=0;j<4;j++){ unsigned w=(unsigned)A[j]+(unsigned)B[j]+carry; R[j]=(unsigned short)w; carry=w>>16; }\n"
     "    h=h*131u+R[0]+R[1]*3u+R[2]*7u+R[3]*11u+carry; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x6dULL}, "OptStress32", 2},

    // Saturating unsigned add then unsigned sub (carry-driven branchless cmov).
    {p+"_satu",
     t+" "+p+"_satu("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0, acc=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned add=s>>5, w=acc+add; if(w<acc) w=0xffffffffu; acc=w;\n"
     "    unsigned sub=s>>9; acc=(acc<sub)?0u:(acc-sub);\n"
     "    h=h*131u+acc; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0xb2ULL}, "OptStress32", 2},

    // Loop-carried saturating signed add (overflow-driven clamp + select).
    {p+"_sats",
     t+" "+p+"_sats("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0; int acc=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    int x=(int)(s>>2), sum;\n"
     "    if(__builtin_add_overflow(acc,x,&sum)) sum=(x<0)?(int)0x80000000:0x7fffffff;\n"
     "    acc=sum; h=h*131u+(unsigned)acc; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x18ULL}, "OptStress32", 2},

    // One subtract feeding six comparison flags in distinct arms (flag reuse).
    {p+"_cmpmulti",
     t+" "+p+"_cmpmulti("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    int x=(int)s, y=(int)(s*2654435761u); int d=x-y; unsigned r=0;\n"
     "    if(d<0) r+=1u; if(d==0) r+=2u; if(d>0) r+=4u;\n"
     "    if(x<y) r+=8u; if(x<=y) r+=16u; if(x>=y) r+=32u;\n"
     "    h=h*131u+r*7u+(unsigned)d; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x2fULL}, "OptStress32", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress32TC("x64o32", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress32TC("x86o32", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress32TC("a64o32", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress32TC("armo32", "int");

INSTANTIATE_TEST_SUITE_P(OptStress32, X64OptStress32RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress32, X86OptStress32RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress32, A64OptStress32RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress32, ARM32OptStress32RT, ::testing::ValuesIn(kARM), rtTCName);
