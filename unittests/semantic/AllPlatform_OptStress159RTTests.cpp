//===- AllPlatform_OptStress159RTTests.cpp - Newton sqrt / bit-ceil / Josephus =//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer), so none touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487) and every
// probe runs on all four targets.
//
//   * newton  - integer square root of rodata operands by Newton's iteration
//               `x = (x + n/x)/2` until it stops decreasing.  Pins a
//               division-based convergent root (distinct from the bit-by-bit
//               restoring isqrt in #145).
//   * bitceil - round rodata values up to the next power of two by bit-smearing
//               `v |= v>>k` then `+1`, plus a trailing-zero log.  Pins a
//               saturating bit-smear (distinct from the Gray/bit-reversal
//               transforms and any table lookup).
//   * josephus- Josephus survivor position for rodata `(n,k)` via the recurrence
//               `r = (r+k) % i`.  Pins a circular-elimination fold (distinct from
//               any linked-list or array removal walk).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress159RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress159RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress159RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress159RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress159RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress159RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress159RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress159RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress159TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // integer square root of rodata operands by Newton's iteration.
    {p+"_newton",
     "static const unsigned char "+p+"_nw[16]={0x3a,0x91,0x07,0xe5,0x6c,0xb8,0x4f,0x12,0xa3,0x5e,0xd0,0x29,0x7b,0xc6,0x84,0xfd};\n"
     +t+" "+p+"_newton("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<16;q++){ unsigned n=(((unsigned)"+p+"_nw[q]<<8)|((s>>(q&7))&0xFFu)); if(n==0u){ acc=acc*131u; continue; }\n"
     "      unsigned x=n,y=(x+1u)/2u; while(y<x){ x=y; y=(x+n/x)/2u; } acc=acc*131u+x; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x23u}, "OptStress159", 2},

    // round rodata values up to the next power of two (bit-smear + ctz log).
    {p+"_bitceil",
     "static const unsigned char "+p+"_bc[16]={0x3a,0x91,0x07,0xe5,0x6c,0xb8,0x4f,0x12,0xa3,0x5e,0xd0,0x29,0x7b,0xc6,0x84,0xfd};\n"
     +t+" "+p+"_bitceil("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<16;q++){ unsigned n=(((unsigned)"+p+"_bc[q]<<6)|((s>>(q&7))&0x3Fu)); if(n==0u) n=1u;\n"
     "      unsigned v=n-1u; v|=v>>1; v|=v>>2; v|=v>>4; v|=v>>8; v|=v>>16; v++;\n"
     "      unsigned tz=0u,w=v; while(!(w&1u)){ w>>=1; tz++; } acc=acc*131u+v+tz; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x34u}, "OptStress159", 2},

    // Josephus survivor position for rodata (n,k) via r=(r+k)%i recurrence.
    {p+"_josephus",
     "static const unsigned char "+p+"_jn[16]={5,9,3,12,7,15,2,11,8,4,14,6,10,13,1,16};\n"
     "static const unsigned char "+p+"_jk[16]={2,3,1,4,2,5,3,1,6,2,4,7,1,3,2,5};\n"
     +t+" "+p+"_josephus("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<16;q++){ unsigned n=((unsigned)"+p+"_jn[q]&15u)+2u, k=((unsigned)"+p+"_jk[q]&7u)+1u;\n"
     "      unsigned res=0u; for(unsigned i=2u;i<=n;i++) res=(res+k)%i; acc=acc*131u+res+n+k; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x65u}, "OptStress159", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress159TC("x64o159", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress159TC("x86o159", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress159TC("a64o159", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress159TC("armo159", "int");

INSTANTIATE_TEST_SUITE_P(OptStress159, X64OptStress159RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress159, X86OptStress159RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress159, A64OptStress159RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress159, ARM32OptStress159RT, ::testing::ValuesIn(kARM), rtTCName);
