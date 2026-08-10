//===- AllPlatform_FMARoundingRTTests.cpp - fused multiply-add rounding --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// All-platform roundtrip probing of fused multiply-add (single-rounding)
// semantics.  `__builtin_fmaf`/`__builtin_fma` lower to a hardware FMA that
// rounds the a*b+c result exactly ONCE:
//
//   * x86 (FMA3)  VFMADD###SS / VFMADD###SD
//   * AArch64     FMADD / FNMADD / FMSUB / FNMSUB
//   * ARM32 VFPv4 VFMA.F32 / VFMS.F32 / VFNMA / VFNMS
//
// A lifter that models FMA as a separate multiply + add rounds TWICE, so the
// low mantissa bit diverges for many operand pairs.  Each kernel runs a serial
// FMA chain over LCG-seeded operands and folds the *bit pattern* of the running
// FP result every iteration, so a single double-rounding step is caught by the
// final integer compare.  Operands are bounded and the accumulator is rescaled
// to avoid overflow/inf.
//
// x86 needs -mfma; ARM32's default cortex-a15 already carries VFPv4 (VFMA), run
// under the MAX CPU model.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64FMARoundingRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64FMARoundingRT, Verify) { roundTripX64(GetParam()); }
class X86FMARoundingRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86FMARoundingRT, Verify) { roundTripX86(GetParam()); }
class A64FMARoundingRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64FMARoundingRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32FMARoundingRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32FMARoundingRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeFMATC(const char *prefix, const char *T,
                                          const char *flags,
                                          const char *targetOverride, int ucCpu) {
  std::string p = prefix, t = T, fl = flags, tov = targetOverride;
  return {
    // float FMA chain: s = fmaf(x,y,s), bit-accumulated -> catches double round.
    {p+"_fmaf_chain",
     t+" "+p+"_fmaf_chain("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0; float s=1.0f;\n"
     "  for(int i=0;i<64;i++){ h=h*1103515245u+12345u;\n"
     "    float x=(float)(int)(h&0xFFFF)*0.000123f-2.0f;\n"
     "    float y=(float)(int)((h>>8)&0xFFFF)*0.000071f+1.0f;\n"
     "    s=__builtin_fmaf(x,y,s);\n"
     "    if(s>1e6f||s<-1e6f) s*=0.0009765625f;\n"
     "    unsigned b; __builtin_memcpy(&b,&s,4); acc=acc*131u+b; }\n"
     "  return ("+t+")acc; }\n",
     {0x1234u}, "FMARounding", 2, fl, false, tov, ucCpu},

    // float negated-FMA: s = fmaf(x,y,-s) (FNMADD/VFNMS path).
    {p+"_fnmaf",
     t+" "+p+"_fnmaf("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0; float s=1.0f;\n"
     "  for(int i=0;i<64;i++){ h=h*1103515245u+12345u;\n"
     "    float x=(float)(int)(h&0xFFFF)*0.000091f-1.5f;\n"
     "    float y=(float)(int)((h>>8)&0xFFFF)*0.000059f+1.0f;\n"
     "    s=__builtin_fmaf(x,y,-s);\n"
     "    if(s>1e6f||s<-1e6f) s*=0.0009765625f;\n"
     "    unsigned b; __builtin_memcpy(&b,&s,4); acc=acc*131u+b; }\n"
     "  return ("+t+")acc; }\n",
     {0x2345u}, "FMARounding", 2, fl, false, tov, ucCpu},

    // float Horner polynomial: p = fmaf(p,x,c_i) (the classic FMA use).
    {p+"_horner",
     t+" "+p+"_horner("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int k=0;k<48;k++){ h=h*1103515245u+12345u;\n"
     "    float x=(float)(int)(h&0x7FFF)*0.00003f-0.5f; float pp=0.7f;\n"
     "    pp=__builtin_fmaf(pp,x,-0.31f); pp=__builtin_fmaf(pp,x,0.17f);\n"
     "    pp=__builtin_fmaf(pp,x,-0.09f); pp=__builtin_fmaf(pp,x,1.0f);\n"
     "    unsigned b; __builtin_memcpy(&b,&pp,4); acc=acc*131u+b; }\n"
     "  return ("+t+")acc; }\n",
     {0x3456u}, "FMARounding", 2, fl, false, tov, ucCpu},

    // double FMA chain: s = fma(x,y,s), bit-accumulated (i386/ARM32 use low 32b).
    {p+"_fma_chain",
     t+" "+p+"_fma_chain("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0; double s=1.0;\n"
     "  for(int i=0;i<64;i++){ h=h*1103515245u+12345u;\n"
     "    double x=(double)(int)(h&0x1FFFF)*0.0000131-3.0;\n"
     "    double y=(double)(int)((h>>8)&0x1FFFF)*0.0000071+1.0;\n"
     "    s=__builtin_fma(x,y,s);\n"
     "    if(s>1e9||s<-1e9) s*=0.0009765625;\n"
     "    unsigned long bb; __builtin_memcpy(&bb,&s,8);\n"
     "    acc=acc*131u+(unsigned)bb+(unsigned)(bb>>32); }\n"
     "  return ("+t+")acc; }\n",
     {0x4567u}, "FMARounding", 2, fl, false, tov, ucCpu},

    // Catastrophic cancellation that only a single-rounding FMA preserves:
    // fmaf(x,y,-round(x*y)) is the exact product's rounding residual (nonzero);
    // a double-rounding mul+add lowering collapses it to 0.  Directly fails if
    // the lifter ever models FMA as a separate multiply + add.
    {p+"_cancel",
     t+" "+p+"_cancel("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<64;i++){ h=h*1103515245u+12345u;\n"
     "    float x=1.0f+(float)(int)(h&0x3FF)*1.1920929e-7f;\n"
     "    float y=1.0f+(float)(int)((h>>10)&0x3FF)*1.1920929e-7f;\n"
     "    float prod=x*y; float r=__builtin_fmaf(x,y,-prod);\n"
     "    unsigned b; __builtin_memcpy(&b,&r,4); acc=acc*131u+b; }\n"
     "  return ("+t+")acc; }\n",
     {0x99u}, "FMARounding", 2, fl, false, tov, ucCpu},

    // float dot-product reduction folded with FMA (data-dependent chain).
    {p+"_dot",
     t+" "+p+"_dot("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0; float s=0.0f;\n"
     "  for(int i=0;i<80;i++){ h=h*1103515245u+12345u;\n"
     "    float x=(float)(int)(h&0xFFF)*0.0007f-1.0f;\n"
     "    float y=(float)(int)((h>>12)&0xFFF)*0.0005f-1.0f;\n"
     "    s=__builtin_fmaf(x,y,s);\n"
     "    if(s>5e5f||s<-5e5f) s*=0.5f;\n"
     "    unsigned b; __builtin_memcpy(&b,&s,4); acc=acc*131u+b; }\n"
     "  return ("+t+")acc; }\n",
     {0x5678u}, "FMARounding", 2, fl, false, tov, ucCpu},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 =
    makeFMATC("x64fma", "long", "-mfma -fno-math-errno", "", -1);
static const std::vector<RoundTripTC> kX86 =
    makeFMATC("x86fma", "int", "-mfma -fno-math-errno", "", -1);
static const std::vector<RoundTripTC> kA64 =
    makeFMATC("a64fma", "long", "-fno-math-errno", "", -1);
static const std::vector<RoundTripTC> kARM =
    makeFMATC("armfma", "int", "-mfpu=neon-vfpv4 -fno-math-errno", "",
              UC_CPU_ARM_MAX);

INSTANTIATE_TEST_SUITE_P(FMARounding, X64FMARoundingRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(FMARounding, X86FMARoundingRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(FMARounding, A64FMARoundingRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(FMARounding, ARM32FMARoundingRT, ::testing::ValuesIn(kARM), rtTCName);
