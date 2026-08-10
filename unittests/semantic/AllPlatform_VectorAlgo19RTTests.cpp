//===- AllPlatform_VectorAlgo19RTTests.cpp - codec/hash algos ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Nineteenth batch of clang -O2 algorithm probes.  Kernels target instruction
// mixes still under-represented by VectorAlgo1-18 — bit-field pack/unpack,
// widening alpha blend, rodata nibble LUT, constant modulo/divide reciprocals,
// rotate-based hashing, argmin/argmax select chains and signed saturating pack:
//   * rgb565    - RGB888<->RGB565 pack/unpack (shift/mask/or bitfield steering).
//   * alphablnd - per-channel alpha blend (widening mul-add + /255 reciprocal).
//   * popcntlut - nibble population-count via 16-entry rodata table lookup.
//   * adler32   - Adler-32 checksum (two running sums, modulo 65521 reciprocal).
//   * lcg32     - 32-bit LCG PRNG mixing (loop-carried mul-add recurrence).
//   * graycode  - binary<->Gray round trip (xor/shift reduction chains).
//   * rothash   - FNV-style rotate hash ((h<<5|h>>27)^c -> ROL + mul).
//   * minmaxidx - running min/max with argmin/argmax (paired compare-select).
//   * satpack16 - signed saturate int32 -> int16 (PACKSSDW / SQXTN / smin-smax).
//   * quantize  - uniform quantize/dequantize (constant /255 and /32 magic mul).
//
// All arithmetic is bounded 32-bit with constant-divisor shifts/divides and
// local arrays (one deliberate static-const rodata LUT), so nothing lowers to a
// libcall Unicorn lacks.  Returns fold to an exact integer so any native-vs-
// lifted lowering divergence surfaces as a mismatch.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo19RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo19RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo19RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo19RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo19RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo19RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeVA19TC(const char *prefix, const char *T,
                                           int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // RGB888 -> RGB565 pack then unpack: bit-field shift/mask/or steering.
    {p+"_rgb565",
     t+" "+p+"_rgb565("+t+" a){\n"
     "  unsigned char img[126];\n"
     "  for(int i=0;i<126;i++) img[i]=(unsigned char)(a*(i+1)+i*3);\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<42;i++){ unsigned R=img[i*3],G=img[i*3+1],B=img[i*3+2];\n"
     "    unsigned pk=((R&0xF8)<<8)|((G&0xFC)<<3)|(B>>3);\n"
     "    unsigned r2=(pk>>11)&0x1F,g2=(pk>>5)&0x3F,b2=pk&0x1F;\n"
     "    acc=acc*131u+((r2<<16)|(g2<<8)|b2); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo19", opt, fl},

    // Per-channel alpha blend: out=(fg*a+bg*(255-a))/255 (widening MAC + recip).
    {p+"_alphablnd",
     t+" "+p+"_alphablnd("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ unsigned fg=(unsigned)(a*(i+1))&0xFF;\n"
     "    unsigned bg=(unsigned)(a*7+i*5)&0xFF, al=(unsigned)(a*3+i)&0xFF;\n"
     "    unsigned o=fg*al+bg*(255-al)+128; o=(o+(o>>8))>>8;\n"
     "    acc=acc*131u+(o&0xFF); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo19", opt, fl},

    // Nibble popcount via 16-entry static-const rodata table lookup.
    {p+"_popcntlut",
     t+" "+p+"_popcntlut("+t+" a){\n"
     "  static const unsigned char nib[16]=\n"
     "    {0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4};\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ unsigned v=(unsigned)(a*(i+1)+i*7),c=0;\n"
     "    for(int k=0;k<8;k++) c+=nib[(v>>(k*4))&0xF];\n"
     "    acc=acc*131u+c; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo19", opt, fl},

    // Adler-32 checksum: two running sums, modulo-65521 reciprocal divide.
    {p+"_adler32",
     t+" "+p+"_adler32("+t+" a){\n"
     "  unsigned char buf[200];\n"
     "  for(int i=0;i<200;i++) buf[i]=(unsigned char)(a*(i+1)+i*3);\n"
     "  unsigned s1=1,s2=0;\n"
     "  for(int i=0;i<200;i++){ s1=(s1+buf[i])%65521u; s2=(s2+s1)%65521u; }\n"
     "  return ("+t+")((s2<<16)|s1);\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo19", opt, fl},

    // 32-bit linear-congruential PRNG mixing (loop-carried mul-add recurrence).
    {p+"_lcg32",
     t+" "+p+"_lcg32("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u,acc=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1664525u+1013904223u;\n"
     "    acc=acc*131u+(s>>24); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo19", opt, fl},

    // binary<->Gray round trip (xor/shift reduction chains).
    {p+"_graycode",
     t+" "+p+"_graycode("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<200;i++){ unsigned v=(unsigned)(a*(i+1)+i);\n"
     "    unsigned g=v^(v>>1), b=g;\n"
     "    b^=b>>16; b^=b>>8; b^=b>>4; b^=b>>2; b^=b>>1;\n"
     "    acc=acc*131u+(g^b); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo19", opt, fl},

    // FNV-style rotate hash: (h<<5|h>>27)^c -> ROL idiom + multiply.
    {p+"_rothash",
     t+" "+p+"_rothash("+t+" a){\n"
     "  unsigned char buf[128];\n"
     "  for(int i=0;i<128;i++) buf[i]=(unsigned char)(a*(i+1)+i*7);\n"
     "  unsigned h=2166136261u;\n"
     "  for(int i=0;i<128;i++){ h=((h<<5)|(h>>27))^buf[i]; h*=16777619u; }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo19", opt, fl},

    // Running min/max with argmin/argmax (paired compare + conditional select).
    {p+"_minmaxidx",
     t+" "+p+"_minmaxidx("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int k=0;k<32;k++){ int x[48];\n"
     "    for(int i=0;i<48;i++) x[i]=(int)((a*(i+1)+k*13)&0x1FFFF)-65536;\n"
     "    int mn=x[0],mx=x[0],mni=0,mxi=0;\n"
     "    for(int i=1;i<48;i++){ if(x[i]<mn){mn=x[i];mni=i;}\n"
     "      if(x[i]>mx){mx=x[i];mxi=i;} }\n"
     "    acc=acc*131u+(unsigned)(mn+mx+mni*7+mxi*3); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo19", opt, fl},

    // Signed saturate int32 -> int16 (PACKSSDW / SQXTN / smin-smax clamp).
    {p+"_satpack16",
     t+" "+p+"_satpack16("+t+" a){\n"
     "  int x[128];\n"
     "  for(int i=0;i<128;i++) x[i]=(int)((a*(i+1))&0x3FFFF)-131072;\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ int v=x[i];\n"
     "    if(v>32767)v=32767; if(v<-32768)v=-32768;\n"
     "    acc=acc*131u+(unsigned)(v&0xFFFF); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x99AABBCULL}, "VectorAlgo19", opt, fl},

    // Uniform quantize/dequantize: constant /255 and /32 magic reciprocals.
    {p+"_quantize",
     t+" "+p+"_quantize("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ int v=(int)((a*(i+1))&0xFFFF);\n"
     "    int q=(v*32+128)/255; if(q>32)q=32;\n"
     "    int dq=q*255/32;\n"
     "    acc=acc*131u+(unsigned)(dq&0xFFFF); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0xAABBCCDULL}, "VectorAlgo19", opt, fl},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeVA19TC("x64v19", "long", 2, "");
static const std::vector<RoundTripTC> kA64 = makeVA19TC("a64v19", "long", 2, "");
static const std::vector<RoundTripTC> kARM = makeVA19TC("armv19", "int", 2, "");

INSTANTIATE_TEST_SUITE_P(VectorAlgo19, X64VectorAlgo19RT,
                         ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo19, A64VectorAlgo19RT,
                         ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo19, ARM32VectorAlgo19RT,
                         ::testing::ValuesIn(kARM), rtTCName);
