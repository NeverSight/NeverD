//===- AllPlatform_OptStress119RTTests.cpp - varint / morton / greedy shapes =//
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
//   * varint - delta + zigzag + LEB128 varint encode of a rodata sequence:
//              `zz=(d<<1)^(d>>31)` then a 7-bit-group emit loop.  Pins a signed
//              delta / zigzag transform feeding a variable-length 7-bit grouping.
//   * morton - Z-order (Morton) code: bit-interleave of two rodata coordinate
//              streams `m|=((x>>b)&1)<<(2b) | ((y>>b)&1)<<(2b+1)`, then a rodata
//              gather.  Pins a bit-spread interleave over two rodata arrays.
//   * actsel - greedy activity selection over adjacent rodata (start,end) pairs:
//              a single-pass scan keeping a `lastEnd` watermark and counting the
//              non-overlapping picks.  Pins an adjacent-pair linear read with a
//              carried greedy state (distinct from the threshold/segment scans).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress119RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress119RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress119RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress119RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress119RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress119RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress119RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress119RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress119TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // delta + zigzag + LEB128 varint encode of a rodata sequence.
    {p+"_varint",
     "static const unsigned char "+p+"_seq[32]={\n"
     "0x10,0x14,0x09,0x2a,0x2f,0x05,0x60,0x58, 0x33,0x40,0x1b,0x77,0x6e,0x02,0x99,0x84,\n"
     "0xa3,0x12,0xc8,0x4d,0x36,0xe1,0x07,0xbb, 0x5a,0x90,0x28,0xf3,0x6c,0x11,0xd7,0x49};\n"
     +t+" "+p+"_varint("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s; int prev=0;\n"
     "    for(int i=0;i<32;i++){ int cur=(int)("+p+"_seq[i]^(s&0xFFu));\n"
     "      int d=cur-prev; prev=cur;\n"
     "      unsigned zz=((unsigned)(d<<1))^((unsigned)(d>>31));\n"
     "      unsigned grp=0u, val=zz;\n"
     "      do { unsigned b=val&0x7Fu; val>>=7; if(val) b|=0x80u; grp=grp*131u+b; } while(val);\n"
     "      acc=acc*131u+grp; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x7Fu}, "OptStress119", 2},

    // Z-order (Morton) bit-interleave of two rodata coordinate streams + gather.
    {p+"_morton",
     "static const unsigned char "+p+"_xs[24]={\n"
     "0x12,0x9a,0x47,0xe3,0x05,0xbd,0x72,0x18, 0x8f,0x23,0xd6,0x4a,0x91,0x0c,0xfe,0x57,\n"
     "0x6b,0xa3,0x2e,0xd0,0x14,0x88,0x3d,0xc9};\n"
     "static const unsigned char "+p+"_ys[24]={\n"
     "0x60,0xf5,0x1b,0xa7,0x42,0xce,0x09,0x96, 0x7d,0xe1,0x35,0xb8,0x4f,0xd2,0x26,0xab,\n"
     "0x53,0x2a,0x9f,0x14,0xc7,0x6b,0xe0,0x38};\n"
     "static const unsigned char "+p+"_lut[16]={5,12,3,9,14,1,7,11,2,8,15,0,6,13,4,10};\n"
     +t+" "+p+"_morton("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int i=0;i<24;i++){ unsigned x="+p+"_xs[i]^(s&0xFFu), y="+p+"_ys[i]^((s>>8)&0xFFu);\n"
     "      unsigned m=0u; for(int b=0;b<8;b++){ m|=((x>>b)&1u)<<(2*b); m|=((y>>b)&1u)<<(2*b+1); }\n"
     "      unsigned g="+p+"_lut[m&15u]^((m>>4)&0xFu);\n"
     "      acc=acc*131u+m+g; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x4Du}, "OptStress119", 2},

    // greedy activity selection over adjacent rodata (start,end) pairs.
    {p+"_actsel",
     "static const unsigned char "+p+"_iv[40]={\n"
     "1,4, 3,5, 0,6, 5,7, 3,8, 5,9, 6,10, 8,11, 8,12, 2,13,\n"
     "12,14, 9,15, 11,16, 14,17, 13,18, 15,19, 16,20, 18,22, 20,24, 21,26};\n"
     +t+" "+p+"_actsel("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    int lastEnd=-1; unsigned cnt=0u;\n"
     "    for(int i=0;i<20;i++){ int st=(int)"+p+"_iv[i*2]+(int)((s>>(i&7))&1u);\n"
     "      int en=(int)"+p+"_iv[i*2+1];\n"
     "      if(st>=lastEnd){ cnt++; lastEnd=en; acc=acc*131u+(unsigned)en; } }\n"
     "    acc=acc*131u+cnt;\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x7Au}, "OptStress119", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress119TC("x64o119", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress119TC("x86o119", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress119TC("a64o119", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress119TC("armo119", "int");

INSTANTIATE_TEST_SUITE_P(OptStress119, X64OptStress119RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress119, X86OptStress119RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress119, A64OptStress119RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress119, ARM32OptStress119RT, ::testing::ValuesIn(kARM), rtTCName);
