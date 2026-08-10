//===- AllPlatform_OptStress153RTTests.cpp - Collatz / Fib-mod / digit root =//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer), so none touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487) and every
// probe runs on all four targets.
//
//   * collatz - Collatz hailstone step counts for rodata seeds: halve when even,
//               `3n+1` when odd, until reaching one (bounded).  Pins a
//               data-dependent convergent iteration (distinct from any fixed-trip
//               count loop).
//   * fibmod  - Fibonacci value at a rodata index taken modulo a rodata modulus:
//               a two-register additive recurrence stepped k times.  Pins a
//               linear two-term recurrence under a modulus (distinct from the
//               geometric square-and-multiply modexp in #145).
//   * digroot - additive digit-root reduction of rodata numbers: repeatedly
//               replace a number by its base-10 digit sum until a single digit
//               remains, counting the rounds.  Pins a digit-sum fixpoint
//               (distinct from any single radix conversion).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress153RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress153RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress153RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress153RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress153RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress153RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress153RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress153RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress153TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Collatz hailstone step counts for rodata seeds (bounded 3n+1 / n>>1).
    {p+"_collatz",
     "static const unsigned char "+p+"_cl[16]={27,5,11,33,7,19,3,45,9,21,15,55,13,37,1,63};\n"
     +t+" "+p+"_collatz("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<16;q++){ unsigned n=((unsigned)"+p+"_cl[q]|1u)+((s>>(q&7))&7u); if(n==0u) n=1u;\n"
     "      unsigned steps=0u;\n"
     "      while(n!=1u && steps<128u){ if(n&1u){ if(n>0x20000000u) break; n=3u*n+1u; } else n>>=1; steps++; }\n"
     "      acc=acc*131u+steps+n; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x1Du}, "OptStress153", 2},

    // Fibonacci index from rodata, value modulo a rodata modulus (two-term rec).
    {p+"_fibmod",
     "static const unsigned char "+p+"_fk[16]={5,9,3,12,7,15,2,18,11,4,20,8,14,6,17,10};\n"
     "static const unsigned char "+p+"_fm[16]={7,11,5,13,9,17,3,19,23,6,29,15,2,21,4,12};\n"
     +t+" "+p+"_fibmod("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<16;q++){ unsigned k=((unsigned)"+p+"_fk[q]&31u)+((s>>(q&7))&7u); unsigned m=((unsigned)"+p+"_fm[q]|2u);\n"
     "      unsigned x=0u,y=1u; for(unsigned z=0;z<k;z++){ unsigned tt=(x+y)%m; x=y; y=tt; }\n"
     "      acc=acc*131u+x; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x2Eu}, "OptStress153", 2},

    // additive digit-root reduction of rodata numbers (digit-sum fixpoint).
    {p+"_digroot",
     "static const unsigned char "+p+"_dr[16]={0x3a,0x91,0x07,0xe5,0x6c,0xb8,0x4f,0x12,0xa3,0x5e,0xd0,0x29,0x7b,0xc6,0x84,0xfd};\n"
     +t+" "+p+"_digroot("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<16;q++){ unsigned n=((unsigned)"+p+"_dr[q]<<4)|((s>>(q&7))&15u); unsigned iters=0u;\n"
     "      while(n>=10u){ unsigned tt=0u,m=n; while(m>0u){ tt+=m%10u; m/=10u; } n=tt; iters++; }\n"
     "      acc=acc*131u+n+iters; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x5Fu}, "OptStress153", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress153TC("x64o153", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress153TC("x86o153", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress153TC("a64o153", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress153TC("armo153", "int");

INSTANTIATE_TEST_SUITE_P(OptStress153, X64OptStress153RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress153, X86OptStress153RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress153, A64OptStress153RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress153, ARM32OptStress153RT, ::testing::ValuesIn(kARM), rtTCName);
