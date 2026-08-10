//===- X86_FpI64CvtRTTests.cpp - i386 64-bit int<->FP cvt ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// AllPlatform_FPUnsignedCvtRTTests deliberately SKIPPED i386 for the 64-bit
// conversions, claiming they lower to a __floatundidf / __fixunsdfdi libcall.
// That is wrong for the default i386 target: clang -O2 INLINES every 64-bit
// int<->FP conversion — `(double)u64` is the SSE2 "magic" sequence (movsd /
// unpcklps / subpd) with TWO rodata bias constants loaded via GOTOFF (a
// constant-pool-mapping case), `(double)s64` / `(s64)double` are x87
// fildll / fistpll, and the result is returned through ST0.  No libcall.  So
// the whole i386 64-bit conversion family was simply never round-tripped.
//
// These probes drive it on i386 (the real gap) with x86-64 and AArch64 as
// already-green cross-checks.  ARM32 is excluded: its 64-bit int<->FP really
// are __aeabi_l2d / __aeabi_d2lz libcalls the bare-metal harness cannot run.
// Every kernel keeps the doubles in range (no out-of-range cvt UB) and folds
// to a value whose low 32 bits carry the full state.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64FpI64CvtRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64FpI64CvtRT, Verify) { roundTripX64(GetParam()); }
class X86FpI64CvtRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86FpI64CvtRT, Verify) { roundTripX86(GetParam()); }
class A64FpI64CvtRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64FpI64CvtRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeI64CvtTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // signed 64 <-> double (x87 fildll / fistpll on i386).
    {p+"_s64d",
     t+" "+p+"_s64d("+t+" a){\n"
     "  long long iacc=(long long)(int)a; double acc=0;\n"
     "  unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<100;i++){ s=s*1103515245u+12345u;\n"
     "    long long v=((long long)(int)s<<11)+(int)(s>>7);\n"
     "    double d=(double)v; d=d*0.5+1.0;\n"
     "    long long back=(long long)d;\n"
     "    iacc+=back^(long long)(s>>3); acc+=d*1e-6; }\n"
     "  return ("+t+")(iacc^(iacc>>32)^(long long)acc); }\n",
     {0xE1u}, "FpI64Cvt", 2, ""},

    // unsigned 64 -> double (SSE magic + rodata bias on i386), back to u64.
    {p+"_u64d",
     t+" "+p+"_u64d("+t+" a){\n"
     "  unsigned long long acc=0; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<100;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned long long u=0x8000000000000000ULL+(unsigned long long)s*2654435761ULL;\n"
     "    double d=(double)u;\n"
     "    unsigned long long back=(unsigned long long)(d/7.0);\n"
     "    acc+=back^(u>>23); }\n"
     "  return ("+t+")(acc^(acc>>32)); }\n",
     {0xE2u}, "FpI64Cvt", 2, ""},

    // 64-bit int <-> float (narrower mantissa; fildll + cvt, float -> i64).
    {p+"_i64f",
     t+" "+p+"_i64f("+t+" a){\n"
     "  unsigned long long acc=1; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<100;i++){ s=s*1103515245u+12345u;\n"
     "    long long v=((long long)(int)s<<9)^0x123456789LL;\n"
     "    unsigned long long u=((unsigned long long)s<<32)|(s^0x55u);\n"
     "    float f=(float)v, g=(float)u;\n"
     "    long long bv=(long long)(f*0.5f);\n"
     "    unsigned long long bu=(unsigned long long)(g*0.25f);\n"
     "    acc=acc*131+(unsigned long long)bv+bu; acc^=acc>>29; }\n"
     "  return ("+t+")(acc^(acc>>32)); }\n",
     {0xE3u}, "FpI64Cvt", 2, ""},

    // double straddling 2^63 -> u64 and -> s64 (bias-sub boundary), both dirs.
    {p+"_d2i64",
     t+" "+p+"_d2i64("+t+" a){\n"
     "  unsigned long long acc=0; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<100;i++){ s=s*1103515245u+12345u;\n"
     "    double d=(double)(unsigned)s*4294967296.0*1.7+(double)(int)(s>>1);\n"
     "    unsigned long long u=(unsigned long long)d;\n"
     "    long long sv=(long long)(d*0.5-3e18);\n"
     "    acc+=u^(unsigned long long)sv; acc^=acc>>27; }\n"
     "  return ("+t+")(acc^(acc>>32)); }\n",
     {0xE4u}, "FpI64Cvt", 2, ""},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeI64CvtTC("x64fc", "long");
static const std::vector<RoundTripTC> kX86 = makeI64CvtTC("x86fc", "int");
static const std::vector<RoundTripTC> kA64 = makeI64CvtTC("a64fc", "long");

INSTANTIATE_TEST_SUITE_P(FpI64Cvt, X64FpI64CvtRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(FpI64Cvt, X86FpI64CvtRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(FpI64Cvt, A64FpI64CvtRT, ::testing::ValuesIn(kA64), rtTCName);
