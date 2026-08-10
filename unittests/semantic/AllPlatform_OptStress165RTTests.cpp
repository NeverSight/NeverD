//===- AllPlatform_OptStress165RTTests.cpp - ext-GCD / totient / divisor sum =//
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
//   * extgcd  - extended Euclidean algorithm on rodata pairs: track the Bezout
//               coefficients alongside the remainder chain and verify
//               `s*a + t*b == gcd`.  Pins a coefficient-carrying GCD with signed
//               arithmetic (distinct from the plain Euclid in #156 and binary
//               GCD in #145).
//   * totient - Euler's totient by counting integers coprime to a rodata modulus
//               (a GCD per candidate).  Pins a coprime census (distinct from any
//               factorization).
//   * divsum  - proper-divisor sum classification (perfect / abundant /
//               deficient) of rodata numbers via a sqrt-bounded divisor sweep.
//               Pins an aliquot-sum test (distinct from the primality sweep in
//               #156).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress165RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress165RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress165RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress165RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress165RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress165RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress165RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress165RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress165TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // extended Euclidean algorithm on rodata pairs (Bezout coefficients).
    {p+"_extgcd",
     "static const unsigned char "+p+"_ea[16]={0x9e,0x37,0xc1,0x5a,0x2f,0xe8,0x73,0x14,0xab,0x60,0xdd,0x06,0x99,0x42,0xbf,0x28};\n"
     "static const unsigned char "+p+"_eb[16]={0x4d,0xf2,0x81,0x3c,0xe0,0x57,0x6b,0xa9,0x18,0xd4,0x2e,0x95,0x70,0xc3,0x0f,0x88};\n"
     +t+" "+p+"_extgcd("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<16;q++){ int a2=(int)((((unsigned)"+p+"_ea[q]<<3)|((s>>(q&7))&7u))+1u), b2=(int)((((unsigned)"+p+"_eb[q]<<3)|((s>>((q>>1)&7))&7u))+1u);\n"
     "      int or_=a2,r=b2,os=1,s2=0,ot=0,t2=1;\n"
     "      while(r!=0){ int quo=or_/r; int tmp=or_-quo*r; or_=r; r=tmp; tmp=os-quo*s2; os=s2; s2=tmp; tmp=ot-quo*t2; ot=t2; t2=tmp; }\n"
     "      int chk=os*a2+ot*b2; acc=acc*131u+(unsigned)or_+(unsigned)(chk==or_?1:0)+(unsigned)(os+100000)+(unsigned)(ot+100000); }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x29u}, "OptStress165", 2},

    // Euler's totient by coprime census against a rodata modulus.
    {p+"_totient",
     "static const unsigned char "+p+"_tt[16]={0x3a,0x91,0x07,0xe5,0x6c,0xb8,0x4f,0x12,0xa3,0x5e,0xd0,0x29,0x7b,0xc6,0x84,0xfd};\n"
     +t+" "+p+"_totient("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<16;q++){ unsigned n=((unsigned)"+p+"_tt[q]&63u)+2u; unsigned cnt=0u;\n"
     "      for(unsigned k=1;k<=n;k++){ unsigned a2=n,b2=k; while(b2){ unsigned r=a2%b2; a2=b2; b2=r; } if(a2==1u) cnt++; }\n"
     "      acc=acc*131u+cnt; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x3Au}, "OptStress165", 2},

    // proper-divisor sum classification of rodata numbers (aliquot sum).
    {p+"_divsum",
     "static const unsigned char "+p+"_ds[16]={0x3a,0x91,0x07,0xe5,0x6c,0xb8,0x4f,0x12,0xa3,0x5e,0xd0,0x29,0x7b,0xc6,0x84,0xfd};\n"
     +t+" "+p+"_divsum("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<16;q++){ unsigned n=((unsigned)"+p+"_ds[q]&127u)+2u; unsigned sum=1u;\n"
     "      for(unsigned d=2u; d*d<=n; d++){ if(n%d==0u){ sum+=d; unsigned e=n/d; if(e!=d) sum+=e; } }\n"
     "      unsigned cls=(sum==n)?2u:((sum>n)?1u:0u); acc=acc*131u+sum+cls; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x6Bu}, "OptStress165", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress165TC("x64o165", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress165TC("x86o165", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress165TC("a64o165", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress165TC("armo165", "int");

INSTANTIATE_TEST_SUITE_P(OptStress165, X64OptStress165RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress165, X86OptStress165RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress165, A64OptStress165RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress165, ARM32OptStress165RT, ::testing::ValuesIn(kARM), rtTCName);
