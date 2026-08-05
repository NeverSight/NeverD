//===- AllPlatform_OptStress147RTTests.cpp - RLE / bit-CRC / radix sort =//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer), so none touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487) and every
// probe runs on all four targets.
//
//   * rle    - run-length encoding of a rodata-seeded stream: a forward walk
//              counts each maximal equal run, tracking run count and longest
//              run.  Pins a run-detection scan (distinct from the move-to-front
//              search-and-shift in #143 and any histogram pass).
//   * crcbit - bit-by-bit CRC-8 over a rodata message (poly 0x07): each byte is
//              folded in, then eight shift-and-conditional-XOR steps reduce the
//              register.  Pins a polynomial-division shift register (distinct
//              from the hardware CRC32 probes and the Gray-code XOR in #141).
//   * radix  - LSD radix sort of a rodata-seeded array by 4-bit digit: three
//              stable counting passes build per-nibble offsets and distribute.
//              Pins a multi-pass stable bucket distribution (distinct from the
//              single-pass histogram/counting sort and the in-place heap/insert
//              sorts in #144).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress147RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress147RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress147RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress147RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress147RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress147RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress147RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress147RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress147TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // run-length encoding of a rodata-seeded stream (run detection scan).
    {p+"_rle",
     "static const unsigned char "+p+"_stream[40]={\n"
     "4,4,4,7,7,2,2,2, 2,9,9,9,1,1,3,3, 3,3,3,6,6,8,8,8, 5,5,5,5,0,0,7,7, 2,2,2,1,1,1,1,1};\n"
     +t+" "+p+"_rle("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s, runs=0u, maxrun=0u;\n"
     "    unsigned d[40];\n"
     "    for(int i=0;i<40;i++) d[i]=(unsigned)"+p+"_stream[i]+((s>>(i&7))&1u);\n"
     "    int i=0;\n"
     "    while(i<40){ unsigned v=d[i], cnt=1u; i++;\n"
     "      while(i<40 && d[i]==v){ cnt++; i++; }\n"
     "      runs++; if(cnt>maxrun) maxrun=cnt; acc=acc*131u+v*101u+cnt; }\n"
     "    acc=acc*131u+runs+maxrun*7u; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x17u}, "OptStress147", 2},

    // bit-by-bit CRC-8 over a rodata message (poly 0x07 shift register).
    {p+"_crcbit",
     "static const unsigned char "+p+"_msg[24]={0x31,0x9a,0x07,0xc5,0x6e,0xb2,0x4f,0x18,0xa3,0x5d,0xd0,0x29,0x7b,0xc6,0x84,0xfd,0x12,0xee,0x53,0x80,0x6a,0x2c,0xf1,0x47};\n"
     +t+" "+p+"_crcbit("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s, crc=s&0xFFu;\n"
     "    for(int i=0;i<24;i++){ crc^=((unsigned)"+p+"_msg[i]^((s>>(i&7))&1u));\n"
     "      for(int b=0;b<8;b++){ if(crc&0x80u) crc=((crc<<1)^0x07u)&0xFFu; else crc=(crc<<1)&0xFFu; }\n"
     "      acc=acc*131u+crc; }\n"
     "    acc=acc*131u+crc; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x28u}, "OptStress147", 2},

    // LSD radix sort of a rodata-seeded array by 4-bit digit (stable buckets).
    {p+"_radix",
     "static const unsigned char "+p+"_d[24]={211,17,140,88,3,199,72,150,46,8,131,2,97,55,188,30,118,77,14,200,41,160,123,5};\n"
     +t+" "+p+"_radix("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned arr[24],tmp[24];\n"
     "    for(int i=0;i<24;i++) arr[i]=((unsigned)"+p+"_d[i]<<2)|((s>>(i&7))&3u);\n"
     "    for(int pass=0;pass<3;pass++){ int sh=pass*4; unsigned cnt[16];\n"
     "      for(int k=0;k<16;k++) cnt[k]=0u;\n"
     "      for(int i=0;i<24;i++) cnt[(arr[i]>>sh)&15u]++;\n"
     "      unsigned sum=0u; for(int k=0;k<16;k++){ unsigned c=cnt[k]; cnt[k]=sum; sum+=c; }\n"
     "      for(int i=0;i<24;i++){ unsigned dg=(arr[i]>>sh)&15u; tmp[cnt[dg]++]=arr[i]; }\n"
     "      for(int i=0;i<24;i++){ arr[i]=tmp[i]; acc=acc*131u+arr[i]; } }\n"
     "    for(int i=0;i<24;i++) acc=acc*131u+arr[i]*(unsigned)(i+1);\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x39u}, "OptStress147", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress147TC("x64o147", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress147TC("x86o147", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress147TC("a64o147", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress147TC("armo147", "int");

INSTANTIATE_TEST_SUITE_P(OptStress147, X64OptStress147RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress147, X86OptStress147RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress147, A64OptStress147RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress147, ARM32OptStress147RT, ::testing::ValuesIn(kARM), rtTCName);
