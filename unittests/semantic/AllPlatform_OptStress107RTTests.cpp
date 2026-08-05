//===- AllPlatform_OptStress107RTTests.cpp - decimal / classify rodata shapes//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer), so none touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487) and every
// probe runs on all four targets.
//
//   * bcdadd - packed-BCD addition of two rodata operand arrays nibble by
//              nibble with decimal carry correction (`if(d>9) d-=10`).  Pins a
//              per-nibble carry recurrence over two indexed rodata loads.
//   * rngmap - threshold classification: a value is placed into a band by a
//              linear scan of an ascending rodata threshold table, then the band
//              gathers a rodata output table (`val[band]`).  Pins a scan-derived
//              index feeding a second rodata gather (piecewise-constant map).
//   * gcd    - Euclidean GCD of rodata operand pairs via the remainder loop
//              `t=x%y; x=y; y=t`.  Pins a data-driven variable-trip remainder
//              recurrence on indexed rodata loads (32-bit `%`, no libcall).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress107RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress107RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress107RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress107RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress107RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress107RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress107RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress107RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress107TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // packed-BCD addition with per-nibble decimal carry over two rodata operands.
    {p+"_bcdadd",
     "static const unsigned char "+p+"_x[16]={\n"
     "0x19,0x37,0x42,0x88,0x05,0x73,0x60,0x21, 0x94,0x46,0x18,0x52,0x37,0x09,0x85,0x70};\n"
     "static const unsigned char "+p+"_y[16]={\n"
     "0x28,0x55,0x13,0x09,0x91,0x34,0x47,0x82, 0x16,0x63,0x29,0x40,0x58,0x77,0x04,0x36};\n"
     +t+" "+p+"_bcdadd("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s, carry=s&1u;\n"
     "    for(int i=0;i<16;i++){\n"
     "      unsigned lo=("+p+"_x[i]&15u)+("+p+"_y[i]&15u)+carry;\n"
     "      unsigned c1=(lo>9u)?1u:0u; if(c1) lo-=10u;\n"
     "      unsigned hi=(("+p+"_x[i]>>4)&15u)+(("+p+"_y[i]>>4)&15u)+c1;\n"
     "      carry=(hi>9u)?1u:0u; if(carry) hi-=10u;\n"
     "      acc=acc*131u+((hi<<4)|lo); }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xBCu}, "OptStress107", 2},

    // threshold band classification, then a rodata output gather by band.
    {p+"_rngmap",
     "static const unsigned char "+p+"_thr[8]={10,30,55,80,110,150,200,250};\n"
     "static const unsigned char "+p+"_val[9]={3,9,17,28,41,60,82,113,150};\n"
     +t+" "+p+"_rngmap("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int k=0;k<32;k++){ unsigned v=(s>>(k&15))&0xFFu, band=0;\n"
     "      for(int b=0;b<8;b++) if(v>="+p+"_thr[b]) band=(unsigned)b+1u;\n"
     "      acc=acc*131u+"+p+"_val[band]; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x2Au}, "OptStress107", 2},

    // Euclidean GCD of rodata operand pairs via the remainder loop.
    {p+"_gcd",
     "static const unsigned char "+p+"_pairs[32]={\n"
     "84,36,120,90,48,18, 75,45,66,22,99,33, 60,84,140,56,28,70,\n"
     "150,25,72,108,16,40, 81,27,96,144,55,11,77,121};\n"
     +t+" "+p+"_gcd("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int i=0;i<16;i++){\n"
     "      unsigned x="+p+"_pairs[i*2]+(s&7u)+1u, y="+p+"_pairs[i*2+1]+((s>>8)&7u)+1u;\n"
     "      while(y){ unsigned tt=x%y; x=y; y=tt; }\n"
     "      acc=acc*131u+x; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x6Du}, "OptStress107", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress107TC("x64o107", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress107TC("x86o107", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress107TC("a64o107", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress107TC("armo107", "int");

INSTANTIATE_TEST_SUITE_P(OptStress107, X64OptStress107RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress107, X86OptStress107RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress107, A64OptStress107RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress107, ARM32OptStress107RT, ::testing::ValuesIn(kARM), rtTCName);
