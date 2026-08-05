//===- AllPlatform_OptStress145RTTests.cpp - binGCD / isqrt / modexp =//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer), so none touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487) and every
// probe runs on all four targets.
//
//   * bingcd - Stein's binary GCD of rodata operand pairs: strip the common
//              power of two, force each operand odd, then repeatedly subtract the
//              smaller from the larger and re-halve.  Pins a shift-and-subtract
//              GCD (distinct from any modulo-based Euclidean reduction).
//   * isqrt  - integer square root of rodata operands by the base-two
//              digit-by-digit method: one sweeping bit tests `res+bit` against a
//              running remainder, no multiply and no divide.  Pins a restoring
//              digit recurrence (distinct from any Newton division iterate).
//   * modexp - modular exponentiation of rodata operands by square-and-multiply:
//              the exponent is consumed bit by bit, squaring the running base and
//              conditionally folding it into the result under a small modulus.
//              Pins a binary-exponentiation reduce loop (distinct from the
//              Karatsuba split-multiply in #143 and the Booth shift-add in #121).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress145RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress145RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress145RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress145RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress145RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress145RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress145RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress145RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress145TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Stein's binary GCD of rodata operand pairs (shift + subtract).
    {p+"_bingcd",
     "static const unsigned char "+p+"_xa[16]={0x9e,0x37,0xc1,0x5a,0x2f,0xe8,0x73,0x14,0xab,0x60,0xdd,0x06,0x99,0x42,0xbf,0x28};\n"
     "static const unsigned char "+p+"_xb[16]={0x4d,0xf2,0x81,0x3c,0xe0,0x57,0x6b,0xa9,0x18,0xd4,0x2e,0x95,0x70,0xc3,0x0f,0x88};\n"
     +t+" "+p+"_bingcd("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<16;q++){\n"
     "      unsigned u=((unsigned)"+p+"_xa[q]<<4)|((s>>(q&7))&15u);\n"
     "      unsigned v=((unsigned)"+p+"_xb[q]<<4)|((s>>((q>>1)&7))&15u);\n"
     "      if(u==0u||v==0u){ acc=acc*131u+(u|v); continue; }\n"
     "      unsigned sh=0u;\n"
     "      while(((u|v)&1u)==0u){ u>>=1; v>>=1; sh++; }\n"
     "      while((u&1u)==0u) u>>=1;\n"
     "      do { while((v&1u)==0u) v>>=1;\n"
     "        if(u>v){ unsigned tmp=u; u=v; v=tmp; }\n"
     "        v=v-u;\n"
     "      } while(v!=0u);\n"
     "      acc=acc*131u+(u<<sh); }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x15u}, "OptStress145", 2},

    // integer square root of rodata operands (base-two digit-by-digit).
    {p+"_isqrt",
     "static const unsigned char "+p+"_v[16]={0x3a,0x91,0x07,0xe5,0x6c,0xb8,0x4f,0x12,0xa3,0x5e,0xd0,0x29,0x7b,0xc6,0x84,0xfd};\n"
     +t+" "+p+"_isqrt("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<32;q++){\n"
     "      unsigned n=((unsigned)"+p+"_v[q&15]<<8)|((s>>(q&7))&0xFFu);\n"
     "      unsigned res=0u, bit=1u<<14, num=n;\n"
     "      while(bit>num) bit>>=2;\n"
     "      while(bit!=0u){\n"
     "        if(num>=res+bit){ num=num-(res+bit); res=(res>>1)+bit; }\n"
     "        else res>>=1;\n"
     "        bit>>=2; }\n"
     "      acc=acc*131u+res*131u+num; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x26u}, "OptStress145", 2},

    // modular exponentiation of rodata operands (square-and-multiply).
    {p+"_modexp",
     "static const unsigned char "+p+"_b[16]={0x07,0x1d,0x3b,0x52,0x6a,0x84,0x9f,0xb3,0xc1,0xd6,0xe8,0xf5,0x11,0x29,0x4c,0x73};\n"
     "static const unsigned char "+p+"_e[16]={0x53,0xa1,0x0c,0xf8,0x37,0x6e,0x9d,0x42,0xbb,0x15,0xc4,0x80,0x2a,0xe7,0x59,0x91};\n"
     "static const unsigned char "+p+"_m[16]={0x65,0x2c,0xd0,0x47,0x9a,0x13,0xf6,0x88,0x3e,0x71,0xac,0x05,0xc9,0x52,0xbd,0x20};\n"
     +t+" "+p+"_modexp("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<16;q++){\n"
     "      unsigned base=((unsigned)"+p+"_b[q]|1u);\n"
     "      unsigned e=((unsigned)"+p+"_e[q]^((s>>(q&7))&0xFFu));\n"
     "      unsigned mod=((unsigned)"+p+"_m[q]|1u)+1u;\n"
     "      unsigned r=1u, b=base%mod;\n"
     "      while(e>0u){ if(e&1u) r=(r*b)%mod; b=(b*b)%mod; e>>=1; }\n"
     "      acc=acc*131u+r+base; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x37u}, "OptStress145", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress145TC("x64o145", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress145TC("x86o145", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress145TC("a64o145", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress145TC("armo145", "int");

INSTANTIATE_TEST_SUITE_P(OptStress145, X64OptStress145RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress145, X86OptStress145RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress145, A64OptStress145RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress145, ARM32OptStress145RT, ::testing::ValuesIn(kARM), rtTCName);
