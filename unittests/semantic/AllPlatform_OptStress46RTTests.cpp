//===- AllPlatform_OptStress46RTTests.cpp - byte/word divmod sub-regs -*-C++*-=//
//
// Roundtrip probes for 8- and 16-bit DIV/IDIV, whose quotient/remainder land in
// sub-registers that the existing DivMulEdge suite (32/64-bit divmod only) does
// not exercise.  On x86 `divb` writes the quotient to AL (offset 0) AND the
// remainder to AH (offset 8 of the parent AX/EAX), so reading both back (clang
// emits `movzbl %ah,...`) splits one parent register at two byte offsets — the
// trickier sub-register-aliasing case than the offset-0 AL reads that were the
// source of several historical optimizer miscompiles.  16-bit `divw` writes AX
// (quotient) and DX (remainder).  Quotient and remainder are both folded into a
// value-dependent hash, so a lift that mismodels the AH/DX sub-register split
// diverges.  Divisors are forced non-zero (and quotients bounded) so no #DE.
//
//   * dm8u    - 8-bit unsigned divmod, quotient (AL) + remainder (AH) hashed.
//   * dm8chain- 8-bit divmod whose remainder feeds the next dividend (AH chain).
//   * dm16u   - 16-bit unsigned divmod, quotient (AX) + remainder (DX) hashed.
//   * dm16chain- 16-bit divmod remainder-fed chain (DX dependency).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress46RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress46RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress46RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress46RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress46RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress46RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress46RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress46RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress46TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // dm8u: 8-bit unsigned divmod -> divb (AL quotient, AH remainder).
    {p+"_dm8u",
     t+" "+p+"_dm8u("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, acc=0;\n"
     "  for(int i=0;i<200;i++){\n"
     "    unsigned char x=(unsigned char)(s>>5), y=(unsigned char)(s|1u);\n"
     "    unsigned char q=(unsigned char)(x/y), r=(unsigned char)(x%y);\n"
     "    acc=acc*131u+(unsigned)q+((unsigned)r<<8);\n"
     "    s=s*1103515245u+12345u; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x50u}, "OptStress46", 2},

    // dm8chain: remainder (AH) of one step seeds the next dividend.
    {p+"_dm8chain",
     t+" "+p+"_dm8chain("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, acc=0; unsigned char carry=7;\n"
     "  for(int i=0;i<200;i++){\n"
     "    unsigned char x=(unsigned char)((s>>4)^carry), y=(unsigned char)(s|3u);\n"
     "    unsigned char q=(unsigned char)(x/y), r=(unsigned char)(x%y);\n"
     "    carry=r; acc=acc*131u+(unsigned)q+((unsigned)r<<7);\n"
     "    s=s*1103515245u+12345u; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x51u}, "OptStress46", 2},

    // dm16u: 16-bit unsigned divmod -> divw (AX quotient, DX remainder).
    {p+"_dm16u",
     t+" "+p+"_dm16u("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, acc=0;\n"
     "  for(int i=0;i<200;i++){\n"
     "    unsigned short x=(unsigned short)(s>>9), y=(unsigned short)(s|1u);\n"
     "    unsigned short q=(unsigned short)(x/y), r=(unsigned short)(x%y);\n"
     "    acc=acc*131u+(unsigned)q+((unsigned)r<<16);\n"
     "    s=s*1103515245u+12345u; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x52u}, "OptStress46", 2},

    // dm16chain: 16-bit divmod remainder (DX) feeds the next dividend.
    {p+"_dm16chain",
     t+" "+p+"_dm16chain("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, acc=0; unsigned short carry=257;\n"
     "  for(int i=0;i<200;i++){\n"
     "    unsigned short x=(unsigned short)((s>>7)^carry), y=(unsigned short)(s|5u);\n"
     "    unsigned short q=(unsigned short)(x/y), r=(unsigned short)(x%y);\n"
     "    carry=r; acc=acc*131u+(unsigned)q+((unsigned)r<<11);\n"
     "    s=s*1103515245u+12345u; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x53u}, "OptStress46", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress46TC("x64o46", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress46TC("x86o46", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress46TC("a64o46", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress46TC("armo46", "int");

INSTANTIATE_TEST_SUITE_P(OptStress46, X64OptStress46RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress46, X86OptStress46RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress46, A64OptStress46RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress46, ARM32OptStress46RT, ::testing::ValuesIn(kARM), rtTCName);
