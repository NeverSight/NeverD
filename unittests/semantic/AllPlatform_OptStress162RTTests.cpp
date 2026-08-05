//===- AllPlatform_OptStress162RTTests.cpp - Roman / double-dabble / base cvt =//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer), so none touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487) and every
// probe runs on all four targets.
//
//   * roman   - greedy subtractive Roman-numeral conversion of rodata numbers:
//               thirteen descending values are divided out to accumulate symbol
//               counts.  Pins a greedy denomination peel (distinct from any
//               positional base conversion).
//   * bcd     - binary-to-BCD by the double-dabble algorithm over rodata words:
//               shift left bit by bit, adding three to any BCD nibble >= 5.  Pins
//               a shift-add-3 packed-decimal conversion (distinct from any
//               divide-by-ten digit extraction).
//   * basecvt - arbitrary-base digit extraction of rodata numbers and immediate
//               reconstruction to verify the round-trip.  Pins a radix
//               extract/rebuild (distinct from the double-dabble shift form
//               above and the digit root in #153).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress162RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress162RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress162RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress162RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress162RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress162RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress162RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress162RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress162TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // greedy subtractive Roman-numeral conversion of rodata numbers.
    {p+"_roman",
     "static const unsigned char "+p+"_rm[16]={0x3a,0x91,0x07,0xe5,0x6c,0xb8,0x4f,0x12,0xa3,0x5e,0xd0,0x29,0x7b,0xc6,0x84,0xfd};\n"
     +t+" "+p+"_roman("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<16;q++){ unsigned n=(((unsigned)"+p+"_rm[q]<<2)|((s>>(q&7))&3u)); unsigned sym=0u,chk=0u,c;\n"
     "      c=n/1000u;n%=1000u;sym+=c;chk=chk*31u+c; c=n/900u;n%=900u;sym+=c;chk=chk*31u+c;\n"
     "      c=n/500u;n%=500u;sym+=c;chk=chk*31u+c; c=n/400u;n%=400u;sym+=c;chk=chk*31u+c;\n"
     "      c=n/100u;n%=100u;sym+=c;chk=chk*31u+c; c=n/90u;n%=90u;sym+=c;chk=chk*31u+c;\n"
     "      c=n/50u;n%=50u;sym+=c;chk=chk*31u+c; c=n/40u;n%=40u;sym+=c;chk=chk*31u+c;\n"
     "      c=n/10u;n%=10u;sym+=c;chk=chk*31u+c; c=n/9u;n%=9u;sym+=c;chk=chk*31u+c;\n"
     "      c=n/5u;n%=5u;sym+=c;chk=chk*31u+c; c=n/4u;n%=4u;sym+=c;chk=chk*31u+c;\n"
     "      c=n/1u;n%=1u;sym+=c;chk=chk*31u+c; acc=acc*131u+sym+chk; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x26u}, "OptStress162", 2},

    // binary-to-BCD by double-dabble over rodata words (shift + add-3).
    {p+"_bcd",
     "static const unsigned char "+p+"_bd[16]={0x3a,0x91,0x07,0xe5,0x6c,0xb8,0x4f,0x12,0xa3,0x5e,0xd0,0x29,0x7b,0xc6,0x84,0xfd};\n"
     +t+" "+p+"_bcd("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<16;q++){ unsigned bin=(((unsigned)"+p+"_bd[q]<<4)|((s>>(q&7))&15u))&0xFFFu; unsigned bcd=0u;\n"
     "      for(int i=11;i>=0;i--){ for(int d=0;d<4;d++){ unsigned nib=(bcd>>(d*4))&0xFu; if(nib>=5u) bcd+=(3u<<(d*4)); }\n"
     "        bcd=(bcd<<1)|((bin>>i)&1u); }\n"
     "      acc=acc*131u+bcd; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x37u}, "OptStress162", 2},

    // arbitrary-base digit extraction + reconstruction of rodata numbers.
    {p+"_basecvt",
     "static const unsigned char "+p+"_bv[16]={0x3a,0x91,0x07,0xe5,0x6c,0xb8,0x4f,0x12,0xa3,0x5e,0xd0,0x29,0x7b,0xc6,0x84,0xfd};\n"
     +t+" "+p+"_basecvt("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<16;q++){ unsigned n=(((unsigned)"+p+"_bv[q]<<4)|((s>>(q&7))&15u)); unsigned base=((unsigned)"+p+"_bv[(q+1)&15]&7u)+2u;\n"
     "      unsigned digits[16]; int nd=0; unsigned m=n;\n"
     "      if(m==0u){ digits[nd++]=0u; } else { while(m>0u && nd<16){ digits[nd++]=m%base; m/=base; } }\n"
     "      unsigned recon=0u; for(int i=nd-1;i>=0;i--) recon=recon*base+digits[i];\n"
     "      acc=acc*131u+recon+(unsigned)nd+((recon==n)?1u:0u); }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x68u}, "OptStress162", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress162TC("x64o162", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress162TC("x86o162", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress162TC("a64o162", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress162TC("armo162", "int");

INSTANTIATE_TEST_SUITE_P(OptStress162, X64OptStress162RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress162, X86OptStress162RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress162, A64OptStress162RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress162, ARM32OptStress162RT, ::testing::ValuesIn(kARM), rtTCName);
