//===- AllPlatform_OptStress258RTTests.cpp - scalar FP at -O0 ============//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Scalar floating point at -O0 — a coverage gap: the historical FP probes are
// almost all -O2 / auto-vectorized, while -O0 emits very different code (i386
// x87 fld/fstp stack traffic, ARM32 VFP, x64/AArch64 scalar SSE/NEON), storing
// every intermediate back to the frame instead of keeping it in a register.
// That store-everything, explicit-convert form is where scalar-FP lift bugs
// (x87 stack modeling, cvt width, ordered/unordered compare) hide.
//
//   * fadd    - float add/sub/mul-by-const, bounded recurrence -> int.
//   * fmuldiv - float multiply and divide (nonzero divisor).
//   * dmix    - double arithmetic, wider mantissa.
//   * fcmpsel - ordered min/max + an unordered (NaN-constant) compare select.
//   * frnd    - truncation / round-half / floor via (int) casts.
//   * fcvt    - int -> float -> double -> int conversion round trips.
//
// Integer in / integer out (FP folded to one integer return), LCG-seeded, all
// four targets, -O0.  Bounded recurrences keep the float magnitudes small so
// (int) truncation stays well-defined; only float/double + signed int casts, so
// i386/ARM32 stay libcall-free (no 64-bit<->FP, no long double).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress258RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress258RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress258RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress258RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress258RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress258RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress258RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress258RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress258TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // float add/sub/mul-by-const, bounded recurrence folded to int.
    {p+"_fadd",
     t+" "+p+"_fadd("+t+" a){ unsigned h=(unsigned)a; float acc=0.0f;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    float x=(float)(int)(h&0x3ffu); float y=(float)(int)((h>>10)&0xffu);\n"
     "    acc=acc*0.5f + x - y*0.25f; }\n"
     "  return ("+t+")(int)acc; }\n",
     {0x12345u}, "OptStress258", 0},

    // float multiply and divide (divisor forced nonzero).
    {p+"_fmuldiv",
     t+" "+p+"_fmuldiv("+t+" a){ unsigned h=(unsigned)a; float acc=1.0f;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    float x=(float)(int)((h&0x7fu)+1); float y=(float)(int)(((h>>7)&0x3fu)+1);\n"
     "    acc=acc*1.5f; acc=acc + x/y - x*0.125f; if(acc>4096.0f) acc=acc*0.001f; }\n"
     "  return ("+t+")(int)acc; }\n",
     {0x23456u}, "OptStress258", 0},

    // double arithmetic, wider mantissa.
    {p+"_dmix",
     t+" "+p+"_dmix("+t+" a){ unsigned h=(unsigned)a; double acc=0.0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    double x=(double)(int)(h&0xffffu); double y=(double)(int)((h>>16)&0xffu);\n"
     "    acc=acc*0.5 + x*0.125 - y + (double)(int)(h&1u)*3.0; }\n"
     "  return ("+t+")(int)acc; }\n",
     {0x34567u}, "OptStress258", 0},

    // ordered min/max + an unordered (NaN-constant) compare select.
    {p+"_fcmpsel",
     t+" "+p+"_fcmpsel("+t+" a){ unsigned h=(unsigned)a; float acc=0.0f;\n"
     "  float qnan=__builtin_nanf(\"\");\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    float x=(float)(int)(h&0x1ffu); float y=(float)(int)((h>>9)&0x1ffu);\n"
     "    float mn=(x<y)?x:y; float mx=(x>y)?x:y;\n"
     "    unsigned uno=(x!=qnan)?1u:0u; /* unordered: always true */\n"
     "    acc=acc*0.5f + mn - mx*0.25f + (float)(int)uno; }\n"
     "  return ("+t+")(int)acc; }\n",
     {0x45678u}, "OptStress258", 0},

    // truncation / round-half / floor via (int) casts.
    {p+"_frnd",
     t+" "+p+"_frnd("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    float x=(float)(int)(h&0xfffu) * 0.3333333f;\n"
     "    int tr=(int)x; int rh=(int)(x+0.5f); int fl=(int)(x-0.5f);\n"
     "    acc=acc*131 + tr + rh*3 + fl; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress258", 0},

    // int -> float -> double -> int conversion round trips.
    {p+"_fcvt",
     t+" "+p+"_fcvt("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    int v=(int)(h&0x7fffu)-0x4000;\n"
     "    float f=(float)v; double d=(double)f * 1.25; float g=(float)d;\n"
     "    acc=acc*131 + (int)f + (int)d + (int)g; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress258", 0},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress258TC("x64o258", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress258TC("x86o258", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress258TC("a64o258", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress258TC("armo258", "int");

INSTANTIATE_TEST_SUITE_P(OptStress258, X64OptStress258RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress258, X86OptStress258RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress258, A64OptStress258RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress258, ARM32OptStress258RT, ::testing::ValuesIn(kARM), rtTCName);
