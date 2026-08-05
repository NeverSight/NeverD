//===- AllPlatform_OptStress271RTTests.cpp - FP control flow at -O0 ======//
//
// Scalar floating point feeding real BRANCHES (not just select) at -O0 — the
// dual of OptStress258, which only exercised FP compare -> select.  At -O0 the
// FP compare -> conditional-branch path is emitted very explicitly (i386
// fcom/fnstsw ax + sahf + jcc, x64/AArch64 ucomiss/fcmp + jcc, ARM32 vcmp +
// vmrs + jcc), and ordered vs unordered compares differ on NaN exactly here.
//
//   * fbr      - FP compare -> if/else-if branch, bounded recurrence.
//   * fminmax  - running min/max via branches (not the select idiom).
//   * fabsneg  - fabs + negate, sign-bit / bitcast paths.
//   * funord   - ordered (<) vs unordered (!(>=)) branch with a NaN operand.
//   * fclamp   - clamp a double to [-100,100] via branches.
//   * fcmp3    - three-way sign-of-difference fed into an integer accumulator.
//
// Integer in / integer out (FP folded to one integer return), LCG-seeded, all
// four targets, -O0.  Magnitudes stay bounded so (int) truncation is defined;
// only float/double + signed int casts, so i386/ARM32 stay libcall-free.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress271RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress271RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress271RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress271RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress271RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress271RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress271RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress271RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress271TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // FP compare -> if/else-if branch, bounded recurrence folded to int.
    {p+"_fbr",
     t+" "+p+"_fbr("+t+" a){ unsigned h=(unsigned)a; float acc=0.0f;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    float x=(float)(int)(h&0x3ffu); float y=(float)(int)((h>>10)&0x3ffu);\n"
     "    if(x<y) acc=acc*0.5f + x; else if(x>y) acc=acc*0.5f - y; else acc=acc*0.25f;\n"
     "    if(acc>8192.0f||acc<-8192.0f) acc=acc*0.001f; }\n"
     "  return ("+t+")(int)acc; }\n",
     {0x12345u}, "OptStress271", 0},

    // Running min/max via branches (not the select idiom).
    {p+"_fminmax",
     t+" "+p+"_fminmax("+t+" a){ unsigned h=(unsigned)a; float mn=1e9f, mx=-1e9f; float acc=0.0f;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    float x=(float)((int)(h&0xffffu)-0x8000);\n"
     "    if(x<mn) mn=x; if(x>mx) mx=x; acc=acc*0.5f+(mx-mn)*0.001f; }\n"
     "  return ("+t+")(int)(mn+mx+acc); }\n",
     {0x23456u}, "OptStress271", 0},

    // fabs + negate (sign-bit / bitcast paths).
    {p+"_fabsneg",
     t+" "+p+"_fabsneg("+t+" a){ unsigned h=(unsigned)a; float acc=0.0f;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    float x=(float)((int)(h&0x7ffu)-0x400);\n"
     "    float ax=__builtin_fabsf(x); float nx=-x;\n"
     "    if(ax>nx) acc=acc*0.5f+ax; else acc=acc*0.5f+nx;\n"
     "    acc=acc - x*0.125f; }\n"
     "  return ("+t+")(int)acc; }\n",
     {0x34567u}, "OptStress271", 0},

    // ordered (<) vs unordered (!(>=)) branch with a NaN operand.
    {p+"_funord",
     t+" "+p+"_funord("+t+" a){ unsigned h=(unsigned)a; float acc=0.0f;\n"
     "  float qnan=__builtin_nanf(\"\");\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    float x=(float)(int)(h&0x3ffu);\n"
     "    float y=(h&0x10000u)?qnan:(float)(int)((h>>10)&0x3ffu);\n"
     "    if(x<y) acc+=1.0f; else acc-=0.5f;       /* x<NaN is false */\n"
     "    if(!(x>=y)) acc+=2.0f; else acc-=1.0f;   /* unordered: true on NaN */\n"
     "    acc=acc*0.5f + x*0.01f; }\n"
     "  return ("+t+")(int)acc; }\n",
     {0x45678u}, "OptStress271", 0},

    // clamp a double to [-100,100] via branches.
    {p+"_fclamp",
     t+" "+p+"_fclamp("+t+" a){ unsigned h=(unsigned)a; double acc=0.0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    double x=(double)((int)(h&0x1ffffu)-0x10000)*0.01;\n"
     "    if(x<-100.0) x=-100.0; else if(x>100.0) x=100.0;\n"
     "    acc=acc*0.5 + x; }\n"
     "  return ("+t+")(int)acc; }\n",
     {0x56789u}, "OptStress271", 0},

    // three-way sign-of-difference fed into an integer accumulator.
    {p+"_fcmp3",
     t+" "+p+"_fcmp3("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    float x=(float)((int)(h&0xfffu)-0x800);\n"
     "    float y=(float)((int)((h>>12)&0xfffu)-0x800);\n"
     "    int s = (x<y)?-1:((x>y)?1:0);\n"
     "    acc=acc*131 + s; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress271", 0},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress271TC("x64o271", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress271TC("x86o271", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress271TC("a64o271", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress271TC("armo271", "int");

INSTANTIATE_TEST_SUITE_P(OptStress271, X64OptStress271RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress271, X86OptStress271RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress271, A64OptStress271RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress271, ARM32OptStress271RT, ::testing::ValuesIn(kARM), rtTCName);
