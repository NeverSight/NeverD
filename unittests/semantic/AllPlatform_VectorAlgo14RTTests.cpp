//===- AllPlatform_VectorAlgo14RTTests.cpp - sign/byte/string ---*- C++ -*-===//
//
// Fourteenth batch of clang -O2 algorithm probes.  Targets count-leading-sign
// (cls / vcls), byte/string scanning (memchr / count / compare), case folding,
// signed saturating narrow (sqxtn / vqmovn), 16-bit byte swap (rev16 / vrev16)
// and xor/checksum reductions — areas where per-lane vs whole-register handling
// and widening/narrowing have historically diverged.
//
// Every algorithm folds to an exact integer for bit-exact comparison.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo14RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo14RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo14RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo14RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo14RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo14RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec14TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Count leading sign bits over an i32 array (cls / vcls).
    {p+"_clsarr",
     t+" "+p+"_clsarr("+t+" a) {\n"
     "  int v[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++) v[i]=(int)(a*(i+1)) ^ (i*0x33AA55);\n"
     "  for (int i=0;i<64;i++) s += __builtin_clrsb(v[i]);\n"
     "  return s;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo14", opt, fl},

    // memchr-like: index of first byte equal to a target (compare scan).
    {p+"_memchr",
     t+" "+p+"_memchr("+t+" a) {\n"
     "  unsigned char b[128]; \n"
     "  for (int i=0;i<128;i++) b[i]=(unsigned char)(a*(i+1)+i*5);\n"
     "  unsigned char tgt=b[97]; int idx=-1;\n"
     "  for (int i=0;i<128;i++) if (b[i]==tgt){ idx=i; break; }\n"
     "  return idx;\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo14", opt, fl},

    // Count bytes equal to a target (cmeq + reduce / pcmpeqb + popcnt).
    {p+"_counteq",
     t+" "+p+"_counteq("+t+" a) {\n"
     "  unsigned char b[128]; int s = 0;\n"
     "  for (int i=0;i<128;i++) b[i]=(unsigned char)((a*(i+1))&7);\n"
     "  for (int i=0;i<128;i++) if (b[i]==3) s++;\n"
     "  return s;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo14", opt, fl},

    // ASCII upper-case fold then checksum (range compare + conditional sub).
    {p+"_toupper",
     t+" "+p+"_toupper("+t+" a) {\n"
     "  unsigned char b[96]; int s = 0;\n"
     "  for (int i=0;i<96;i++){ unsigned char c=(unsigned char)('a'+((a*(i+1))%26)); b[i]=c; }\n"
     "  for (int i=0;i<96;i++){ unsigned char c=b[i]; if (c>='a'&&c<='z') c=(unsigned char)(c-32); s += c; }\n"
     "  return s;\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo14", opt, fl},

    // Reverse a byte array and checksum (rev / shuffle).
    {p+"_strrev",
     t+" "+p+"_strrev("+t+" a) {\n"
     "  unsigned char b[64], r[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++) b[i]=(unsigned char)(a*(i+1)+i);\n"
     "  for (int i=0;i<64;i++) r[i]=b[63-i];\n"
     "  for (int i=0;i<64;i++) s ^= r[i] + i;\n"
     "  return s;\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo14", opt, fl},

    // XOR reduction over a u32 array (eor reduce / pxor + fold).
    {p+"_xorsum",
     t+" "+p+"_xorsum("+t+" a) {\n"
     "  unsigned v[64]; unsigned x = 0;\n"
     "  for (int i=0;i<64;i++) v[i]=(unsigned)(a*(i+1)) ^ (unsigned)(i*0x9E3779B1u);\n"
     "  for (int i=0;i<64;i++) x ^= v[i];\n"
     "  return (int)x;\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo14", opt, fl},

    // memcmp-like: index of first differing byte between two arrays.
    {p+"_memcmp",
     t+" "+p+"_memcmp("+t+" a) {\n"
     "  unsigned char x[128], y[128]; \n"
     "  for (int i=0;i<128;i++){ x[i]=(unsigned char)(a*(i+1)); y[i]=x[i]; }\n"
     "  y[83]=(unsigned char)(x[83]+1); int idx=128;\n"
     "  for (int i=0;i<128;i++) if (x[i]!=y[i]){ idx=i; break; }\n"
     "  return idx;\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo14", opt, fl},

    // Max absolute value of an i16 array (abs + max reduce).
    {p+"_maxabs16",
     t+" "+p+"_maxabs16("+t+" a) {\n"
     "  short v[64]; int mx = 0;\n"
     "  for (int i=0;i<64;i++) v[i]=(short)(a*(i+1) - i*333);\n"
     "  for (int i=0;i<64;i++){ int x=v[i]; if (x<0)x=-x; if (x>mx)mx=x; }\n"
     "  return mx;\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo14", opt, fl},

    // Signed saturating narrow i32 -> i16 (sqxtn / vqmovn / packssdw).
    {p+"_sqnarrow",
     t+" "+p+"_sqnarrow("+t+" a) {\n"
     "  int v[64]; short r[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++) v[i]=(int)(a*(i+1)) - (i*5000);\n"
     "  for (int i=0;i<64;i++){ int x=v[i]; if(x>32767)x=32767; if(x<-32768)x=-32768; r[i]=(short)x; }\n"
     "  for (int i=0;i<64;i++) s += r[i];\n"
     "  return s;\n"
     "}\n",
     {0x99AABBCULL}, "VectorAlgo14", opt, fl},

    // 16-bit byte swap across a u16 array (rev16 / vrev16).
    {p+"_bswap16",
     t+" "+p+"_bswap16("+t+" a) {\n"
     "  unsigned short v[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++) v[i]=(unsigned short)(a*(i+1)+i*101);\n"
     "  for (int i=0;i<64;i++){ unsigned short x=v[i]; x=(unsigned short)((x>>8)|(x<<8)); s += x; }\n"
     "  return s;\n"
     "}\n",
     {0xAABBCCDULL}, "VectorAlgo14", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec14 =
    makeVec14TC("x64v14", "long", 2, "-msse4.2");
static const std::vector<RoundTripTC> kA64Vec14 =
    makeVec14TC("a64v14", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Vec14 =
    makeVec14TC("armv14v", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo14, X64VectorAlgo14RT,
                         ::testing::ValuesIn(kX64Vec14), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo14, A64VectorAlgo14RT,
                         ::testing::ValuesIn(kA64Vec14), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo14, ARM32VectorAlgo14RT,
                         ::testing::ValuesIn(kARM32Vec14), rtTCName);
