//===- AllPlatform_CondLoopRTTests.cpp - branchless cond loops ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Loop-carried, branchless conditional idioms.  clang -O2 lowers each per-
// iteration branch into conditional-move / conditional-select chains that read
// flags produced earlier in the same block (x86 cmov/bt, AArch64 csel/csinc/
// csneg/ccmp, ARM32 predication).  This is the historically most fragile area
// of the MedIR flag optimizer (#147-149, #161, #222, #253-#254 runones), so a
// wrong flag source / stale carry silently corrupts the loop-carried value.
//
// Every function folds to an exact integer return value (32-bit arithmetic;
// ARM32 builds for cortex-a15 so `/` and `%` stay hardware instructions).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64CondLoopRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64CondLoopRT, Verify) { roundTripX64(GetParam()); }

class A64CondLoopRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64CondLoopRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32CondLoopRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32CondLoopRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeCondTC(const char *prefix, const char *T,
                                           int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Double clamp: lo/hi cmov chain feeding a running sum (two compares/block).
    {p+"_clampsum",
     t+" "+p+"_clampsum("+t+" a) {\n"
     "  int s=0;\n"
     "  for (int i=0;i<128;i++){ int v=(int)(a*(i+1))%1000; int lo=-200, hi=300;\n"
     "    if (v<lo) v=lo; if (v>hi) v=hi; s+=v; }\n"
     "  return s;\n"
     "}\n",
     {0x1234567ULL}, "CondLoop", opt, fl},

    // Three-way sign accumulate: (x>0)-(x<0) per iteration (compound flags).
    {p+"_sgnsum",
     t+" "+p+"_sgnsum("+t+" a) {\n"
     "  int s=0;\n"
     "  for (int i=0;i<150;i++){ int x=(int)(a*(i+3)) - (int)(a*97);\n"
     "    s += (x>0) - (x<0); }\n"
     "  return s;\n"
     "}\n",
     {0x2233445ULL}, "CondLoop", opt, fl},

    // Running max + count of new maxima (cmov + conditional increment, runones
    // class: a `cmp` feeding both a select and a predicated increment).
    {p+"_runmax",
     t+" "+p+"_runmax("+t+" a) {\n"
     "  int best=-2000000000, cnt=0;\n"
     "  for (int i=0;i<128;i++){ int v=(int)(a*(i*7+1));\n"
     "    if (v>best){ best=v; cnt++; } }\n"
     "  return best + cnt*131;\n"
     "}\n",
     {0x3344556ULL}, "CondLoop", opt, fl},

    // Saturating signed-8 accumulate (double bound clamp, narrow type).
    {p+"_satacc8",
     t+" "+p+"_satacc8("+t+" a) {\n"
     "  int s=0;\n"
     "  for (int i=0;i<200;i++){ int v=(int)(a*(i+1))&0xFF; if(v>127)v-=256;\n"
     "    int acc=s+v; if(acc>127)acc=127; if(acc<-128)acc=-128; s=acc; }\n"
     "  return s;\n"
     "}\n",
     {0x4455667ULL}, "CondLoop", opt, fl},

    // Conditional negate by parity (AArch64 csneg / ARM rsbmi / x86 cmov+neg).
    {p+"_condneg",
     t+" "+p+"_condneg("+t+" a) {\n"
     "  int s=0;\n"
     "  for (int i=0;i<160;i++){ int x=(int)(a*(i+1))%777;\n"
     "    s += (i&1) ? -x : x; }\n"
     "  return s;\n"
     "}\n",
     {0x5566778ULL}, "CondLoop", opt, fl},

    // Compound AND condition counting (range membership → BOOL_AND of flags).
    {p+"_twocond",
     t+" "+p+"_twocond("+t+" a) {\n"
     "  int s=0;\n"
     "  for (int i=0;i<256;i++){ int x=(int)(a*(i+1))%1000 - 500;\n"
     "    if (x>-100 && x<100) s += x; else s -= 1; }\n"
     "  return s;\n"
     "}\n",
     {0x6677889ULL}, "CondLoop", opt, fl},

    // Branchless absolute difference sum (abs idiom: mask = x>>31; (x^m)-m).
    {p+"_absdiff",
     t+" "+p+"_absdiff("+t+" a) {\n"
     "  int s=0;\n"
     "  for (int i=0;i<200;i++){ int x=(int)(a*(i+1)), y=(int)(a*(i+2));\n"
     "    int d=x-y; if(d<0)d=-d; s += d>>3; }\n"
     "  return s;\n"
     "}\n",
     {0x778899AULL}, "CondLoop", opt, fl},

    // Track min and max simultaneously (two cmov chains sharing the cmp block).
    {p+"_minmax",
     t+" "+p+"_minmax("+t+" a) {\n"
     "  int mn=2000000000, mx=-2000000000;\n"
     "  for (int i=0;i<140;i++){ int v=(int)(a*(i*13+5));\n"
     "    if (v<mn) mn=v; if (v>mx) mx=v; }\n"
     "  return (mx>>4) - (mn>>4);\n"
     "}\n",
     {0x88990ABULL}, "CondLoop", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Cond =
    makeCondTC("x64cl", "long", 2, "");
static const std::vector<RoundTripTC> kA64Cond =
    makeCondTC("a64cl", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Cond =
    makeCondTC("armcl", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(CondLoop, X64CondLoopRT,
                         ::testing::ValuesIn(kX64Cond), rtTCName);
INSTANTIATE_TEST_SUITE_P(CondLoop, A64CondLoopRT,
                         ::testing::ValuesIn(kA64Cond), rtTCName);
INSTANTIATE_TEST_SUITE_P(CondLoop, ARM32CondLoopRT,
                         ::testing::ValuesIn(kARM32Cond), rtTCName);
