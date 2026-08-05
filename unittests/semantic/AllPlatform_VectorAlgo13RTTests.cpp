//===- AllPlatform_VectorAlgo13RTTests.cpp - misc NEON paths ----*- C++ -*-===//
//
// Thirteenth batch of clang -O2 algorithm probes.  Targets remaining NEON
// corners: trailing-zero counts (rbit+clz), bit-select blends (vbsl/vbit),
// shift-right narrow (vshrn / shrn / packsswb), saturating doubling multiply
// high (vqdmulh / sqdmulh), pairwise max chains, widening accumulate, bitfield
// extract (ubfx), saturating accumulate (vqadd), per-element bit reverse
// (rbit / vrbit) and conditional counting.
//
// Every algorithm folds to an exact integer for bit-exact original/recompiled
// comparison.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo13RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo13RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo13RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo13RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo13RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo13RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec13TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Trailing-zero count over a u32 array (rbit+clz / tzcnt).
    {p+"_ctzarr",
     t+" "+p+"_ctzarr("+t+" a) {\n"
     "  unsigned v[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++) v[i]=((unsigned)(a*(i+1)) ^ (unsigned)(i*2246822519u)) | 0x80000000u;\n"
     "  for (int i=0;i<64;i++) s += __builtin_ctz(v[i]);\n"
     "  return s;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo13", opt, fl},

    // Bit-select blend: r = (cond ? x : y) per element via masks (vbsl/vbit).
    {p+"_bsl",
     t+" "+p+"_bsl("+t+" a) {\n"
     "  int x[64], y[64], m[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++){ x[i]=(int)(a*(i+1)); y[i]=(int)(a*(i+3)); m[i]=((int)(a*(i+5))&1)?-1:0; }\n"
     "  for (int i=0;i<64;i++) s ^= ((x[i]&m[i])|(y[i]&~m[i])) + i;\n"
     "  return s;\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo13", opt, fl},

    // Shift-right narrow i32 -> i16 ((v>>3) truncated), summed (vshrn / packssdw).
    {p+"_shrn",
     t+" "+p+"_shrn("+t+" a) {\n"
     "  int v[64]; short r[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++) v[i]=(int)(a*(i+1)) - (i*777);\n"
     "  for (int i=0;i<64;i++) r[i]=(short)(v[i]>>3);\n"
     "  for (int i=0;i<64;i++) s += r[i];\n"
     "  return s;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo13", opt, fl},

    // Saturating doubling multiply high i16: clamp((2*a*b)>>16) summed.
    {p+"_qdmulh",
     t+" "+p+"_qdmulh("+t+" a) {\n"
     "  short v[64], w[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++){ v[i]=(short)(a*(i+1)); w[i]=(short)(a*(i+3)+i); }\n"
     "  for (int i=0;i<64;i++){ int p=((int)v[i]*(int)w[i]*2)>>16; if(p>32767)p=32767; if(p<-32768)p=-32768; s += p; }\n"
     "  return s;\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo13", opt, fl},

    // Pairwise max chains across a window then reduce.
    {p+"_pmaxw",
     t+" "+p+"_pmaxw("+t+" a) {\n"
     "  int v[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++) v[i]=(int)(a*(i+1)) ^ (i*0x2468);\n"
     "  for (int i=0;i+1<64;i+=2){ int m=v[i]>v[i+1]?v[i]:v[i+1]; s += m; }\n"
     "  return s;\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo13", opt, fl},

    // Widening accumulate: sum of i16 array into i32 (vaddw / pmaddwd-ish).
    {p+"_widenacc",
     t+" "+p+"_widenacc("+t+" a) {\n"
     "  short v[96]; int acc = 0;\n"
     "  for (int i=0;i<96;i++) v[i]=(short)(a*(i+1) - i*271);\n"
     "  for (int i=0;i<96;i++) acc += (int)v[i] * 3;\n"
     "  return acc;\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo13", opt, fl},

    // Bitfield extract: sum of ((v>>5)&0x3FF) (ubfx / bextr).
    {p+"_bfx",
     t+" "+p+"_bfx("+t+" a) {\n"
     "  unsigned v[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++) v[i]=(unsigned)(a*(i+1)) ^ (unsigned)(i*0x9E3779B1u);\n"
     "  for (int i=0;i<64;i++) s += (int)((v[i]>>5)&0x3FF);\n"
     "  return s;\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo13", opt, fl},

    // Saturating unsigned accumulate into u8 then sum (vqadd).
    {p+"_qacc",
     t+" "+p+"_qacc("+t+" a) {\n"
     "  unsigned char r[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++){ int acc=0; for(int k=0;k<4;k++){ acc += (int)((unsigned char)(a*(i+1)+k*40)); } r[i]=(unsigned char)(acc>255?255:acc); }\n"
     "  for (int i=0;i<64;i++) s += r[i];\n"
     "  return s;\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo13", opt, fl},

    // Per-element 32-bit bit reverse, summed (rbit / vrbit byte + rev).
    {p+"_rbitarr",
     t+" "+p+"_rbitarr("+t+" a) {\n"
     "  unsigned v[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++) v[i]=(unsigned)(a*(i+1)) ^ (unsigned)(i*0x85EBCA77u);\n"
     "  for (int i=0;i<64;i++){ unsigned x=v[i]; x=((x>>1)&0x55555555u)|((x&0x55555555u)<<1);"
     " x=((x>>2)&0x33333333u)|((x&0x33333333u)<<2); x=((x>>4)&0x0F0F0F0Fu)|((x&0x0F0F0F0Fu)<<4);"
     " x=((x>>8)&0x00FF00FFu)|((x&0x00FF00FFu)<<8); x=(x>>16)|(x<<16); s ^= (int)x + i; }\n"
     "  return s;\n"
     "}\n",
     {0x99AABBCULL}, "VectorAlgo13", opt, fl},

    // Conditional count: number of elements above a threshold (cmp+mask reduce).
    {p+"_cmpcount",
     t+" "+p+"_cmpcount("+t+" a) {\n"
     "  int v[128]; int s = 0;\n"
     "  for (int i=0;i<128;i++) v[i]=(int)(a*(i+1)) ^ (i*0x13572468);\n"
     "  for (int i=0;i<128;i++) if (v[i] > 0) s++;\n"
     "  return s;\n"
     "}\n",
     {0xAABBCCDULL}, "VectorAlgo13", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec13 =
    makeVec13TC("x64v13", "long", 2, "-msse4.2");
static const std::vector<RoundTripTC> kA64Vec13 =
    makeVec13TC("a64v13", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Vec13 =
    makeVec13TC("armv13v", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo13, X64VectorAlgo13RT,
                         ::testing::ValuesIn(kX64Vec13), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo13, A64VectorAlgo13RT,
                         ::testing::ValuesIn(kA64Vec13), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo13, ARM32VectorAlgo13RT,
                         ::testing::ValuesIn(kARM32Vec13), rtTCName);
