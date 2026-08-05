//===- AllPlatform_FPEdgeValueRTTests.cpp - FP special-value bit exact -C++-===//
//
// Adversarial probes that push FP special values (NaN payloads, +/-0, +/-Inf,
// denormals) through arithmetic, min/max, copysign, fabs, sqrt, and float<->
// double conversion, returning the RESULT BIT PATTERN.  This is the #389 class
// (single-precision mis-lifted as double -> sign/order flips): the lifted code
// must reproduce the hardware bit pattern exactly.  Inputs/outputs travel as
// raw u64 bitcasts so special values survive the integer ABI; x64 + AArch64
// only (i386 x87 uses 80-bit intermediates -- a real ISA difference, not a
// lift bug).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64FPEdgeRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64FPEdgeRT, Verify) { roundTripX64(GetParam()); }

class A64FPEdgeRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64FPEdgeRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeFPEdgeTC(const char *prefix) {
  std::string p = prefix;
  return {
    // Double arithmetic web over special values: +-0, +-Inf, NaN, denormal.
    {p+"_dbl_web",
     "long "+p+"_dbl_web(long a){\n"
     "  unsigned long seeds[8]={0x7FF8000000000001ULL,0x0000000000000001ULL,\n"
     "    0x8000000000000000ULL,0x7FF0000000000000ULL,0xFFF0000000000000ULL,\n"
     "    0x3FF0000000000000ULL,0xBFF0000000000000ULL,0x0008000000000000ULL};\n"
     "  unsigned idx=(unsigned)a&7u; double x; __builtin_memcpy(&x,&seeds[idx],8);\n"
     "  double y; __builtin_memcpy(&y,&seeds[(idx+3)&7],8);\n"
     "  double r=(x+y)*y - x/y; if(r<y) r=r*2.0; \n"
     "  unsigned long o; __builtin_memcpy(&o,&r,8); return (long)o; }\n",
     {3ULL}, "FPEdge", 2},

    // fmin/fmax with NaN (one operand NaN -> returns the non-NaN per IEEE-754).
    {p+"_minmax_nan",
     "long "+p+"_minmax_nan(long a){\n"
     "  unsigned long sn=0x7FF8000000000000ULL,sv=0x4010000000000000ULL;\n"
     "  double n; __builtin_memcpy(&n,&sn,8); double v; __builtin_memcpy(&v,&sv,8);\n"
     "  double lo=__builtin_fmin(n,v), hi=__builtin_fmax(v,n);\n"
     "  double r=lo+hi*3.0+(double)(a&0);\n"
     "  unsigned long o; __builtin_memcpy(&o,&r,8); return (long)o; }\n",
     {0ULL}, "FPEdge", 2},

    // copysign / fabs over signed zero and signed Inf.
    {p+"_copysign",
     "long "+p+"_copysign(long a){\n"
     "  unsigned long sz=0x8000000000000000ULL,si=0x7FF0000000000000ULL;\n"
     "  double z; __builtin_memcpy(&z,&sz,8); double inf; __builtin_memcpy(&inf,&si,8);\n"
     "  double r=__builtin_copysign(inf,z)+__builtin_fabs(z)*1.0+(double)(a&0);\n"
     "  unsigned long o; __builtin_memcpy(&o,&r,8); return (long)o; }\n",
     {0ULL}, "FPEdge", 2},

    // float (single) arithmetic over special values -> the #389 precision class.
    {p+"_flt_web",
     "long "+p+"_flt_web(long a){\n"
     "  unsigned seeds[8]={0x7FC00001u,0x00000001u,0x80000000u,0x7F800000u,\n"
     "    0xFF800000u,0x3F800000u,0xBF800000u,0x00400000u};\n"
     "  unsigned idx=(unsigned)a&7u; float x; __builtin_memcpy(&x,&seeds[idx],4);\n"
     "  float y; __builtin_memcpy(&y,&seeds[(idx+5)&7],4);\n"
     "  float r=(x*y)+y - x; if(r<=y) r=r-1.0f;\n"
     "  unsigned o; __builtin_memcpy(&o,&r,4); return (long)(unsigned long)o; }\n",
     {5ULL}, "FPEdge", 2},

    // double<->float round-trip narrowing (precision loss must match hardware).
    {p+"_narrow",
     "long "+p+"_narrow(long a){\n"
     "  unsigned long sd=0x400921FB54442D18ULL; /* pi */\n"
     "  double d; __builtin_memcpy(&d,&sd,8);\n"
     "  float f=(float)d; double back=(double)f; double r=back-d+(double)(a&0);\n"
     "  unsigned long o; __builtin_memcpy(&o,&r,8); return (long)o; }\n",
     {0ULL}, "FPEdge", 2},

    // Single-precision compare ordering over NaN/Inf (movmskps/ucomiss class).
    {p+"_flt_cmp",
     "long "+p+"_flt_cmp(long a){\n"
     "  unsigned seeds[6]={0x7FC00000u,0x7F800000u,0xFF800000u,0x00000000u,0x80000000u,0x40490FDBu};\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<6;i++){ float x; __builtin_memcpy(&x,&seeds[i],4);\n"
     "    for(int j=0;j<6;j++){ float y; __builtin_memcpy(&y,&seeds[j],4);\n"
     "      acc=acc*3u+(x<y)+2u*(x==y)+4u*(x>y)+8u*(x!=y); } }\n"
     "  return (long)(unsigned long)(acc+(unsigned)(a&0)); }\n",
     {0ULL}, "FPEdge", 2},

    // sqrt of negative / zero / Inf (NaN result bit pattern).  -fno-math-errno
    // keeps clang emitting the bare sqrt instruction instead of an errno-path
    // libcall to sqrt (which is unmapped under Unicorn), so the test exercises
    // the lifted instruction rather than a runtime-library call.
    {p+"_sqrt_edge",
     "long "+p+"_sqrt_edge(long a){\n"
     "  unsigned long sn=0xBFF0000000000000ULL,sz=0x8000000000000000ULL;\n"
     "  double neg; __builtin_memcpy(&neg,&sn,8); double z; __builtin_memcpy(&z,&sz,8);\n"
     "  double r=__builtin_sqrt(neg)+__builtin_sqrt(z)*2.0+(double)(a&0);\n"
     "  unsigned long o; __builtin_memcpy(&o,&r,8); return (long)o; }\n",
     {0ULL}, "FPEdge", 2, "-fno-math-errno"},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeFPEdgeTC("x64fpe");
static const std::vector<RoundTripTC> kA64 = makeFPEdgeTC("a64fpe");

INSTANTIATE_TEST_SUITE_P(FPEdge, X64FPEdgeRT,
                         ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(FPEdge, A64FPEdgeRT,
                         ::testing::ValuesIn(kA64), rtTCName);
