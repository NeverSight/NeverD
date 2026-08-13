//===- X86_X87LongDoubleConvRTTests.cpp - f80 conversion edges -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Adjacent to the recently fixed f80 (`long double`) precision/lift work, this
// probes the *conversion* edges of the 80-bit x87 type that X87LongDoubleRTTests
// (pure f80 arithmetic) does not: narrowing f80 -> double/float (fstpl/fstps
// rounding), widening int/double -> f80 (fild/fldl), and the f80 sign-bit ops
// (fchs/fabs).  Each kernel keeps the f80 value live across the conversion so a
// wrong rounding/extension diverges native-vs-lifted.  The result is always read
// from the low 64 mantissa bytes of a stored long double, or from a narrowed
// double/float / integer, so only well-defined state is compared.
//
//   * narrow_d  - compute in f80, narrow to double each step, accumulate.
//   * narrow_f  - compute in f80, narrow to float each step, accumulate.
//   * widen_mix - widen an int (fild) and a double (fldl) into f80 and combine.
//   * chs_abs   - f80 negate (fchs) and abs (fabsl -> fabs) sign-bit handling.
//   * to_int    - truncate an f80 value to int (fistp/fisttp), accumulate.
//   * cmp_order - f80 ordering across zero/negative driving a branch.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64X87LDConvRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64X87LDConvRT, Verify) { roundTripX64(GetParam()); }
class X86X87LDConvRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86X87LDConvRT, Verify) { roundTripX86(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeLDConvTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Compute in f80, narrow to double each iteration (fstpl rounds 80->64),
    // accumulate in double: a 64-bit-only model never rounds at the narrow.
    {p+"_narrow_d",
     t+" "+p+"_narrow_d("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u; long double x=(long double)u; double s=0;\n"
     "  for(int i=0;i<60;i++){ x=x*1.0000001L+3.14159265358979L;\n"
     "    double d=(double)x; s=s*0.5+d/7.0; }\n"
     "  unsigned long long m; __builtin_memcpy(&m,&s,8);\n"
     "  return ("+t+")(unsigned)(m ^ (m>>32)); }\n",
     {0x57ULL}, "X87LDConv", 2},

    // Narrow f80 -> float (fstps rounds 80->32) each step.
    {p+"_narrow_f",
     t+" "+p+"_narrow_f("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u; long double x=(long double)u; float s=0;\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<60;i++){ x=x*1.0000003L+2.718281828L;\n"
     "    float f=(float)x; s=s*0.25f+f; unsigned bits; __builtin_memcpy(&bits,&s,4);\n"
     "    h=h*131u+bits; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x39ULL}, "X87LDConv", 2},

    // Widen an int (fild) and a double (fldl) into f80, combine, store f80.
    {p+"_widen_mix",
     t+" "+p+"_widen_mix("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u; long double s=0; double d=(double)u*0.3;\n"
     "  for(int i=0;i<50;i++){ long double xi=(long double)(int)(u+i);\n"
     "    long double xd=(long double)d; s=s*1.0000001L + xi*0.5L - xd*0.25L; }\n"
     "  unsigned long long m; __builtin_memcpy(&m,&s,8);\n"
     "  return ("+t+")(unsigned)(m ^ (m>>32)); }\n",
     {0x6eULL}, "X87LDConv", 2},

    // f80 negate (fchs) and abs (fabsl) -- sign-bit-only manipulation of the
    // 80-bit value, then accumulate.
    {p+"_chs_abs",
     t+" "+p+"_chs_abs("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u; long double x=(long double)u - 17.5L, s=0;\n"
     "  for(int i=0;i<60;i++){ x=x*1.0000005L-0.987654321L;\n"
     "    long double ax=__builtin_fabsl(x); long double nx=-x;\n"
     "    s=s + ax*0.5L + nx*0.25L; }\n"
     "  unsigned long long m; __builtin_memcpy(&m,&s,8);\n"
     "  return ("+t+")(unsigned)(m ^ (m>>32)); }\n",
     {0x84ULL}, "X87LDConv", 2},

    // Truncate an f80 value to int each step (fistp under the truncating control
    // word) and fold the integer part into the running hash; the f80 fractional
    // state carries forward.  Isolates the FIST rounding-mode fix.
    {p+"_to_int",
     t+" "+p+"_to_int("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u; long double x=(long double)u*0.5L; unsigned h=0;\n"
     "  for(int i=0;i<60;i++){ x=x*1.0000009L+1.6180339887L;\n"
     "    int k=(int)x; h=h*131u+(unsigned)k; x=x-(long double)k*0.5L; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x29ULL}, "X87LDConv", 2},

    // f80 ordering between two *computed* (non-constant) values drives control
    // flow: comparing the loop-carried accumulator against a derived value keeps
    // the operands out of the x87 stack-resident-constant form, isolating the
    // f80 compare lift itself.
    {p+"_cmp_sel",
     t+" "+p+"_cmp_sel("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u; long double x=(long double)u, acc=0; unsigned h=0;\n"
     "  for(int i=0;i<60;i++){ x=x*1.0000007L+0.123456789L; long double y=x/3.0L;\n"
     "    if(y>acc){ acc=y; h=h*131u+1u; }\n"
     "    else if(y<-acc){ acc=-y; h=h*131u+3u; }\n"
     "    else { acc=acc*0.999L; h=h*131u+7u; } }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x4dULL}, "X87LDConv", 2},

    // FIST inside a conditional branch: the truncating `(int)x` cast sits in one
    // arm of an if/else so the x87 stack top reaching the convert is the
    // freshly-updated value, not the loop-carried phi.
    {p+"_fist_cond",
     t+" "+p+"_fist_cond("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u; long double x=(long double)u*0.25L; unsigned h=0;\n"
     "  for(int i=0;i<60;i++){ x=x*1.0000006L+2.5L;\n"
     "    if(u&1u){ int k=(int)x; h=h*131u+(unsigned)k; x=x-(long double)k; }\n"
     "    else { h=h*131u+9u; }\n"
     "    u=u*1103515245u+12345u; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x4dULL}, "X87LDConv", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64C = makeLDConvTC("x64ldc", "long");
static const std::vector<RoundTripTC> kX86C = makeLDConvTC("x86ldc", "int");

INSTANTIATE_TEST_SUITE_P(X87LDConv, X64X87LDConvRT,
                         ::testing::ValuesIn(kX64C), rtTCName);
INSTANTIATE_TEST_SUITE_P(X87LDConv, X86X87LDConvRT,
                         ::testing::ValuesIn(kX86C), rtTCName);
