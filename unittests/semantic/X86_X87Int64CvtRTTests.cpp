//===- X86_X87Int64CvtRTTests.cpp - f80 <-> int64 conversions --*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// X87LongDoubleConvRTTests covers f80 <-> int32 (`(int)x`, fild dword) and
// f80 <-> double, but never the 64-bit integer conversions.  `(long double)
// (long long)v` is a `fildll` that loads a full 64-bit integer into the 80-bit
// stack top (exact: f80 has a 64-bit mantissa), `(long long)x` is a `fistpll`,
// and the unsigned 64-bit forms add/subtract the 2^63 bias around them.  Both
// i386 and x86-64 use x87 for `long double`, so both inline these (no libcall).
// The f80 model was only recently widened to 80 bits, so the 64-bit-integer
// conversion edge is exactly where a leftover 32-bit-width assumption would
// hide.
//
//   * i64_rt - int64 -> f80 -> int64 round trip (fildll / fistpll).
//   * u64_rt - unsigned64 -> f80 -> unsigned64, values >= 2^63 (both biases).
//   * mix64  - signed and unsigned 64-bit conversions mixed with an f80 compare.
//
// Values stay in range (no out-of-range cvt UB); folds to a low-32-bit state.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64X87I64CvtRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64X87I64CvtRT, Verify) { roundTripX64(GetParam()); }
class X86X87I64CvtRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86X87I64CvtRT, Verify) { roundTripX86(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeX87I64TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // int64 -> f80 (fildll) -> int64 (fistpll) round trip.
    {p+"_i64_rt",
     t+" "+p+"_i64_rt("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; long long acc=0;\n"
     "  for(int i=0;i<80;i++){ s=s*1103515245u+12345u;\n"
     "    long long v=((long long)(int)s<<18)+(int)(s>>9);\n"
     "    long double x=(long double)v; x=x*1.0000001L+0.5L;\n"
     "    long long back=(long long)x;\n"
     "    acc+=back^(long long)(s>>3); }\n"
     "  return ("+t+")(acc^(acc>>32)); }\n",
     {0xF1u}, "X87I64Cvt", 2},

    // unsigned64 (bit 63 set) -> f80 (fildll+bias) -> unsigned64 (fistpll+bias).
    {p+"_u64_rt",
     t+" "+p+"_u64_rt("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned long long acc=0;\n"
     "  for(int i=0;i<80;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned long long u=0x8000000000000000ULL+(unsigned long long)s*2654435761ULL;\n"
     "    long double x=(long double)u;\n"
     "    unsigned long long back=(unsigned long long)x;\n"
     "    acc+=back^(u>>23); }\n"
     "  return ("+t+")(acc^(acc>>32)); }\n",
     {0xF2u}, "X87I64Cvt", 2},

    // Mixed signed/unsigned 64-bit conversions across an f80 ordering branch.
    {p+"_mix64",
     t+" "+p+"_mix64("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; long long acc=0; long double run=0;\n"
     "  for(int i=0;i<80;i++){ s=s*1103515245u+12345u;\n"
     "    long long v=((long long)(int)s<<16)-(int)(s>>11);\n"
     "    long double x=(long double)v; run=run*0.5L+x*1e-7L;\n"
     "    if(run>0.0L){ acc+=(long long)x; }\n"
     "    else { unsigned long long u=(unsigned long long)(-v); acc^=(long long)u; } }\n"
     "  return ("+t+")(acc^(acc>>32)); }\n",
     {0xF3u}, "X87I64Cvt", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeX87I64TC("x64x87i", "long");
static const std::vector<RoundTripTC> kX86 = makeX87I64TC("x86x87i", "int");

INSTANTIATE_TEST_SUITE_P(X87I64Cvt, X64X87I64CvtRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(X87I64Cvt, X86X87I64CvtRT, ::testing::ValuesIn(kX86), rtTCName);
