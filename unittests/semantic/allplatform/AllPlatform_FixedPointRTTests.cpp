//===- AllPlatform_FixedPointRTTests.cpp - fixed-point DSP algos -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// clang -O2 fixed-point / DSP algorithm probes.  Q-format math packs 64-bit
// widening multiplies (smull/smlal on ARM, imul/EDX on x86, smull/smulh on
// AArch64), constant 64-bit shifts, magic-number divides, saturation cmov/csel
// chains, and loop-carried feedback state — each lowered very differently per
// target, which stresses the lifter's widening-multiply / high-half extraction
// paths and the optimizer's loop-carried-value and sub-register tracking.
//
// All 64-bit shifts use *constant* amounts and every 64-bit product comes from
// 32-bit operands (sext/zext) so nothing lowers to a runtime helper Unicorn
// lacks (no __aeabi_lasr/__divdi3/__muldi3).  Each function loops so all paths
// run and folds to an exact integer return value.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64FixedPointRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64FixedPointRT, Verify) { roundTripX64(GetParam()); }

class A64FixedPointRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64FixedPointRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32FixedPointRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32FixedPointRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeFixedTC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Q16.16 multiply: (int)(((int64)x * y) >> 16).  32x32->64 widening mul
    // (smull / imul / smull) + constant 64-bit arithmetic shift + truncate.
    {p+"_q16mul",
     t+" "+p+"_q16mul("+t+" a) {\n"
     "  int acc=0;\n"
     "  for (int i=1;i<=100;i++){\n"
     "    int x=(int)(a*i)+0x12000; int y=(int)(a-i*3)-0x8000;\n"
     "    long long pr=(long long)x*y;\n"
     "    acc += (int)(pr>>16); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "FixedPoint", opt, fl},

    // Q16.16 dot product: 64-bit loop-carried accumulator fed by smlal.
    // Exercises mul-accumulate into a 64-bit register pair on ARM32.
    {p+"_q16dot",
     t+" "+p+"_q16dot("+t+" a) {\n"
     "  long long acc=0;\n"
     "  for (int i=1;i<=64;i++){\n"
     "    int x=(int)(a+i)*3; int y=(int)(a-i)*5+0x10000;\n"
     "    acc += (long long)x*y; }\n"
     "  return ("+t+")(acc>>20);\n"
     "}\n",
     {0x2233445ULL}, "FixedPoint", opt, fl},

    // Q15 saturating multiply to signed-16 range (cmov/csel saturation).
    {p+"_q15sat",
     t+" "+p+"_q15sat("+t+" a) {\n"
     "  int acc=0;\n"
     "  for (int i=1;i<=80;i++){\n"
     "    int x=((int)(a*i)&0xFFFF)-0x8000;\n"
     "    int y=((int)(a*7-i)&0xFFFF)-0x8000;\n"
     "    int pr=(x*y)>>15;\n"
     "    if (pr>32767) pr=32767; if (pr<-32768) pr=-32768;\n"
     "    acc += pr; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3344556ULL}, "FixedPoint", opt, fl},

    // Chained 8-bit linear interpolation: v += ((target-v)*t)>>8 (loop-carried).
    {p+"_lerp",
     t+" "+p+"_lerp("+t+" a) {\n"
     "  int v=(int)a&0xFFFF;\n"
     "  for (int i=1;i<=120;i++){\n"
     "    int target=(int)(a*i)&0xFFFF; int tt=i&0xFF;\n"
     "    v = v + (((target-v)*tt)>>8); }\n"
     "  return ("+t+")v;\n"
     "}\n",
     {0x4455667ULL}, "FixedPoint", opt, fl},

    // RGBA alpha blend: per-channel (sc*a + dc*(255-a) + 127)/255 with byte
    // pack/unpack.  The /255 lowers to a magic multiply; bytes are shifted in
    // and out (sub-register / mask stress).
    {p+"_rgba",
     t+" "+p+"_rgba("+t+" a) {\n"
     "  unsigned acc=0;\n"
     "  for (int i=0;i<64;i++){\n"
     "    unsigned src=(unsigned)(a*(i+1));\n"
     "    unsigned dst=(unsigned)(a^(unsigned)(i*0x01010101));\n"
     "    unsigned al=(unsigned)(i*4)&0xFF; unsigned out=0;\n"
     "    for (int c=0;c<4;c++){\n"
     "      unsigned sc=(src>>(c*8))&0xFF; unsigned dc=(dst>>(c*8))&0xFF;\n"
     "      unsigned bl=(sc*al + dc*(255-al) + 127)/255u;\n"
     "      out |= (bl&0xFF)<<(c*8); }\n"
     "    acc ^= out; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x5566778ULL}, "FixedPoint", opt, fl},

    // First-order IIR low-pass: y += ((x-y)*alpha)>>8 (loop-carried feedback).
    // %10000 is a magic-number modulo; the feedback defeats vectorization.
    {p+"_iir",
     t+" "+p+"_iir("+t+" a) {\n"
     "  int y=0,out=0;\n"
     "  for (int i=0;i<200;i++){\n"
     "    int x=(int)((unsigned)(a*(i+1))%10000)-5000;\n"
     "    y = y + (((x-y)*40)>>8); out ^= y; }\n"
     "  return ("+t+")out;\n"
     "}\n",
     {0x6677889ULL}, "FixedPoint", opt, fl},

    // Fixed-point Horner polynomial evaluation in Q8.8.
    {p+"_horner",
     t+" "+p+"_horner("+t+" a) {\n"
     "  int acc=0;\n"
     "  for (int i=1;i<=60;i++){\n"
     "    int x=((int)(a*i)&0xFFFF)-0x8000;\n"
     "    int r=0x40;\n"
     "    r=((r*x)>>8)+0x120; r=((r*x)>>8)-0x80; r=((r*x)>>8)+0x300;\n"
     "    acc += r; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x778899AULL}, "FixedPoint", opt, fl},

    // High 32 bits of a 32x32 product: (int)(((int64)x*y)>>32).  Exercises the
    // smulh / imul-EDX / smull-high-half extraction path explicitly.
    {p+"_mulhi",
     t+" "+p+"_mulhi("+t+" a) {\n"
     "  int acc=0;\n"
     "  for (int i=1;i<=100;i++){\n"
     "    int x=(int)(a*i)^0x5A5A5A5A; int y=(int)(a+i*131)^0x3C3C3C3C;\n"
     "    acc += (int)(((long long)x*y)>>32); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x88990ABULL}, "FixedPoint", opt, fl},

    // FIR (no feedback): clang -O2 vectorizes the sliding-window 64-bit MAC into
    // sshll/saddw widen + shrn narrow on AArch64 (vmull.s32 + vshrn on ARM32),
    // exercising the narrowing right-shift lift on both targets.
    {p+"_bqfir",
     t+" "+p+"_bqfir("+t+" a) {\n"
     "  int x1=0,x2=0,out=0;\n"
     "  for(int i=0;i<128;i++){\n"
     "    int x0=(int)((unsigned)(a*(i+1))%4096)-2048;\n"
     "    long long ac=(long long)x0*0x800+(long long)x1*0x1000+(long long)x2*0x800;\n"
     "    int y0=(int)(ac>>12);\n"
     "    x2=x1; x1=x0; out^=y0; }\n"
     "  return ("+t+")out;\n"
     "}\n",
     {0x99AABBCULL}, "FixedPoint", opt, fl},

    // FIR with a NEGATIVE coefficient: forces the umull + signed-correction
    // lowering and (on AArch64) the SHRN narrowing right shift.
    {p+"_bqneg",
     t+" "+p+"_bqneg("+t+" a) {\n"
     "  int x1=0,x2=0,out=0;\n"
     "  for(int i=0;i<128;i++){\n"
     "    int x0=(int)((unsigned)(a*(i+1))%4096)-2048;\n"
     "    long long ac=(long long)x0*0x800-(long long)x1*0x1800+(long long)x2*0x600;\n"
     "    int y0=(int)(ac>>12);\n"
     "    x2=x1; x1=x0; out^=y0; }\n"
     "  return ("+t+")out;\n"
     "}\n",
     {0x99AABBCULL}, "FixedPoint", opt, fl},

    // First-order IIR: the y0 feedback prevents vectorization, so this stays a
    // scalar 64-bit MAC with register-pressure spilling (exercises frame sizing).
    {p+"_bqfb",
     t+" "+p+"_bqfb("+t+" a) {\n"
     "  int y1=0,out=0;\n"
     "  for(int i=0;i<128;i++){\n"
     "    int x0=(int)((unsigned)(a*(i+1))%4096)-2048;\n"
     "    long long ac=(long long)x0*0x800-(long long)y1*0x1800;\n"
     "    int y0=(int)(ac>>12);\n"
     "    y1=y0; out^=y0; }\n"
     "  return ("+t+")out;\n"
     "}\n",
     {0x99AABBCULL}, "FixedPoint", opt, fl},

    // Biquad (direct form I): four loop-carried states + 64-bit Q12 MAC chain
    // of five smull/smlal terms.  The high register pressure forces an argument
    // spill, stressing frame-size accuracy and the ARM push/pop base lift.
    {p+"_biquad",
     t+" "+p+"_biquad("+t+" a) {\n"
     "  int x1=0,x2=0,y1=0,y2=0,out=0;\n"
     "  for (int i=0;i<128;i++){\n"
     "    int x0=(int)((unsigned)(a*(i+1))%4096)-2048;\n"
     "    long long ac=(long long)x0*0x800 + (long long)x1*0x1000\n"
     "               + (long long)x2*0x800 - (long long)y1*0x1800\n"
     "               + (long long)y2*0x600;\n"
     "    int y0=(int)(ac>>12);\n"
     "    x2=x1; x1=x0; y2=y1; y1=y0; out ^= y0; }\n"
     "  return ("+t+")out;\n"
     "}\n",
     {0x99AABBCULL}, "FixedPoint", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Fixed =
    makeFixedTC("x64fx", "long", 2, "");
static const std::vector<RoundTripTC> kA64Fixed =
    makeFixedTC("a64fx", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Fixed =
    makeFixedTC("armfx", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(FixedPoint, X64FixedPointRT,
                         ::testing::ValuesIn(kX64Fixed), rtTCName);
INSTANTIATE_TEST_SUITE_P(FixedPoint, A64FixedPointRT,
                         ::testing::ValuesIn(kA64Fixed), rtTCName);
INSTANTIATE_TEST_SUITE_P(FixedPoint, ARM32FixedPointRT,
                         ::testing::ValuesIn(kARM32Fixed), rtTCName);
