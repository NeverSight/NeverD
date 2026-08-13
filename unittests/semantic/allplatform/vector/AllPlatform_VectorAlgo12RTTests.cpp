//===- AllPlatform_VectorAlgo12RTTests.cpp - reduction/narrow ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Twelfth batch of clang -O2 algorithm probes.  Focuses on horizontal
// reductions and widening/narrowing NEON paths that earlier batches under-
// exercised: i8/i16 horizontal sums (vpaddl chains / uaddlv / psadbw),
// saturating narrow (vqmovun / uqxtn / packuswb), min/max reduction, rounding
// average (vrhadd), per-element abs (vabs), leading-zero counts (vclz/clz),
// signed multiply-high, 16-entry byte table lookup (vtbl) and shift-insert.
//
// Every algorithm folds to an exact integer so original and recompiled code
// must agree bit-for-bit.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo12RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo12RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo12RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo12RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo12RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo12RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec12TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Horizontal sum of an i8 array (vpaddl chain / uaddlv / psadbw).
    {p+"_hsum8",
     t+" "+p+"_hsum8("+t+" a) {\n"
     "  signed char v[128]; int s = 0;\n"
     "  for (int i=0;i<128;i++) v[i]=(signed char)(a*(i+1) + i*7);\n"
     "  for (int i=0;i<128;i++) s += v[i];\n"
     "  return s;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo12", opt, fl},

    // Horizontal sum of an i16 array.
    {p+"_hsum16",
     t+" "+p+"_hsum16("+t+" a) {\n"
     "  short v[96]; int s = 0;\n"
     "  for (int i=0;i<96;i++) v[i]=(short)(a*(i+1) - i*131);\n"
     "  for (int i=0;i<96;i++) s += v[i];\n"
     "  return s;\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo12", opt, fl},

    // Saturating narrow i32 -> u8 (vqmovun / uqxtn / packuswb).
    {p+"_qnarrow",
     t+" "+p+"_qnarrow("+t+" a) {\n"
     "  int v[64]; unsigned char r[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++) v[i]=(int)(a*(i+1)) - (i*1000);\n"
     "  for (int i=0;i<64;i++){ int x=v[i]; r[i]=(unsigned char)(x<0?0:(x>255?255:x)); }\n"
     "  for (int i=0;i<64;i++) s += r[i]*(i&3);\n"
     "  return s;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo12", opt, fl},

    // Max reduction over an i32 array.
    {p+"_maxred",
     t+" "+p+"_maxred("+t+" a) {\n"
     "  int v[64]; \n"
     "  for (int i=0;i<64;i++) v[i]=(int)(a*(i+1)) ^ (i*0x55AA);\n"
     "  int mx=v[0];\n"
     "  for (int i=1;i<64;i++) if (v[i]>mx) mx=v[i];\n"
     "  return mx;\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo12", opt, fl},

    // Rounding average of two u8 arrays: (a+b+1)>>1 (vrhadd).
    {p+"_avgr",
     t+" "+p+"_avgr("+t+" a) {\n"
     "  unsigned char x[64], y[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(unsigned char)(a*(i+1)); y[i]=(unsigned char)(a*(i+3)+i); }\n"
     "  for (int i=0;i<64;i++) s += (x[i]+y[i]+1)>>1;\n"
     "  return s;\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo12", opt, fl},

    // Per-element abs of an i32 array (vabs).
    {p+"_absarr",
     t+" "+p+"_absarr("+t+" a) {\n"
     "  int v[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++) v[i]=(int)(a*(i+1)) - (i*0x40000);\n"
     "  for (int i=0;i<64;i++){ int x=v[i]; s += x<0?-x:x; }\n"
     "  return s;\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo12", opt, fl},

    // Leading-zero count over a u32 array (vclz / clz / lzcnt).
    {p+"_clzarr",
     t+" "+p+"_clzarr("+t+" a) {\n"
     "  unsigned v[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++) v[i]=((unsigned)(a*(i+1)) ^ (unsigned)(i*2654435761u)) | 1u;\n"
     "  for (int i=0;i<64;i++) s += __builtin_clz(v[i]);\n"
     "  return s;\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo12", opt, fl},

    // Signed multiply-high i16: (a*b)>>16 per element, summed.
    {p+"_mulhi16",
     t+" "+p+"_mulhi16("+t+" a) {\n"
     "  short v[64], w[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++){ v[i]=(short)(a*(i+1)); w[i]=(short)(a*(i+5)+i); }\n"
     "  for (int i=0;i<64;i++) s += ((int)v[i]*(int)w[i])>>16;\n"
     "  return s;\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo12", opt, fl},

    // 16-entry byte table lookup (vtbl / tbl / pshufb).
    {p+"_tbl16",
     t+" "+p+"_tbl16("+t+" a) {\n"
     "  unsigned char tbl[16], idx[64]; int s = 0;\n"
     "  for (int i=0;i<16;i++) tbl[i]=(unsigned char)(a*(i+1)+i*13);\n"
     "  for (int i=0;i<64;i++) idx[i]=(unsigned char)((a*(i+7))&15);\n"
     "  for (int i=0;i<64;i++) s += tbl[idx[i]&15];\n"
     "  return s;\n"
     "}\n",
     {0x99AABBCULL}, "VectorAlgo12", opt, fl},

    // Shift-left then OR-insert low nibble (shift/insert patterns).
    {p+"_sli",
     t+" "+p+"_sli("+t+" a) {\n"
     "  unsigned v[64], w[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++){ v[i]=(unsigned)(a*(i+1)); w[i]=(unsigned)(a*(i+3)+i); }\n"
     "  for (int i=0;i<64;i++) s ^= (int)((v[i]<<4) | (w[i]&0xF)) + i;\n"
     "  return s;\n"
     "}\n",
     {0xAABBCCDULL}, "VectorAlgo12", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec12 =
    makeVec12TC("x64v12", "long", 2, "-msse4.2");
static const std::vector<RoundTripTC> kA64Vec12 =
    makeVec12TC("a64v12", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Vec12 =
    makeVec12TC("armv12v", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo12, X64VectorAlgo12RT,
                         ::testing::ValuesIn(kX64Vec12), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo12, A64VectorAlgo12RT,
                         ::testing::ValuesIn(kA64Vec12), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo12, ARM32VectorAlgo12RT,
                         ::testing::ValuesIn(kARM32Vec12), rtTCName);
