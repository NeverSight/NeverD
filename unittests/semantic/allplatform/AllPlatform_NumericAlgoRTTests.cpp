//===- AllPlatform_NumericAlgoRTTests.cpp - DSP/numeric kernels -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// clang -O2 numeric/DSP kernel probes (1D convolution, sum-of-abs-diff, scale +
// narrow, saturating pack, rounding average, fixed-point rounding multiply).
// These are exactly what clang auto-vectorizes into widening multiplies
// (smull/umull/vmull), widening accumulates (saddw/uaddw), narrowing shifts
// (shrn/vshrn) and saturating narrows (sqxtn/vqmovn) — the highest-yield area
// for "narrowing/widening op lowered as a full-width placeholder" lift bugs.
//
// Local arrays exercise the stack frame; everything is bounded 16/32-bit with
// constant-divisor shifts only, so nothing lowers to a libcall Unicorn lacks.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64NumericAlgoRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64NumericAlgoRT, Verify) { roundTripX64(GetParam()); }

class A64NumericAlgoRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64NumericAlgoRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32NumericAlgoRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32NumericAlgoRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeNumTC(const char *prefix, const char *T,
                                          int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Q15 3-tap FIR convolution with saturation: i16*i16->i32 MAC, >>15, clamp
    // to i16.  Vectorizes to smull/vmull + shrn/vshrn + sqxtn/vqmovn.
    {p+"_conv1d",
     t+" "+p+"_conv1d("+t+" a) {\n"
     "  short x[40]; int acc=0;\n"
     "  for(int i=0;i<40;i++) x[i]=(short)((a*(i+1))&0xFFFF);\n"
     "  for(int i=2;i<40;i++){\n"
     "    int s=((int)x[i-2]*0x2000+(int)x[i-1]*0x4000+(int)x[i]*0x2000)>>15;\n"
     "    if(s>32767)s=32767; if(s<-32768)s=-32768;\n"
     "    acc+=s; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "NumericAlgo", opt, fl},

    // Sum of absolute differences over bytes: |a[i]-b[i]| accumulated.  Clang
    // vectorizes to uabd/vabd + widening add (uaddlp / psadbw).
    {p+"_sad8",
     t+" "+p+"_sad8("+t+" a) {\n"
     "  unsigned char x[64], y[64]; unsigned acc=0;\n"
     "  for(int i=0;i<64;i++){ x[i]=(unsigned char)(a*(i+1)); y[i]=(unsigned char)(a*(i+3)+i); }\n"
     "  for(int i=0;i<64;i++){ int d=(int)x[i]-(int)y[i]; acc+=(unsigned)(d<0?-d:d); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2233445ULL}, "NumericAlgo", opt, fl},

    // Scale then narrow with rounding: out = (u8)((in*scale + 128) >> 8).  Hits
    // widening multiply + rounding narrowing shift (rshrn/vrshrn).
    {p+"_scalenarrow",
     t+" "+p+"_scalenarrow("+t+" a) {\n"
     "  unsigned short in[48]; unsigned acc=0;\n"
     "  for(int i=0;i<48;i++) in[i]=(unsigned short)((a*(i+1))&0x3FF);\n"
     "  for(int i=0;i<48;i++){\n"
     "    unsigned v=((unsigned)in[i]*181u+128u)>>8;\n"
     "    acc=acc*131u+(v&0xFFu); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3344556ULL}, "NumericAlgo", opt, fl},

    // Saturating pack to signed-8: clamp each i16 to [-128,127], pack.  Hits
    // sqxtn/vqmovn / packsswb.
    {p+"_packsat",
     t+" "+p+"_packsat("+t+" a) {\n"
     "  short in[64]; unsigned acc=0;\n"
     "  for(int i=0;i<64;i++) in[i]=(short)((a*(i+1))&0x1FF)-256;\n"
     "  for(int i=0;i<64;i++){\n"
     "    int v=in[i]; if(v>127)v=127; if(v<-128)v=-128;\n"
     "    acc=(acc<<5)+(acc>>27)+(unsigned)(v&0xFF); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x4455667ULL}, "NumericAlgo", opt, fl},

    // Rounding halving average of two byte streams: (x+y+1)>>1.  Hits
    // urhadd/vrhadd / pavgb.
    {p+"_avground",
     t+" "+p+"_avground("+t+" a) {\n"
     "  unsigned char x[64], y[64]; unsigned acc=0;\n"
     "  for(int i=0;i<64;i++){ x[i]=(unsigned char)(a*(i+1)); y[i]=(unsigned char)(a*7+i*3); }\n"
     "  for(int i=0;i<64;i++){ unsigned avg=((unsigned)x[i]+(unsigned)y[i]+1u)>>1; acc^=avg<<((i&3)*8); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x5566778ULL}, "NumericAlgo", opt, fl},

    // Fixed-point rounding doubling multiply-high to i16: (2*a*b + 0x8000)>>16.
    // Hits sqrdmulh/vqrdmulh on NEON.
    {p+"_mulhrs",
     t+" "+p+"_mulhrs("+t+" a) {\n"
     "  short x[48], y[48]; int acc=0;\n"
     "  for(int i=0;i<48;i++){ x[i]=(short)((a*(i+1))&0xFFFF); y[i]=(short)((a*5-i)&0xFFFF); }\n"
     "  for(int i=0;i<48;i++){\n"
     "    int p=(2*(int)x[i]*(int)y[i]+0x8000)>>16;\n"
     "    if(p>32767)p=32767; if(p<-32768)p=-32768;\n"
     "    acc+=p; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x6677889ULL}, "NumericAlgo", opt, fl},

    // 3x3 integer matrix * vector, repeated (widening MAC, no narrowing).
    {p+"_matvec",
     t+" "+p+"_matvec("+t+" a) {\n"
     "  int acc=0;\n"
     "  for(int k=0;k<60;k++){\n"
     "    int m[9], v[3];\n"
     "    for(int i=0;i<9;i++) m[i]=(int)(a*(i+1)+k)%97-48;\n"
     "    for(int i=0;i<3;i++) v[i]=(int)(a*(i+2)+k*3)%51-25;\n"
     "    for(int r=0;r<3;r++){ int s=0; for(int c=0;c<3;c++) s+=m[r*3+c]*v[c]; acc^=s; } }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x778899AULL}, "NumericAlgo", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Num =
    makeNumTC("x64n", "long", 2, "");
static const std::vector<RoundTripTC> kA64Num =
    makeNumTC("a64n", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Num =
    makeNumTC("armn", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(NumericAlgo, X64NumericAlgoRT,
                         ::testing::ValuesIn(kX64Num), rtTCName);
INSTANTIATE_TEST_SUITE_P(NumericAlgo, A64NumericAlgoRT,
                         ::testing::ValuesIn(kA64Num), rtTCName);
INSTANTIATE_TEST_SUITE_P(NumericAlgo, ARM32NumericAlgoRT,
                         ::testing::ValuesIn(kARM32Num), rtTCName);
