//===- AllPlatform_OptStress69RTTests.cpp - single-precision FP -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Single-precision (`float`) counterparts of the #470 FP-return consume shapes.
// `float` returns occupy a narrower register slice (XMM0 low 32 / AArch64 S0 /
// ARM S0), and `float`/`double` conversions ride along the call, so any width
// or conversion gap in the FP-return modeling surfaces here even though the
// double forms (OptStress67/68) pass.
//
//   * f32cmp   - branch on a float helper's return (FP compare in a successor).
//   * f32acc   - float accumulator across a call (loop-carried FP return).
//   * f32tern  - acc += cond ? h(x) : x*0.5f  (float merged at a PHI).
//   * f32rec   - recursive float accumulation.
//   * f2d      - float helper return widened to double, accumulated.
//
// Each folds to one integer return; no libm, no 64-bit divide helper.
// All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress69RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress69RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress69RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress69RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress69RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress69RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress69RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress69RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress69TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Branch on a float helper's return (FP compare consumed after the call).
    {p+"_f32cmp",
     "static float "+p+"_h(float) __attribute__((noinline));\n"
     +t+" "+p+"_f32cmp("+t+" a){ unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    float r="+p+"_h((float)((s>>8)&0xff));\n"
     "    if(r>64.0f) h=h*131u+7u; else if(r<16.0f) h=h*131u+3u; else h=h*131u+1u; }\n"
     "  return ("+t+")h; }\n"
     "static float "+p+"_h(float x){ return x*0.5f + x*x*0.001f; }\n",
     {0x61u}, "OptStress69", 2},

    // Float accumulator across a call (loop-carried FP return + reset).
    {p+"_f32acc",
     "static float "+p+"_h(float,float) __attribute__((noinline));\n"
     +t+" "+p+"_f32acc("+t+" a){ unsigned s=(unsigned)a; float r=1.0f, tot=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    r="+p+"_h(r,(float)((s>>9)&0x3f)); tot+=r; if(r>1e6f) r=1.0f; }\n"
     "  return ("+t+")(long long)tot; }\n"
     "static float "+p+"_h(float a,float b){ return a*0.5f + b*0.25f + 1.0f; }\n",
     {0x62u}, "OptStress69", 2},

    // Float merged at a ternary PHI (one arm is the call result).
    {p+"_f32tern",
     "static float "+p+"_h(float) __attribute__((noinline));\n"
     +t+" "+p+"_f32tern("+t+" a){ unsigned s=(unsigned)a; float acc=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    float x=(float)((s>>8)&0xff);\n"
     "    acc += (s&1u)? "+p+"_h(x) : x*0.5f; }\n"
     "  return ("+t+")(long long)acc; }\n"
     "static float "+p+"_h(float x){ return x*x*0.01f + 1.0f; }\n",
     {0x63u}, "OptStress69", 2},

    // Recursive float accumulation: each level adds the recursive call's return.
    {p+"_f32rec",
     "static float "+p+"_rec(int,float) __attribute__((noinline));\n"
     +t+" "+p+"_f32rec("+t+" a){ unsigned s=(unsigned)a; float acc=0;\n"
     "  for(int i=0;i<60;i++){ s=s*1103515245u+12345u;\n"
     "    acc += "+p+"_rec((int)((s>>10)&7),(float)((s>>4)&0x1f)); }\n"
     "  return ("+t+")(long long)acc; }\n"
     "static float "+p+"_rec(int n,float x){ if(n<=0) return x;\n"
     "  return x*0.5f + "+p+"_rec(n-1, x*0.5f); }\n",
     {0x64u}, "OptStress69", 2},

    // Float helper return widened to double, accumulated (float/double mix).
    {p+"_f2d",
     "static float "+p+"_h(float) __attribute__((noinline));\n"
     +t+" "+p+"_f2d("+t+" a){ unsigned s=(unsigned)a; double acc=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    acc += (double)"+p+"_h((float)((s>>7)&0x7f)) * 1.5; }\n"
     "  return ("+t+")(long long)acc; }\n"
     "static float "+p+"_h(float x){ return x*0.25f + 2.0f; }\n",
     {0x65u}, "OptStress69", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress69TC("x64o69", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress69TC("x86o69", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress69TC("a64o69", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress69TC("armo69", "int");

INSTANTIATE_TEST_SUITE_P(OptStress69, X64OptStress69RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress69, X86OptStress69RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress69, A64OptStress69RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress69, ARM32OptStress69RT, ::testing::ValuesIn(kARM), rtTCName);
