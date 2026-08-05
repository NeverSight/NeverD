//===- AllPlatform_OptStress43RTTests.cpp - FP/int flag mixing ---*-C++*-=//
//
// Roundtrip probes around the floating-point comparison-flag path that recent
// rounds repeatedly broke (#389 COMISS signed misread, #395 FP-flag folded into
// an integer ZF).  Each kernel interleaves float and double comparisons with
// integer comparisons and feeds the booleans into branches and branchless
// selects, so a lift that confuses ucomiss/ucomisd flags with integer flags, or
// mismodels FP min/max / sign tests, diverges.  Bounded -O2 loops returning a
// value-dependent hash; values stay finite (no NaN/Inf) and both sides run the
// same Unicorn FPU, so identical FP modeling must produce identical results.
//
//   * fpmix   - float `<`, double `>=`, and integer `>` combined in branches.
//   * fpsel   - branchless FP ternary chains -> MIN/MAX SS/SD + FP cmov/csel.
//   * fpclass - FP sign-bit and magnitude thresholds bucketed into integers.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress43RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress43RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress43RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress43RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress43RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress43RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress43RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress43RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress43TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // fpmix: float `<`, double `>=`, integer `>` combined into branches.
    {p+"_fpmix",
     t+" "+p+"_fpmix("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int acc=0;\n"
     "  float fa=(float)(int)(s&0xFFFF)/64.0f;\n"
     "  double da=(double)(int)((s>>8)&0xFFFF)/256.0;\n"
     "  for(int j=0;j<200;j++){\n"
     "    float fb=fa+(float)(j&15);\n"
     "    double db=da-(double)(j&7);\n"
     "    int c1=(fa<fb), c2=(da>=db), c3=((int)s&7)>3;\n"
     "    acc += c1?5:-2; acc ^= c2?0x33:0x11;\n"
     "    if(c1 && c3) acc+=7; else if(c2 || !c3) acc-=3;\n"
     "    acc = acc*131 + (int)fb + (int)db;\n"
     "    s=s*1103515245u+12345u;\n"
     "    fa=(float)(int)((s>>5)&0xFFFF)/32.0f;\n"
     "    da=(double)(int)((s>>3)&0xFFFF)/128.0; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x71u}, "OptStress43", 2},

    // fpsel: branchless FP min/max ternary chains (MINSS/MAXSS / FMINNM etc).
    {p+"_fpsel",
     t+" "+p+"_fpsel("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int acc=0;\n"
     "  for(int j=0;j<200;j++){\n"
     "    float x=(float)(int)(s&0x3FFF)/16.0f - 256.0f;\n"
     "    float y=(float)(int)((s>>7)&0x3FFF)/16.0f - 256.0f;\n"
     "    float lo = x<y ? x : y;\n"
     "    float hi = x<y ? y : x;\n"
     "    double dz = (double)lo * (double)hi;\n"
     "    double dc = dz < 0.0 ? -dz : dz;\n"
     "    acc = acc*131 + (int)(lo) - (int)(hi) + (int)(dc/8.0);\n"
     "    s=s*1103515245u+12345u; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x72u}, "OptStress43", 2},

    // fpclass: FP sign-bit and magnitude thresholds -> integer buckets.
    {p+"_fpclass",
     t+" "+p+"_fpclass("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int acc=0;\n"
     "  for(int j=0;j<200;j++){\n"
     "    double v=(double)(int)s/((double)(int)((s>>9)|1u));\n"
     "    int bucket;\n"
     "    if(v<0.0){ bucket = v<-100.0 ? 0 : (v<-1.0 ? 1 : 2); }\n"
     "    else     { bucket = v>100.0 ? 5 : (v>1.0 ? 4 : 3); }\n"
     "    int neg = (v<0.0); int big = ((v<0.0?-v:v) > 50.0);\n"
     "    acc = acc*131 + bucket*7 + neg*3 + big;\n"
     "    s=s*1103515245u+12345u; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x73u}, "OptStress43", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress43TC("x64o43", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress43TC("x86o43", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress43TC("a64o43", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress43TC("armo43", "int");

INSTANTIATE_TEST_SUITE_P(OptStress43, X64OptStress43RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress43, X86OptStress43RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress43, A64OptStress43RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress43, ARM32OptStress43RT, ::testing::ValuesIn(kARM), rtTCName);
