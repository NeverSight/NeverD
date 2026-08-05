//===- AllPlatform_FPConvEdgeRTTests.cpp - FP<->int convert edges -*-C++*-=//
//
// Roundtrip probes for floating-point <-> integer conversion and scalar FP
// arithmetic over IEEE special values (NaN, +/-Inf, denormal, and magnitudes
// straddling INT_MAX / UINT_MAX), the saturation-semantics class the lifter must
// reproduce per target: x86 `cvttss2si` makes an out-of-range/NaN result the
// integer indefinite 0x80000000, whereas ARM `vcvt` / AArch64 `fcvtzs` saturate
// to INT_MAX / INT_MIN / 0 — a real ISA divergence the lift has to keep, so a
// faithful lift must re-emit the same instruction rather than fold the C-level
// out-of-range conversion to a constant.  Unlike AllPlatform_FPEdgeValueRTTests
// (x64/AArch64 only), this covers all four targets: i386 is forced onto SSE
// (`-mfpmath=sse`) so it converts with cvttss2si instead of x87's 80-bit path,
// and ARM32 stays on its cortex-a15 VFP default.  Special values travel as raw
// bit patterns through memory so they survive the integer ABI; every kernel is
// 32-bit-result only (no `__fixdfdi`-style 64-bit soft-float libcall on the
// 32-bit targets) and folds its results into a value-dependent hash compared
// native vs lifted at -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64FPConvEdgeRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64FPConvEdgeRT, Verify) { roundTripX64(GetParam()); }
class X86FPConvEdgeRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86FPConvEdgeRT, Verify) { roundTripX86(GetParam()); }
class A64FPConvEdgeRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64FPConvEdgeRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32FPConvEdgeRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32FPConvEdgeRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeFPConvTC(const char *prefix, const char *T,
                                             const char *Extra) {
  std::string p = prefix, t = T, e = Extra;
  return {
    // float -> int / unsigned over edge magnitudes (cvttss2si / vcvt / fcvtzs
    // saturation).  The float bit patterns are runtime (loaded from memory and
    // indexed by the argument) so clang cannot constant-fold the conversion.
    {p+"_f2i",
     t+" "+p+"_f2i("+t+" a){\n"
     "  unsigned s[10]={0x7FC00001u,0x7F800000u,0xFF800000u,0x4F000000u,\n"
     "    0xCF000000u,0x4F800000u,0x3F800000u,0xBF800000u,0x00000001u,0x4B7FFFFFu};\n"
     "  unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<10;i++){ float x; __builtin_memcpy(&x,&s[(i+(unsigned)a)%10u],4);\n"
     "    int si=(int)x; unsigned ui=(unsigned)x;\n"
     "    h = h*131u + (unsigned)si*7u + ui*3u; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x3ULL}, "FPConv", 2, e},

    // double -> int / unsigned over edge magnitudes (cvttsd2si / vcvt / fcvtzs).
    {p+"_d2i",
     t+" "+p+"_d2i("+t+" a){\n"
     "  unsigned long long s[10]={0x7FF8000000000001ULL,0x7FF0000000000000ULL,\n"
     "    0xFFF0000000000000ULL,0x41E0000000000000ULL,0xC1E0000000000000ULL,\n"
     "    0x41F0000000000000ULL,0x3FF0000000000000ULL,0xBFF0000000000000ULL,\n"
     "    0x0000000000000001ULL,0x4150000000000000ULL};\n"
     "  unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<10;i++){ double x; __builtin_memcpy(&x,&s[(i+(unsigned)a)%10u],8);\n"
     "    int si=(int)x; unsigned ui=(unsigned)x;\n"
     "    h = h*131u + (unsigned)si*7u + ui*3u; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x5ULL}, "FPConv", 2, e},

    // int / unsigned -> float and double, returning the result bit pattern: the
    // rounding the conversion applies (cvtsi2ss round-to-nearest-even on a value
    // with more than 24 significant bits) must match the hardware.
    {p+"_i2f",
     t+" "+p+"_i2f("+t+" a){\n"
     "  unsigned s[10]={0u,1u,0x7FFFFFFFu,0x80000000u,0xFFFFFFFFu,0x00FFFFFFu,\n"
     "    0x01000001u,0x7FFFFFC0u,0x12345678u,0xA5A5A5A5u};\n"
     "  unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<10;i++){ unsigned v=s[(i+(unsigned)a)%10u];\n"
     "    float fs=(float)(int)v, fu=(float)v;\n"
     "    double ds=(double)(int)v;\n"
     "    unsigned bs,bu; __builtin_memcpy(&bs,&fs,4); __builtin_memcpy(&bu,&fu,4);\n"
     "    unsigned long long bd; __builtin_memcpy(&bd,&ds,8);\n"
     "    h = h*131u + bs*7u + bu*3u + (unsigned)(bd>>32) + (unsigned)bd; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x7ULL}, "FPConv", 2, e},

    // Scalar FP arithmetic web over special values, returning the float result
    // bit pattern: NaN propagation, signed-zero and infinity arithmetic must be
    // bit-exact between native and lifted (the #389 single-precision class).
    {p+"_fweb",
     t+" "+p+"_fweb("+t+" a){\n"
     "  unsigned s[8]={0x7FC00001u,0x00000001u,0x80000000u,0x7F800000u,\n"
     "    0xFF800000u,0x3F800000u,0xBF800000u,0x00400000u};\n"
     "  unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<8;i++){ float x; __builtin_memcpy(&x,&s[(i+(unsigned)a)&7u],4);\n"
     "    float y; __builtin_memcpy(&y,&s[(i+5u)&7u],4);\n"
     "    float r=(x*y)+y-x; r=r/(x+1.0f); float r2=__builtin_fabsf(r)-y;\n"
     "    unsigned b1,b2; __builtin_memcpy(&b1,&r,4); __builtin_memcpy(&b2,&r2,4);\n"
     "    h = h*131u + b1*7u + b2*3u; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x9ULL}, "FPConv", 2, e},

    // FP compare ordering matrix over NaN/Inf/zero: each unordered/ordered
    // predicate must match the hardware compare (ucomiss / fcmp), and the
    // optimizer must keep each predicate's polarity distinct.
    {p+"_fcmp",
     t+" "+p+"_fcmp("+t+" a){\n"
     "  unsigned s[6]={0x7FC00000u,0x7F800000u,0xFF800000u,0x00000000u,\n"
     "    0x80000000u,0x40490FDBu};\n"
     "  unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<6;i++){ float x; __builtin_memcpy(&x,&s[i],4);\n"
     "    for(int j=0;j<6;j++){ float y; __builtin_memcpy(&y,&s[j],4);\n"
     "      unsigned r=(x<y)+2u*(x<=y)+4u*(x==y)+8u*(x!=y)+16u*(x>y)+32u*(x>=y);\n"
     "      h=h*31u+r; } }\n"
     "  return ("+t+")(unsigned long)(h+(unsigned)(a&0)); }\n",
     {0xBULL}, "FPConv", 2, e},

    // double<->float narrowing/widening round-trips over edge magnitudes: the
    // precision loss (round-to-nearest-even on narrowing) must be bit-exact.
    {p+"_narrow",
     t+" "+p+"_narrow("+t+" a){\n"
     "  unsigned long long s[6]={0x400921FB54442D18ULL,0x7FF8000000000000ULL,\n"
     "    0x7FF0000000000000ULL,0x3E70000000000001ULL,0x47EFFFFFE0000000ULL,\n"
     "    0x0000000000000001ULL};\n"
     "  unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<6;i++){ double d; __builtin_memcpy(&d,&s[(i+(unsigned)a)%6u],8);\n"
     "    float f=(float)d; double back=(double)f;\n"
     "    unsigned bf; __builtin_memcpy(&bf,&f,4);\n"
     "    unsigned long long bb; __builtin_memcpy(&bb,&back,8);\n"
     "    h = h*131u + bf*7u + (unsigned)(bb>>32) + (unsigned)bb; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0xDULL}, "FPConv", 2, e},
  };
}
// clang-format on

// i386 is forced onto SSE math so its conversions use cvttss2si (matching the
// lifted re-emission) rather than x87's 80-bit intermediates; the other targets
// use their hardware-FP default.
static const std::vector<RoundTripTC> kX64 = makeFPConvTC("x64fpc", "long", "");
static const std::vector<RoundTripTC> kX86 =
    makeFPConvTC("x86fpc", "int", "-msse2 -mfpmath=sse");
static const std::vector<RoundTripTC> kA64 = makeFPConvTC("a64fpc", "long", "");
static const std::vector<RoundTripTC> kARM = makeFPConvTC("armfpc", "int", "");

INSTANTIATE_TEST_SUITE_P(FPConv, X64FPConvEdgeRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(FPConv, X86FPConvEdgeRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(FPConv, A64FPConvEdgeRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(FPConv, ARM32FPConvEdgeRT, ::testing::ValuesIn(kARM), rtTCName);
