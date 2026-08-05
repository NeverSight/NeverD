//===- X86_X87LongDoubleRTTests.cpp - 80-bit long double roundtrip -*-C++*-=//
//
// `long double` on x86/x86-64 is the 80-bit x87 extended-precision type: clang
// emits `fldt`/`fstpt` (m80 load/store) and computes on the x87 stack at 64-bit
// mantissa precision.  Earlier x87 tests dodged this by using values that are
// exact in 64-bit (10.0/5.0 = 2.0), so a double-precision model happened to
// match.  These kernels accumulate non-representable constants over many
// iterations and return the low mantissa bytes, where a 64-bit vs 80-bit model
// diverges in the last bits -- exercising the x87 register as genuine f80.
//
// The result is read back from the low 8 bytes (the 64-bit mantissa) of the
// stored long double, never the undefined padding bytes, so original and lifted
// are compared on well-defined state only.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64X87LongDoubleRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64X87LongDoubleRT, Verify) { roundTripX64(GetParam()); }
class X86X87LongDoubleRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86X87LongDoubleRT, Verify) { roundTripX86(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeLongDoubleTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Multiply-and-add accumulation with constants that are not exact in any
    // binary float -- 80-bit and 64-bit mantissas diverge after a few rounds.
    {p+"_acc",
     t+" "+p+"_acc("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u;\n"
     "  long double x=(long double)u, s=0;\n"
     "  for(int i=0;i<50;i++){ x=x*1.0000001L+3.14159265358979L; s=s+x/7.0L; }\n"
     "  unsigned long long m; __builtin_memcpy(&m,&s,8);\n"
     "  return ("+t+")(unsigned)(m ^ (m>>32)); }\n",
     {0x57ULL}, "X87LongDouble", 2},

    // Division-heavy chain: 80-bit division rounds differently from 64-bit.
    {p+"_divchain",
     t+" "+p+"_divchain("+t+" a){\n"
     "  unsigned u=(unsigned)a|3u;\n"
     "  long double x=(long double)u;\n"
     "  for(int i=0;i<48;i++){ x = (x+1.0L)/1.3000000001L; x = x*2.7L - 0.333333333333L; }\n"
     "  unsigned long long m; __builtin_memcpy(&m,&x,8);\n"
     "  return ("+t+")(unsigned)(m ^ (m>>32)); }\n",
     {0xa3ULL}, "X87LongDouble", 2},

    // sqrt chain via the hardware x87 FSQRT (inline asm forces the instruction
    // instead of a `sqrtl` libcall): the 80-bit FSQRT result diverges from a
    // 64-bit one.  x is round-tripped through memory each step so the kernel
    // exercises f80 precision, not deep x87-stack register allocation.
    {p+"_sqrtchain",
     t+" "+p+"_sqrtchain("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u;\n"
     "  long double x=(long double)u + 0.5L; unsigned h=0;\n"
     "  for(int i=0;i<40;i++){ long double v=x*1.0000003L+2.0L;\n"
     "    __asm__(\"fsqrt\":\"+t\"(v)); x=v+0.1L;\n"
     "    unsigned long long m; __builtin_memcpy(&m,&x,8); h=h*131u+(unsigned)m; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x39ULL}, "X87LongDouble", 2},

    // Mixed-width: load a double (fldl, widened to f80) and an m80 constant,
    // combine, store the m80 result -- exercises both the widen and the native
    // 80-bit store path in one kernel.
    {p+"_mixwidth",
     t+" "+p+"_mixwidth("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u;\n"
     "  double d=(double)u * 1.1;\n"
     "  long double x=(long double)d, s=0;\n"
     "  for(int i=0;i<48;i++){ x=x*1.0000001L + (long double)d*0.5L; s=s+x - (long double)i*0.7L; }\n"
     "  unsigned long long m; __builtin_memcpy(&m,&s,8);\n"
     "  return ("+t+")(unsigned)(m ^ (m>>32)); }\n",
     {0x6eULL}, "X87LongDouble", 2},

    // Pure 80-bit polynomial (Horner) with non-exact coefficients.
    {p+"_horner",
     t+" "+p+"_horner("+t+" a){\n"
     "  long double x=(long double)((unsigned)a|1u) * 0.0001L;\n"
     "  long double r=0;\n"
     "  long double c[5]={1.1L,2.2000001L,3.3L,4.4444L,5.5L};\n"
     "  for(int k=0;k<200;k++){ r=0; for(int i=0;i<5;i++) r=r*x+c[i]; x=x+0.00001L; }\n"
     "  unsigned long long m; __builtin_memcpy(&m,&r,8);\n"
     "  return ("+t+")(unsigned)(m ^ (m>>32)); }\n",
     {0x21ULL}, "X87LongDouble", 2},

    // Comparison/select driven by 80-bit values: the branch a 64-bit model
    // takes can differ at the boundary where the two precisions disagree.
    {p+"_cmpsel",
     t+" "+p+"_cmpsel("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u; long double x=(long double)u, acc=0;\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<60;i++){ x=x*1.0000007L+0.123456789L;\n"
     "    long double y=x/3.0L;\n"
     "    if(y> acc) { acc=y; h=h*131u+1u; } else { acc=acc*0.999L; h=h*131u+7u; } }\n"
     "  unsigned long long m; __builtin_memcpy(&m,&acc,8);\n"
     "  return ("+t+")(unsigned)(h ^ (unsigned)m ^ (unsigned)(m>>32)); }\n",
     {0x84ULL}, "X87LongDouble", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64LD = makeLongDoubleTC("x64ld", "long");
static const std::vector<RoundTripTC> kX86LD = makeLongDoubleTC("x86ld", "int");

INSTANTIATE_TEST_SUITE_P(X87LongDouble, X64X87LongDoubleRT,
                         ::testing::ValuesIn(kX64LD), rtTCName);
INSTANTIATE_TEST_SUITE_P(X87LongDouble, X86X87LongDoubleRT,
                         ::testing::ValuesIn(kX86LD), rtTCName);
