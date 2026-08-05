//===- AllPlatform_OptStress157RTTests.cpp - popcount / base64 / Hamming =//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer), so none touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487) and every
// probe runs on all four targets.
//
//   * popcnt  - population count + parity of rodata words via the Kernighan
//               `x &= x-1` bit-clear loop.  Pins a set-bit-stripping count
//               (distinct from any table or hardware popcount).
//   * b64     - base64-style encode of a rodata byte stream: each 3-byte group
//               regroups into four 6-bit indices that map through a rodata
//               alphabet table.  Pins a 3->4 bit regroup + table lookup (distinct
//               from any byte-wise substitution).
//   * hamming - Hamming distance between two rodata bit-vectors: XOR the pair and
//               Kernighan-count the set bits.  Pins an XOR-then-popcount distance
//               (distinct from the single-word popcount above).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress157RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress157RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress157RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress157RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress157RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress157RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress157RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress157RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress157TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // population count + parity of rodata words (Kernighan bit-clear).
    {p+"_popcnt",
     "static const unsigned char "+p+"_pc[24]={0x9e,0x37,0xc1,0x5a,0x2f,0xe8,0x73,0x14,0xab,0x60,0xdd,0x06,0x99,0x42,0xbf,0x28,0x4d,0xf2,0x81,0x3c,0xe0,0x57,0x6b,0xa9};\n"
     +t+" "+p+"_popcnt("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<24;q++){ unsigned w=(((unsigned)"+p+"_pc[q]<<8)|((s>>(q&7))&0xFFu)); unsigned x=w,cnt=0u;\n"
     "      while(x){ x&=x-1u; cnt++; } unsigned par=cnt&1u; acc=acc*131u+cnt+par*7u; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x21u}, "OptStress157", 2},

    // base64-style encode of a rodata byte stream (3->4 regroup + alphabet map).
    {p+"_b64",
     "static const unsigned char "+p+"_bd[18]={0x4a,0x17,0xc3,0x9e,0x52,0x88,0x6f,0x21,0xbd,0x05,0xe7,0x3c,0x70,0xa9,0x16,0xd4,0x8b,0x42};\n"
     "static const unsigned char "+p+"_balpha[64]={65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,48,49,50,51,52,53,54,55,56,57,43,47};\n"
     +t+" "+p+"_b64("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned d[18]; for(int i=0;i<18;i++) d[i]=((unsigned)"+p+"_bd[i]^((s>>(i&7))&3u))&0xFFu;\n"
     "    for(int g=0;g+3<=18;g+=3){ unsigned b0=d[g],b1=d[g+1],b2=d[g+2];\n"
     "      unsigned i0=b0>>2,i1=((b0&3u)<<4)|(b1>>4),i2=((b1&15u)<<2)|(b2>>6),i3=b2&63u;\n"
     "      unsigned c0=(unsigned)"+p+"_balpha[i0],c1=(unsigned)"+p+"_balpha[i1],c2=(unsigned)"+p+"_balpha[i2],c3=(unsigned)"+p+"_balpha[i3];\n"
     "      acc=acc*131u+c0+c1*3u+c2*7u+c3*11u; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x32u}, "OptStress157", 2},

    // Hamming distance between two rodata bit-vectors (XOR + Kernighan count).
    {p+"_hamming",
     "static const unsigned char "+p+"_ha[24]={0x9e,0x37,0xc1,0x5a,0x2f,0xe8,0x73,0x14,0xab,0x60,0xdd,0x06,0x99,0x42,0xbf,0x28,0x4d,0xf2,0x81,0x3c,0xe0,0x57,0x6b,0xa9};\n"
     "static const unsigned char "+p+"_hb[24]={0x4d,0xf2,0x81,0x3c,0xe0,0x57,0x6b,0xa9,0x18,0xd4,0x2e,0x95,0x70,0xc3,0x0f,0x88,0x9e,0x37,0xc1,0x5a,0x2f,0xe8,0x73,0x14};\n"
     +t+" "+p+"_hamming("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s, dist=0u;\n"
     "    for(int i=0;i<24;i++){ unsigned x=((unsigned)"+p+"_ha[i]^((s>>(i&7))&1u))^((unsigned)"+p+"_hb[i]^((s>>((i+4)&7))&1u));\n"
     "      while(x){ x&=x-1u; dist++; } acc=acc*131u+dist; }\n"
     "    acc=acc*131u+dist; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x63u}, "OptStress157", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress157TC("x64o157", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress157TC("x86o157", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress157TC("a64o157", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress157TC("armo157", "int");

INSTANTIATE_TEST_SUITE_P(OptStress157, X64OptStress157RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress157, X86OptStress157RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress157, A64OptStress157RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress157, ARM32OptStress157RT, ::testing::ValuesIn(kARM), rtTCName);
