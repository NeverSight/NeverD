//===- AllPlatform_OptStress41RTTests.cpp - bit-op insn selection -*-C++*-=//
//
// Roundtrip probes for instruction-selection corners around bit manipulation
// and flag-producing arithmetic that earlier OptStress rounds did not target
// directly.  Each kernel is a bounded -O2 loop returning a value-dependent hash
// so any lift/optimizer/codegen divergence shows as a return mismatch; all are
// libcall-free on every target (no variable 64-bit shift or 64-bit divide).
//
//   * bitops  - variable-position bit test/set/clear/toggle -> x86 BT/BTS/BTR/
//               BTC (bit index taken mod operand size), ARM shift+and/orr/bic/eor.
//   * clztz   - __builtin_clz/ctz behind a zero guard -> BSR/BSF/LZCNT/TZCNT,
//               AArch64/ARM CLZ(+RBIT); the guard branch stresses flag modeling.
//   * adc64   - 64-bit add/sub/mul/compare chain -> carry/borrow across the
//               register pair on 32-bit targets (libcall-free).
//   * ovf     - __builtin_add/mul_overflow driving branches -> the OF/CF result
//               of add/imul consumed by a conditional (MedFlags overflow path).
//   * rotmix  - __builtin_rotateleft/right with a runtime count -> ROL/ROR and
//               the (32-n) masked back-shift, plus a SHLD-style funnel.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress41RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress41RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress41RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress41RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress41RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress41RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress41RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress41RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress41TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // bitops: variable-position test/set/clear/toggle (x86 BT/BTS/BTR/BTC).
    {p+"_bitops",
     t+" "+p+"_bitops("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u, acc=0;\n"
     "  for(int i=0;i<200;i++){\n"
     "    unsigned n=(x>>3)&31u;\n"
     "    if(x & (1u<<n)) acc+=3u; else acc-=1u;\n"
     "    x |= (1u<<n);\n"
     "    n=(x>>7)&31u;  x &= ~(1u<<n);\n"
     "    n=(x>>11)&31u; x ^= (1u<<n);\n"
     "    acc=acc*131u + x;\n"
     "    x=x*1103515245u+12345u; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x1Bu}, "OptStress41", 2},

    // clztz: clz/ctz behind a zero guard (BSR/BSF/LZCNT/TZCNT / CLZ+RBIT).
    {p+"_clztz",
     t+" "+p+"_clztz("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u, acc=0;\n"
     "  for(int i=0;i<200;i++){\n"
     "    unsigned tz = x ? (unsigned)__builtin_ctz(x) : 32u;\n"
     "    unsigned lz = x ? (unsigned)__builtin_clz(x) : 32u;\n"
     "    acc += tz*7u + lz*5u;\n"
     "    if(tz > lz) acc ^= 0xAAu; else acc += 1u;\n"
     "    x = x*1103515245u+12345u + (acc&7u); }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x2Cu}, "OptStress41", 2},

    // adc64: 64-bit add/sub/mul/compare chain (carry/borrow on 32-bit pairs).
    {p+"_adc64",
     t+" "+p+"_adc64("+t+" a){\n"
     "  unsigned long long x=(unsigned long long)(unsigned)a|1ull;\n"
     "  unsigned long long y=x ^ 0x9E3779B97F4A7C15ull, acc=0;\n"
     "  for(int i=0;i<200;i++){\n"
     "    acc += x + y;\n"
     "    acc -= (x ^ y);\n"
     "    if(x < y) acc += 0x100000001ull; else acc ^= y;\n"
     "    x = x*6364136223846793005ull + 1442695040888963407ull;\n"
     "    y = y + acc; }\n"
     "  return ("+t+")(unsigned)(acc ^ (acc>>32)); }\n",
     {0x3Du}, "OptStress41", 2},

    // ovf: __builtin_add/mul_overflow results drive branches (OF/CF path).
    {p+"_ovf",
     t+" "+p+"_ovf("+t+" a){\n"
     "  int x=(int)a|1, acc=0;\n"
     "  for(int i=0;i<200;i++){\n"
     "    int r;\n"
     "    if(__builtin_add_overflow(x, acc, &r)) acc=(acc>>1)^0x5555; else acc=r;\n"
     "    if(__builtin_mul_overflow(x, 3, &r)) acc-=7; else acc+=r & 0xFF;\n"
     "    x = x*1103515245+12345; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x4Eu}, "OptStress41", 2},

    // rotmix: runtime-count rotate (ROL/ROR) plus a SHLD-style funnel shift.
    {p+"_rotmix",
     t+" "+p+"_rotmix("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u, y=x^0xDEADBEEFu, acc=0;\n"
     "  for(int i=0;i<200;i++){\n"
     "    unsigned n=((x>>5)&31u)|1u;\n"
     "    unsigned rl=__builtin_rotateleft32(x,n);\n"
     "    unsigned rr=__builtin_rotateright32(y,n);\n"
     "    unsigned fn=(x<<n)|(y>>(32u-n));\n"
     "    acc += rl ^ rr ^ fn;\n"
     "    x=x*1103515245u+12345u; y=y+acc; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x5Fu}, "OptStress41", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress41TC("x64o41", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress41TC("x86o41", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress41TC("a64o41", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress41TC("armo41", "int");

INSTANTIATE_TEST_SUITE_P(OptStress41, X64OptStress41RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress41, X86OptStress41RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress41, A64OptStress41RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress41, ARM32OptStress41RT, ::testing::ValuesIn(kARM), rtTCName);
