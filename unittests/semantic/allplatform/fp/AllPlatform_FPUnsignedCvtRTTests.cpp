//===- AllPlatform_FPUnsignedCvtRTTests.cpp - unsigned FP cvt --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// clang -O2 unsigned integer <-> floating-point conversion kernels.  Unsigned
// conversions are the awkward ones: x86 has no unsigned cvt, so `(double)u64`
// and `(u64)double` lower to a sign-bit test plus a 2^63 bias add/sub branch
// (FLOAT_UINT2FLOAT / FLOAT_FLOAT2UINT), while AArch64 uses ucvtf/fcvtzu and
// ARM32/i386 their own VFP/x87 sequences.  u32 conversions run on all four
// targets; the u64 cases here cover the 64-bit targets x86-64 / AArch64.  i386
// also INLINES the 64-bit conversions (SSE2 magic + x87 fild/fistp, no libcall)
// and is round-tripped separately by X86_FpI64CvtRTTests; only ARM32 genuinely
// lowers 64-bit int<->FP to __aeabi_* libcalls the bare-metal harness lacks.
// Every conversion is deterministic, so the kernels fold to an exact int.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64FPUCvtRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64FPUCvtRT, Verify) { roundTripX64(GetParam()); }

class X86FPUCvtRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86FPUCvtRT, Verify) { roundTripX86(GetParam()); }

class A64FPUCvtRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64FPUCvtRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32FPUCvtRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32FPUCvtRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
// u32<->double/float conversions: inline on all four targets.
static std::vector<RoundTripTC> makeU32Cvt(const char *prefix) {
  std::string p = prefix;
  return {
    {p+"_u32d",
     "int "+p+"_u32d(int a){ double acc=0;\n"
     "  for(int i=0;i<128;i++){ unsigned u=(unsigned)(a*(i+1))*2654435761u+i;\n"
     "    double d=(double)u; d=d*0.5+(double)(u&0xFFFF);\n"
     "    unsigned back=(unsigned)d; acc += (double)(back & 0x3FFFFF); }\n"
     "  unsigned r=(unsigned)acc; return (int)(r ^ (r>>7)); }\n",
     {0x1234567ULL}, "FPUCvt", 2, ""},

    {p+"_u32f",
     "int "+p+"_u32f(int a){ float acc=0;\n"
     "  for(int i=0;i<128;i++){ unsigned u=(unsigned)(a+i*131)*40503u;\n"
     "    float f=(float)(u&0xFFFFF); f=f*1.25f+(float)(i&63);\n"
     "    unsigned back=(unsigned)f; acc += (float)(back & 0x3FFFF); }\n"
     "  unsigned r=(unsigned)acc; return (int)(r ^ (r>>5)); }\n",
     {0x2233445ULL}, "FPUCvt", 2, ""},

    // Round-trip an unsigned value near the 32-bit top through double and back.
    {p+"_u32hi",
     "int "+p+"_u32hi(int a){ unsigned acc=0;\n"
     "  for(int i=0;i<120;i++){ unsigned u=0xFFFF0000u + (unsigned)(a*i+i*7);\n"
     "    double d=(double)u; unsigned back=(unsigned)(d/3.0);\n"
     "    acc += back ^ (u>>11); }\n"
     "  return (int)(acc ^ (acc>>9)); }\n",
     {0x3344556ULL}, "FPUCvt", 2, ""},
  };
}

// u64<->double conversions: x86 bias-branch / AArch64 ucvtf+fcvtzu.
static std::vector<RoundTripTC> makeU64Cvt(const char *prefix) {
  std::string p = prefix;
  return {
    {p+"_u64d",
     "long "+p+"_u64d(long a){ double acc=0;\n"
     "  for(int i=0;i<100;i++){ unsigned long u=(unsigned long)(a*(i+1))\n"
     "      *2654435761ull + (unsigned)i;\n"
     "    double d=(double)u; d=d*0.5;\n"
     "    unsigned long back=(unsigned long)d; acc += (double)(back & 0xFFFFFFF); }\n"
     "  unsigned long r=(unsigned long)acc; return (long)(r ^ (r>>17)); }\n",
     {0x4455667ULL}, "FPUCvt", 2, ""},

    // Values with bit 63 set exercise the 2^63 bias path of (double)u64.
    {p+"_u64hi",
     "long "+p+"_u64hi(long a){ unsigned long acc=0;\n"
     "  for(int i=0;i<100;i++){ unsigned long u=0x8000000000000000ull\n"
     "      + (unsigned long)(a*i+i*131)*2654435761ull;\n"
     "    double d=(double)u; unsigned long back=(unsigned long)(d/7.0);\n"
     "    acc += back ^ (u>>23); }\n"
     "  return (long)(acc ^ (acc>>31)); }\n",
     {0x5566778ULL}, "FPUCvt", 2, ""},

    // double->u64 of values straddling 2^63 (the fcvtzu / bias-sub boundary).
    {p+"_d2u64",
     "long "+p+"_d2u64(long a){ unsigned long acc=0;\n"
     "  for(int i=0;i<100;i++){ double d=(double)(unsigned)(a*i+i)\n"
     "      *4294967296.0 * 1.7 + (double)i;\n"
     "    unsigned long u=(unsigned long)d; acc += u ^ (u>>29); }\n"
     "  return (long)(acc ^ (acc>>13)); }\n",
     {0x6677889ULL}, "FPUCvt", 2, ""},
  };
}

static const std::vector<RoundTripTC> kX64U32 = makeU32Cvt("x64uc");
static const std::vector<RoundTripTC> kX86U32 = makeU32Cvt("x86uc");
static const std::vector<RoundTripTC> kA64U32 = makeU32Cvt("a64uc");
static const std::vector<RoundTripTC> kARM32U32 = makeU32Cvt("armuc");
static const std::vector<RoundTripTC> kX64U64 = makeU64Cvt("x64uc64");
static const std::vector<RoundTripTC> kA64U64 = makeU64Cvt("a64uc64");
// clang-format on

INSTANTIATE_TEST_SUITE_P(FPUCvt32, X64FPUCvtRT, ::testing::ValuesIn(kX64U32),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(FPUCvt32, X86FPUCvtRT, ::testing::ValuesIn(kX86U32),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(FPUCvt32, A64FPUCvtRT, ::testing::ValuesIn(kA64U32),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(FPUCvt32, ARM32FPUCvtRT, ::testing::ValuesIn(kARM32U32),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(FPUCvt64, X64FPUCvtRT, ::testing::ValuesIn(kX64U64),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(FPUCvt64, A64FPUCvtRT, ::testing::ValuesIn(kA64U64),
                         rtTCName);
