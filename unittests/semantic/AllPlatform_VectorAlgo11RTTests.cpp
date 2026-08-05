//===- AllPlatform_VectorAlgo11RTTests.cpp - mixed DSP algos ----*- C++ -*-===//
//
// Eleventh batch of clang -O2 algorithm probes.  Targets paths historically
// thin on coverage: i16 widening multiply-accumulate (pmaddwd / smlal / vmlal),
// data-dependent gather, 4x4 matrix-vector, saturating clamp+narrow, argmax
// with a loop-carried index, array interleave (zip / st2), prefix product,
// vectorized popcount, int<->float round trips and per-element byte swap.
//
// Each algorithm folds to an exact integer return value, so original and
// recompiled code must agree bit-for-bit (both run the same IEEE / integer
// hardware after lift -> recompile).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo11RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo11RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo11RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo11RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo11RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo11RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec11TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // i16 widening dot product: s += (int)v[i]*(int)w[i]  -> pmaddwd/smlal/vmlal.
    {p+"_dotw16",
     t+" "+p+"_dotw16("+t+" a) {\n"
     "  short v[64], w[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++){ v[i]=(short)(a*(i+1)); w[i]=(short)(a*(i+3)+i); }\n"
     "  for (int i=0;i<64;i++) s += (int)v[i]*(int)w[i];\n"
     "  return s;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo11", opt, fl},

    // Data-dependent gather: s += data[idx[i] & 63].
    {p+"_gather",
     t+" "+p+"_gather("+t+" a) {\n"
     "  int data[64], idx[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++){ data[i]=(int)(a*(i+1)); idx[i]=(int)((a*(i+7))&63); }\n"
     "  for (int i=0;i<64;i++) s += data[idx[i] & 63];\n"
     "  return s;\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo11", opt, fl},

    // 4x4 matrix * vector (nested MAC).
    {p+"_matvec",
     t+" "+p+"_matvec("+t+" a) {\n"
     "  int m[16], x[4], y[4]; int s = 0;\n"
     "  for (int i=0;i<16;i++) m[i]=(int)(a*(i+1))&0xFF;\n"
     "  for (int i=0;i<4;i++) x[i]=(int)(a*(i+2))&0xFF;\n"
     "  for (int r=0;r<4;r++){ int acc=0; for (int c=0;c<4;c++) acc+=m[r*4+c]*x[c]; y[r]=acc; }\n"
     "  for (int i=0;i<4;i++) s += y[i];\n"
     "  return s;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo11", opt, fl},

    // Saturating clamp + narrow to u8: r[i] = clamp(v[i]*3+7, 0, 255).
    {p+"_satclamp",
     t+" "+p+"_satclamp("+t+" a) {\n"
     "  int v[64]; unsigned char r[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++) v[i]=(int)(a*(i+1)) - (i*97);\n"
     "  for (int i=0;i<64;i++){ int x=v[i]*3+7; r[i]=(unsigned char)(x<0?0:(x>255?255:x)); }\n"
     "  for (int i=0;i<64;i++) s += r[i];\n"
     "  return s;\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo11", opt, fl},

    // Argmax with loop-carried index (compare + conditional index update).
    {p+"_argmax",
     t+" "+p+"_argmax("+t+" a) {\n"
     "  int v[64]; \n"
     "  for (int i=0;i<64;i++) v[i]=(int)(a*(i+1)) ^ (i*0x1234);\n"
     "  int mx=v[0], mxi=0;\n"
     "  for (int i=1;i<64;i++) if (v[i]>mx){ mx=v[i]; mxi=i; }\n"
     "  return mx ^ (mxi<<8);\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo11", opt, fl},

    // Array interleave: z[2i]=x[i], z[2i+1]=y[i]  -> zip / st2 / punpck.
    {p+"_interleave",
     t+" "+p+"_interleave("+t+" a) {\n"
     "  int x[32], y[32], z[64]; int s = 0;\n"
     "  for (int i=0;i<32;i++){ x[i]=(int)(a*(i+1)); y[i]=(int)(a*(i+5)); }\n"
     "  for (int i=0;i<32;i++){ z[2*i]=x[i]; z[2*i+1]=y[i]; }\n"
     "  for (int i=0;i<64;i++) s ^= z[i] + i;\n"
     "  return s;\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo11", opt, fl},

    // Prefix product (loop-carried scalar, masked to avoid library division).
    {p+"_prefixprod",
     t+" "+p+"_prefixprod("+t+" a) {\n"
     "  unsigned v[32]; int s = 0; unsigned p = 1;\n"
     "  for (int i=0;i<32;i++) v[i]=((unsigned)(a*(i+1))&7)+1;\n"
     "  for (int i=0;i<32;i++){ p=(p*v[i])&0x3FFFFFFF; s ^= (int)p + i; }\n"
     "  return s;\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo11", opt, fl},

    // Vectorized popcount over an array.
    {p+"_popcntarr",
     t+" "+p+"_popcntarr("+t+" a) {\n"
     "  unsigned v[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++) v[i]=(unsigned)(a*(i+1)) ^ (unsigned)(i*0x9E3779B1u);\n"
     "  for (int i=0;i<64;i++) s += __builtin_popcount(v[i]);\n"
     "  return s;\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo11", opt, fl},

    // int -> float scale -> int reduce (cvt round trip).
    {p+"_mixfloat",
     t+" "+p+"_mixfloat("+t+" a) {\n"
     "  int v[48]; float f[48]; int s = 0;\n"
     "  for (int i=0;i<48;i++) v[i]=(int)(a*(i+1)) & 0xFFFF;\n"
     "  for (int i=0;i<48;i++) f[i]=(float)v[i]*0.5f + 1.0f;\n"
     "  for (int i=0;i<48;i++) s += (int)f[i];\n"
     "  return s;\n"
     "}\n",
     {0x99AABBCULL}, "VectorAlgo11", opt, fl},

    // Per-element byte swap (bswap / rev / vrev).
    {p+"_revbytes",
     t+" "+p+"_revbytes("+t+" a) {\n"
     "  unsigned v[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++) v[i]=(unsigned)(a*(i+1)) ^ (unsigned)(i*0x85EBCA77u);\n"
     "  for (int i=0;i<64;i++) s ^= (int)__builtin_bswap32(v[i]) + i;\n"
     "  return s;\n"
     "}\n",
     {0xAABBCCDULL}, "VectorAlgo11", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec11 =
    makeVec11TC("x64v11", "long", 2, "-msse4.2");
static const std::vector<RoundTripTC> kA64Vec11 =
    makeVec11TC("a64v11", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Vec11 =
    makeVec11TC("armv11v", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo11, X64VectorAlgo11RT,
                         ::testing::ValuesIn(kX64Vec11), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo11, A64VectorAlgo11RT,
                         ::testing::ValuesIn(kA64Vec11), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo11, ARM32VectorAlgo11RT,
                         ::testing::ValuesIn(kARM32Vec11), rtTCName);
