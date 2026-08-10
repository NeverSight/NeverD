//===- AllPlatform_OptStress61RTTests.cpp - bit packing / swap -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Bit-level packing, endian swaps, bitfield read-modify-write and
// extract/deposit idioms.  clang -O2 turns these into dense shift/mask/and/or
// chains plus REV/REV16/RBIT (AArch64), BSWAP/MOVBE (x86), REV/RBIT (ARM32)
// and bitfield insert/extract (BFI/BFXIL/UBFX, SBFX, x86 shld).  Exercises the
// narrow-width shift, mask-truncation and bitfield codegen paths.
//
//   * bitpack  - pack/unpack 4 bytes <-> word, rotate the lanes, refold.
//   * bswapmix - byte-swap 16/32/64 + endian round-trip mixing.
//   * bitrev   - reverse bits in a word, gray-code encode/decode.
//   * bfstruct - C struct bitfields: read/modify/write packed fields.
//   * depext   - bit deposit/extract (scatter/gather by a mask) in plain C.
//   * fieldmix - signed + unsigned bitfield extraction at odd offsets.
//
// All integer, fold to one return, no float / 64-bit divide helper.
// All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress61RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress61RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress61RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress61RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress61RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress61RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress61RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress61RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress61TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Pack/unpack 4 bytes <-> word, rotate the lanes, refold.
    {p+"_bitpack",
     t+" "+p+"_bitpack("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<300;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned b0=s&0xff,b1=(s>>8)&0xff,b2=(s>>16)&0xff,b3=(s>>24)&0xff;\n"
     "    unsigned w=(b3<<24)|(b2<<16)|(b1<<8)|b0;\n"
     "    w=(w<<8)|(w>>24);\n"
     "    unsigned c0=w&0xff,c1=(w>>8)&0xff,c2=(w>>16)&0xff,c3=(w>>24)&0xff;\n"
     "    h=h*131u+(c0+2u*c1+3u*c2+4u*c3); h^=h>>13; }\n"
     "  return ("+t+")h; }\n",
     {0x91u}, "OptStress61", 2},

    // Byte-swap 16/32/64 + endian round-trip mixing.
    {p+"_bswapmix",
     t+" "+p+"_bswapmix("+t+" a){\n"
     "  unsigned long long h=0, s=(unsigned long long)a;\n"
     "  for(int i=0;i<200;i++){ s=s*6364136223846793005ull+1ull;\n"
     "    unsigned long long x=s;\n"
     "    unsigned long long bs=((x&0x00000000000000ffull)<<56)|((x&0x000000000000ff00ull)<<40)"
     "|((x&0x0000000000ff0000ull)<<24)|((x&0x00000000ff000000ull)<<8)"
     "|((x&0x000000ff00000000ull)>>8)|((x&0x0000ff0000000000ull)>>24)"
     "|((x&0x00ff000000000000ull)>>40)|((x&0xff00000000000000ull)>>56);\n"
     "    unsigned lo=(unsigned)x; unsigned bl=((lo&0xff)<<24)|((lo&0xff00)<<8)"
     "|((lo>>8)&0xff00)|((lo>>24)&0xff);\n"
     "    unsigned short hw=(unsigned short)(x>>11); unsigned short bw=(unsigned short)((hw<<8)|(hw>>8));\n"
     "    h+=bs^((unsigned long long)bl<<3)^bw; h^=h>>17; }\n"
     "  h^=h>>32; return ("+t+")h; }\n",
     {0x92u}, "OptStress61", 2},

    // Reverse bits in a word, gray-code encode/decode.
    {p+"_bitrev",
     t+" "+p+"_bitrev("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<300;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned x=s;\n"
     "    x=((x>>1)&0x55555555u)|((x&0x55555555u)<<1);\n"
     "    x=((x>>2)&0x33333333u)|((x&0x33333333u)<<2);\n"
     "    x=((x>>4)&0x0f0f0f0fu)|((x&0x0f0f0f0fu)<<4);\n"
     "    x=((x>>8)&0x00ff00ffu)|((x&0x00ff00ffu)<<8);\n"
     "    x=(x>>16)|(x<<16);\n"
     "    unsigned g=s^(s>>1); unsigned b=g;\n"
     "    b^=b>>1; b^=b>>2; b^=b>>4; b^=b>>8; b^=b>>16;\n"
     "    h=h*131u+x+g*3u+b*5u; h^=h>>15; }\n"
     "  return ("+t+")h; }\n",
     {0x93u}, "OptStress61", 2},

    // C struct bitfields: read/modify/write packed fields.
    {p+"_bfstruct",
     "struct BF{ unsigned lo:5; int mid:7; unsigned hi:12; unsigned top:8; };\n"
     +t+" "+p+"_bfstruct("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<300;i++){ s=s*1103515245u+12345u;\n"
     "    struct BF f;\n"
     "    f.lo=s&0x1f; f.mid=(int)((s>>5)&0x7f)-64; f.hi=(s>>12)&0xfff; f.top=(s>>24)&0xff;\n"
     "    f.lo=(f.lo+1u)&0x1f; f.mid=f.mid<0? -f.mid : f.mid; f.hi^=0xa5a; f.top=(f.top<<1)|(f.top>>7);\n"
     "    h=h*131u+f.lo+(unsigned)(f.mid+128)+f.hi+f.top; h^=h>>11; }\n"
     "  return ("+t+")h; }\n",
     {0x94u}, "OptStress61", 2},

    // Bit deposit/extract (scatter/gather by a mask) in plain C.
    {p+"_depext",
     t+" "+p+"_depext("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned src=s, mask=(s>>7)|0x10101011u, dep=0; unsigned bb=src;\n"
     "    for(unsigned m=mask; m; m&=m-1u){ unsigned low=m&(0u-m);\n"
     "      if(bb&1u) dep|=low; bb>>=1; }\n"
     "    unsigned ext=0, k=0;\n"
     "    for(unsigned m=mask; m; m&=m-1u){ unsigned low=m&(0u-m);\n"
     "      if(src&low) ext|=(1u<<k); k++; }\n"
     "    h=h*131u+dep+ext*3u; h^=h>>14; }\n"
     "  return ("+t+")h; }\n",
     {0x95u}, "OptStress61", 2},

    // Signed + unsigned bitfield extraction at odd offsets.
    {p+"_fieldmix",
     t+" "+p+"_fieldmix("+t+" a){\n"
     "  unsigned long long h=0, s=(unsigned long long)a;\n"
     "  for(int i=0;i<200;i++){ s=s*6364136223846793005ull+1ull;\n"
     "    int sf=(int)((s<<(32-13))>>(64-13));\n"          // signed 13-bit at offset 0
     "    unsigned uf=(unsigned)((s>>17)&0x7ff);\n"        // unsigned 11-bit at offset 17
     "    int sf2=(int)((s<<(64-9-40))>>(64-9));\n"        // signed 9-bit at offset 40
     "    unsigned uf2=(unsigned)((s>>49)&0x7fff);\n"      // unsigned 15-bit at offset 49
     "    h+=(unsigned long long)(sf+4096)+uf*3ull+(unsigned long long)(sf2+256)*5ull+uf2*7ull;\n"
     "    h^=h>>19; }\n"
     "  h^=h>>32; return ("+t+")h; }\n",
     {0x96u}, "OptStress61", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress61TC("x64o61", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress61TC("x86o61", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress61TC("a64o61", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress61TC("armo61", "int");

INSTANTIATE_TEST_SUITE_P(OptStress61, X64OptStress61RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress61, X86OptStress61RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress61, A64OptStress61RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress61, ARM32OptStress61RT, ::testing::ValuesIn(kARM), rtTCName);
