//===- AllPlatform_OptStress264RTTests.cpp - x87 long double at -O0 ======//
//
// 80-bit x87 long double at -O0 (x86 / x86-64 only) — the freshly-modified x87
// area (TOP-relative modeling, f80 precision) is exercised here with the -O0
// store-everything form: every long double subexpression round-trips through an
// 80-bit `fstpt`/`fldt` frame slot, and conversions go through `fistp`/`fild`.
// AArch64/ARM32 are excluded: their long double is 128-bit / double and the wide
// ops turn into libcalls the bare-metal harness cannot run.
//
//   * ldarith - long double add / sub / mul-by-const, bounded recurrence.
//   * ldcmp   - long double ordered compare driving min/max select.
//   * ldcvt   - int -> long double -> double -> int conversion round trips.
//   * ldsqrt  - __builtin_sqrtl (fsqrt) over a bounded long double.
//   * ldmix   - combined arithmetic + compare + convert into one accumulator.
//
// Integer in / integer out (x87 result folded via fistp to one integer return),
// LCG-seeded.  Only x64 / x86, -O0.  Bounded recurrences keep magnitudes small
// so (int) truncation stays well-defined.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress264RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress264RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress264RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress264RT, Verify) { roundTripX86(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress264TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // long double add / sub / mul-by-const, bounded recurrence.
    {p+"_ldarith",
     t+" "+p+"_ldarith("+t+" a){ unsigned h=(unsigned)a; long double acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    long double x=(long double)(int)(h&0x3ffu); long double y=(long double)(int)((h>>10)&0xffu);\n"
     "    acc=acc*0.5L + x - y*0.25L; }\n"
     "  return ("+t+")(int)acc; }\n",
     {0x12345u}, "OptStress264", 0},

    // long double ordered compare driving min/max select.
    {p+"_ldcmp",
     t+" "+p+"_ldcmp("+t+" a){ unsigned h=(unsigned)a; long double acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    long double x=(long double)(int)(h&0x1ffu); long double y=(long double)(int)((h>>9)&0x1ffu);\n"
     "    long double mn=(x<y)?x:y; long double mx=(x>y)?x:y;\n"
     "    acc=acc*0.5L + mn - mx*0.25L; }\n"
     "  return ("+t+")(int)acc; }\n",
     {0x23456u}, "OptStress264", 0},

    // int -> long double -> double -> int conversion round trips.
    {p+"_ldcvt",
     t+" "+p+"_ldcvt("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    int v=(int)(h&0x7fffu)-0x4000;\n"
     "    long double l=(long double)v * 1.25L; double d=(double)l; int r=(int)l;\n"
     "    acc=acc*131 + r + (int)d + (int)(long double)d; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress264", 0},

    // __builtin_sqrtl (fsqrt) over a bounded long double.  -fno-math-errno so
    // clang inlines the x87 fsqrt instead of a sqrtl libcall the harness lacks.
    {p+"_ldsqrt",
     t+" "+p+"_ldsqrt("+t+" a){ unsigned h=(unsigned)a; long double acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    long double x=(long double)(int)((h&0xffffu)+1);\n"
     "    acc=acc*0.5L + __builtin_sqrtl(x); }\n"
     "  return ("+t+")(int)acc; }\n",
     {0x45678u}, "OptStress264", 0, "-fno-math-errno"},

    // combined arithmetic + compare + convert into one accumulator.
    {p+"_ldmix",
     t+" "+p+"_ldmix("+t+" a){ unsigned h=(unsigned)a; long double acc=1;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    long double x=(long double)(int)(h&0xfffu);\n"
     "    acc=acc*0.5L + x*0.125L; if(acc>(long double)(int)((h>>12)&0xffu)) acc=acc-1.0L;\n"
     "    acc += (long double)((int)acc & 7); }\n"
     "  return ("+t+")(int)acc; }\n",
     {0x56789u}, "OptStress264", 0},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress264TC("x64o264", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress264TC("x86o264", "int");

INSTANTIATE_TEST_SUITE_P(OptStress264, X64OptStress264RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress264, X86OptStress264RT, ::testing::ValuesIn(kX86), rtTCName);
