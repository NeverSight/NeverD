//===- AllPlatform_OptStress156RTTests.cpp - Euclid GCD / CRT / is-prime =//
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
//   * euclid  - Euclidean GCD of rodata operand pairs via the remainder loop
//               `(a,b) -> (b, a%b)`.  Pins a modulo-based GCD (distinct from the
//               shift-and-subtract binary GCD in #145).
//   * crt     - Chinese-remainder solve for two rodata congruences by scanning
//               `[0, m1*m2)` for the residue that matches both.  Pins a
//               brute-force simultaneous-congruence search (distinct from any
//               single modular reduce).
//   * isprime - trial-division primality of rodata numbers, dividing by every
//               `d` up to `sqrt(n)`.  Pins a divisor-sweep primality test
//               (distinct from any sieve or factor table).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress156RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress156RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress156RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress156RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress156RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress156RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress156RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress156RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress156TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Euclidean GCD of rodata operand pairs (remainder loop).
    {p+"_euclid",
     "static const unsigned char "+p+"_ea[16]={0x9e,0x37,0xc1,0x5a,0x2f,0xe8,0x73,0x14,0xab,0x60,0xdd,0x06,0x99,0x42,0xbf,0x28};\n"
     "static const unsigned char "+p+"_eb[16]={0x4d,0xf2,0x81,0x3c,0xe0,0x57,0x6b,0xa9,0x18,0xd4,0x2e,0x95,0x70,0xc3,0x0f,0x88};\n"
     +t+" "+p+"_euclid("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<16;q++){ unsigned a2=(((unsigned)"+p+"_ea[q]<<4)|((s>>(q&7))&15u))+1u, b2=(((unsigned)"+p+"_eb[q]<<4)|((s>>((q>>1)&7))&15u))+1u;\n"
     "      while(b2){ unsigned tt=a2%b2; a2=b2; b2=tt; } acc=acc*131u+a2; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x20u}, "OptStress156", 2},

    // Chinese-remainder solve for two rodata congruences (residue search).
    {p+"_crt",
     "static const unsigned char "+p+"_cr[8]={2,5,1,3,4,0,6,2};\n"
     "static const unsigned char "+p+"_cm[8]={3,4,5,2,6,1,3,5};\n"
     +t+" "+p+"_crt("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<8;q++){ unsigned m1=((unsigned)"+p+"_cm[q]&7u)+2u, r1=(unsigned)"+p+"_cr[q]%m1;\n"
     "      unsigned m2=((unsigned)"+p+"_cm[(q+1)&7]&7u)+2u, r2=(unsigned)"+p+"_cr[(q+1)&7]%m2;\n"
     "      unsigned lim=m1*m2, found=lim; for(unsigned v=0;v<lim;v++){ if(v%m1==r1 && v%m2==r2){ found=v; break; } }\n"
     "      acc=acc*131u+found+m1+m2; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x31u}, "OptStress156", 2},

    // trial-division primality of rodata numbers (divisor sweep to sqrt).
    {p+"_isprime",
     "static const unsigned char "+p+"_ip[16]={0x3a,0x91,0x07,0xe5,0x6c,0xb8,0x4f,0x12,0xa3,0x5e,0xd0,0x29,0x7b,0xc6,0x84,0xfd};\n"
     +t+" "+p+"_isprime("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<16;q++){ unsigned n=(((unsigned)"+p+"_ip[q]<<3)|((s>>(q&7))&7u))+2u; unsigned isp=1u;\n"
     "      for(unsigned d=2u; d*d<=n; d++){ if(n%d==0u){ isp=0u; break; } }\n"
     "      acc=acc*131u+isp+(isp?n:0u); }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x62u}, "OptStress156", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress156TC("x64o156", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress156TC("x86o156", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress156TC("a64o156", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress156TC("armo156", "int");

INSTANTIATE_TEST_SUITE_P(OptStress156, X64OptStress156RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress156, X86OptStress156RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress156, A64OptStress156RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress156, ARM32OptStress156RT, ::testing::ValuesIn(kARM), rtTCName);
