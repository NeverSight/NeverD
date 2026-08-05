//===- AllPlatform_OptStress213RTTests.cpp - 64-bit div/mod, no libcall ===//
//
// Guards the recurring "RDX:RAX-form 64-bit divide lifted to i128" miscompile:
// x86-64 `int64_t/int64_t` is `cqo; idiv r64` and `uint64_t/uint64_t` is
// `xor edx,edx; div r64` — the RDX half is only sign-/zero-extension of the
// dividend, NOT a true 128-bit numerator.  Modelling it as an i128 sdiv/srem
// pulls in `__divti3`/`__modti3`, which do not exist in the bare Unicorn image;
// the lift must keep a native i64 sdiv/udiv/srem/urem.  All operands are runtime
// values (LCG-seeded, divisor forced odd so never zero) so the divide cannot be
// constant-folded to a magic multiply.
//
//   * sdiv64   - loop-carried signed 64-bit division (cqo; idiv).
//   * udiv64   - loop-carried unsigned 64-bit division (xor edx; div).
//   * divmod64 - quotient AND remainder of one 64-bit division both consumed.
//   * divneg64 - signed 64-bit division spanning negative dividends/divisors.
//
// Only x86-64 and AArch64 are instantiated: i386 and ARM32 lower a *variable*
// 64-bit divide to `__divdi3`/`__aeabi_ldivmod` libcalls, which the bare-metal
// roundtrip harness cannot link, so they are out of scope here (32-bit divides
// are covered by OptStress202 `divmod`).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress213RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress213RT, Verify) { roundTripX64(GetParam()); }
class A64OptStress213RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress213RT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress213TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Loop-carried signed 64-bit division (cqo; idiv r64 → must stay i64 sdiv).
    {p+"_sdiv64",
     t+" "+p+"_sdiv64("+t+" a){\n"
     "  long long acc=(long long)a^0x5A5A5A5AA5A5A5A5LL;\n"
     "  unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<96;i++){ s=s*1103515245u+12345u;\n"
     "    long long d=(long long)((unsigned long long)s*0x9E3779B1ULL)|1LL;\n"
     "    acc=(acc+(long long)(int)s)/d;\n"
     "    acc^=(long long)s<<13; acc+=0x123456789ABCDEFLL; }\n"
     "  return ("+t+")(acc^(acc>>32)); }\n",
     {0x71u}, "OptStress213", 2},

    // Loop-carried unsigned 64-bit division (xor edx; div r64 → must stay i64 udiv).
    {p+"_udiv64",
     t+" "+p+"_udiv64("+t+" a){\n"
     "  unsigned long long acc=(unsigned long long)a*0xD1B54A32D192ED03ULL+1ULL;\n"
     "  unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<96;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned long long d=((unsigned long long)s*0x100000001B3ULL)|1ULL;\n"
     "    acc=(acc+(unsigned long long)s)/d;\n"
     "    acc^=acc>>17; acc=acc*6364136223846793005ULL+s; }\n"
     "  return ("+t+")(acc^(acc>>32)); }\n",
     {0x72u}, "OptStress213", 2},

    // Quotient and remainder of one division both consumed (single idiv recovery).
    {p+"_divmod64",
     t+" "+p+"_divmod64("+t+" a){\n"
     "  unsigned long long acc=(unsigned long long)a^0xABCDEF0123456789ULL;\n"
     "  unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<96;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned long long d=((unsigned long long)s<<7)|0x101ULL;\n"
     "    unsigned long long q=acc/d, r=acc%d;\n"
     "    acc=q*131ULL+r+((unsigned long long)s<<20); }\n"
     "  return ("+t+")(acc^(acc>>32)); }\n",
     {0x73u}, "OptStress213", 2},

    // Signed 64-bit division over negative dividends/divisors (sign-handling).
    {p+"_divneg64",
     t+" "+p+"_divneg64("+t+" a){\n"
     "  long long acc=-(long long)a-0x3FFFFFFFFFFFLL;\n"
     "  unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<96;i++){ s=s*1103515245u+12345u;\n"
     "    long long d=(long long)(int)s; d=(d|1);\n"
     "    long long n=acc-((long long)s<<11);\n"
     "    acc=n/d + n%d;\n"
     "    acc-=0x55555555LL; }\n"
     "  return ("+t+")(acc^(acc>>32)); }\n",
     {0x74u}, "OptStress213", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress213TC("x64o213", "long");
static const std::vector<RoundTripTC> kA64 = makeOptStress213TC("a64o213", "long");

INSTANTIATE_TEST_SUITE_P(OptStress213, X64OptStress213RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress213, A64OptStress213RT, ::testing::ValuesIn(kA64), rtTCName);
