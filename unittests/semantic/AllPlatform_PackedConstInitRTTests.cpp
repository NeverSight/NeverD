//===- AllPlatform_PackedConstInitRTTests.cpp - SIMD const-init ---*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for packed SIMD lanes built from runtime-seeded vectors and
// .rodata vector constants — the class the todo flagged as "x86 FP vector const
// init / AArch64 FP vector / ARM32 NEON VBIC/VSHR" still-open limitations.  Each
// kernel uses clang's vector extensions so one source lowers to SSE on x86/i386,
// NEON on AArch64/ARM32: per-lane min/max/abs (UMIN/SMAX/PMINUB/PMAXSW), a
// compare->mask->blend, packed bit-clear + variable/const shifts (VBIC/VSHR/
// PSRL), packed multiply-add, and a float vector add/mul/min/max reduced to a
// scalar.  The vector seeds derive from the argument so clang cannot fold them
// away, and a .rodata constant vector is mixed in to exercise the const-init
// load path.  Each folds the lanes into a value-dependent hash, compared native
// vs lifted at -O2 on all four targets (integer return; FP stays in-vector so no
// ARM32 soft-float libcall).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64PackRT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64PackRT, Verify) { roundTripX64(GetParam()); }
class X86PackRT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86PackRT, Verify) { roundTripX86(GetParam()); }
class A64PackRT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64PackRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32PackRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32PackRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makePackTC(const char *prefix, const char *T,
                                           const char *extra) {
  std::string p = prefix, t = T, e = extra;
  return {
    // Per-lane unsigned/signed min & max over 4x32 vectors (PMINUD/PMAXSD /
    // UMIN/SMAX), one operand a .rodata constant vector.
    {p+"_minmax",
     "typedef unsigned vu4 __attribute__((vector_size(16)));\n"
     "typedef int vs4 __attribute__((vector_size(16)));\n"
     +t+" "+p+"_minmax("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u;\n"
     "  vu4 v={s,s*2654435761u,s^0x9E3779B9u,s+0x12345u};\n"
     "  vu4 k={0x10000u,0xFFFFFFFFu,0x7FFFu,0x80000000u};\n"
     "  vu4 lo=__builtin_elementwise_min(v,k), hi=__builtin_elementwise_max(v,k);\n"
     "  vs4 sv=(vs4)v, sk=(vs4)k; vs4 smn=__builtin_elementwise_min(sv,sk);\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<4;i++) h=h*131u+lo[i]+hi[i]*7u+(unsigned)smn[i]*3u;\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x41ULL}, "Pack", 2, e},

    // Per-lane absolute value over 4x32 signed lanes (PABSD / ABS).
    {p+"_abs",
     "typedef int vs4 __attribute__((vector_size(16)));\n"
     +t+" "+p+"_abs("+t+" a){\n"
     "  int s=(int)a|1;\n"
     "  vs4 v={s,-s*3,s^0x55555555,-(s+1)};\n"
     "  vs4 av=__builtin_elementwise_abs(v);\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<4;i++) h=h*131u+(unsigned)av[i]+(unsigned)(i*7);\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x53ULL}, "Pack", 2, e},

    // Compare -> mask -> blend over 8x16 lanes (PCMPGTW + blend / CMGT+BSL).
    {p+"_blend",
     "typedef unsigned short vu8 __attribute__((vector_size(16)));\n"
     +t+" "+p+"_blend("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u;\n"
     "  vu8 v={(unsigned short)s,(unsigned short)(s>>1),(unsigned short)(s*3u),\n"
     "    (unsigned short)(s^0xBEEFu),(unsigned short)(s+7u),(unsigned short)(s*5u),\n"
     "    (unsigned short)(s>>3),(unsigned short)(s|0x100u)};\n"
     "  vu8 k={0x1000,0x2000,0x3000,0x4000,0x5000,0x6000,0x7000,0x8000};\n"
     "  vu8 gt=(vu8)(v>k); vu8 r=(v & gt) | ((vu8)(k-v) & ~gt);\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<8;i++) h=h*131u+(unsigned)r[i]+(unsigned)i;\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x67ULL}, "Pack", 2, e},

    // Packed bit-clear (v & ~k = VBIC) + const and variable right shifts
    // (PSRLW/VSHR), the ARM32 VBIC/VSHR class.
    {p+"_bicshift",
     "typedef unsigned vu4 __attribute__((vector_size(16)));\n"
     +t+" "+p+"_bicshift("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u;\n"
     "  vu4 v={s,s*2654435761u,s^0x9E3779B9u,s+0x12345u};\n"
     "  vu4 m={0xF0F0F0F0u,0x00FF00FFu,0xFFFF0000u,0x0F0F0F0Fu};\n"
     "  vu4 bic=v & ~m;\n"
     "  vu4 sr=bic>>3; vu4 sl=v<<5;\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<4;i++) h=h*131u+bic[i]+sr[i]*7u+sl[i]*3u;\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x71ULL}, "Pack", 2, e},

    // Packed multiply + add over 4x32 lanes (PMULLD + PADDD / MLA).
    {p+"_muladd",
     "typedef unsigned vu4 __attribute__((vector_size(16)));\n"
     +t+" "+p+"_muladd("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u;\n"
     "  vu4 v={s,s*3u,s*5u,s*7u};\n"
     "  vu4 w={11u,13u,17u,19u};\n"
     "  vu4 c={0x100u,0x200u,0x300u,0x400u};\n"
     "  vu4 r=v*w+c;\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<4;i++) h=h*131u+r[i]+(unsigned)i;\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x29ULL}, "Pack", 2, e},

    // Float vector add/mul/min/max reduced to a scalar bit pattern (the AArch64
    // FP-vector / x86 packed-FP const-init path).  No NaN inputs so the value is
    // a plain ordered result; the bit pattern still pins lane order/precision.
    {p+"_fp",
     "typedef float vf4 __attribute__((vector_size(16)));\n"
     +t+" "+p+"_fp("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u;\n"
     "  vf4 v={(float)(s&0xFFu),(float)((s>>8)&0xFFu)+0.5f,\n"
     "    (float)((s>>16)&0xFFu)*1.25f,(float)((s>>24)&0xFFu)-3.0f};\n"
     "  vf4 k={1.5f,2.25f,3.125f,4.0625f};\n"
     "  vf4 sum=v*k+k; vf4 dif=v*k-k; vf4 prod=sum*dif+v;\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<4;i++){ float r=prod[i]+sum[i]*0.5f+dif[i];\n"
     "    unsigned b; __builtin_memcpy(&b,&r,4); h=h*131u+b+(unsigned)i; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x9CULL}, "Pack", 2, e},
  };
}
// clang-format on

// i386 needs SSE2 for the 128-bit vector path (its default is x87 + no SSE);
// the other targets use their hardware-SIMD default (SSE on x86_64, NEON on
// AArch64 / cortex-a15 ARM32).
static const std::vector<RoundTripTC> kX64 = makePackTC("x64pk", "long", "");
static const std::vector<RoundTripTC> kX86 =
    makePackTC("x86pk", "int", "-msse2 -mfpmath=sse");
static const std::vector<RoundTripTC> kA64 = makePackTC("a64pk", "long", "");
static const std::vector<RoundTripTC> kARM =
    makePackTC("armpk", "int", "-mfpu=neon");

INSTANTIATE_TEST_SUITE_P(Pack, X64PackRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Pack, X86PackRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(Pack, A64PackRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Pack, ARM32PackRT, ::testing::ValuesIn(kARM), rtTCName);
