//===- AllPlatform_OptStress134RTTests.cpp - LFSR / table popcount / Morton =//
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
//   * lfsr   - Galois/Fibonacci-style linear feedback shift register whose tap
//              positions come from a rodata array; the XOR-feedback stream is
//              folded.  Pins a bit-feedback recurrence indexed by rodata taps
//              (distinct from any arithmetic recurrence).
//   * popcnt - population count via a rodata nibble-popcount lookup table summed
//              over LCG words.  Pins a table-driven bit count (distinct from a
//              hardware popcount or shift-and-add loop).
//   * morton - Morton / Z-order bit interleave using a rodata nibble-spread
//              table.  Pins a bit-deposit gather `spread[lo]|spread[hi]<<8`
//              (distinct from the Gray-code transform in #127).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress134RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress134RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress134RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress134RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress134RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress134RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress134RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress134RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress134TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // LFSR with rodata-driven tap positions, XOR-feedback stream folded.
    {p+"_lfsr",
     "static const unsigned char "+p+"_taps[6]={0,2,3,5,11,15};\n"
     +t+" "+p+"_lfsr("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned reg=(s|1u)&0xFFFFu;\n"
     "    for(int i=0;i<40;i++){ unsigned bit=0u;\n"
     "      for(int k=0;k<6;k++) bit^=(reg>>("+p+"_taps[k]&15u))&1u;\n"
     "      reg=((reg<<1)|(bit&1u))&0xFFFFu; acc=acc*131u+reg; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x2Bu}, "OptStress134", 2},

    // table-driven population count via a rodata nibble-popcount LUT.
    {p+"_popcnt",
     "static const unsigned char "+p+"_pc[16]={0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4};\n"
     +t+" "+p+"_popcnt("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s, w=s;\n"
     "    for(int q=0;q<32;q++){ w=w*1103515245u+12345u; unsigned x=w^((unsigned)q*2654435761u);\n"
     "      unsigned c=0u; for(int n=0;n<8;n++) c+="+p+"_pc[(x>>(n*4))&15u];\n"
     "      acc=acc*131u+c+(x&7u); }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x5Du}, "OptStress134", 2},

    // Morton / Z-order bit interleave via a rodata nibble-spread table.
    {p+"_morton",
     "static const unsigned char "+p+"_spread[16]={\n"
     "0x00,0x01,0x04,0x05,0x10,0x11,0x14,0x15, 0x40,0x41,0x44,0x45,0x50,0x51,0x54,0x55};\n"
     +t+" "+p+"_morton("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<48;q++){ unsigned xx=(s>>(q&7))&0xFFu, yy=(s>>((q+3)&7))&0xFFu;\n"
     "      unsigned mx="+p+"_spread[xx&15u]|("+p+"_spread[(xx>>4)&15u]<<8);\n"
     "      unsigned my="+p+"_spread[yy&15u]|("+p+"_spread[(yy>>4)&15u]<<8);\n"
     "      unsigned m=mx|(my<<1); acc=acc*131u+m; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x66u}, "OptStress134", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress134TC("x64o134", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress134TC("x86o134", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress134TC("a64o134", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress134TC("armo134", "int");

INSTANTIATE_TEST_SUITE_P(OptStress134, X64OptStress134RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress134, X86OptStress134RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress134, A64OptStress134RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress134, ARM32OptStress134RT, ::testing::ValuesIn(kARM), rtTCName);
