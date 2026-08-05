//===- AllPlatform_VectorAlgo46RTTests.cpp - size-opt const-pool SIMD -----===//
//
// Forty-sixth batch of clang vector probes, run at -Os to exercise the size
// optimizer's vectorization + constant-pool layout, which differs markedly from
// the -O2 form (tighter / packed pools, different `.text`-embedded literal
// placement on ARM32).  Focus is the constant-pool-dense idioms #531's
// embedExecSegmentRun rebuilds — bitmask gather (PMOVMSKB family) plus
// constant-coefficient FIR / dot-product / polynomial networks whose
// coefficient vectors materialize as embedded constant pools.
//
// Each kernel is an autovectorizable loop folded to one exact integer for a
// bit-exact original-vs-lifted compare.  Bitmask kernels fold into a u32
// accumulator; widening-MAC kernels mirror VectorAlgo31's dotacc (i64 += of a
// non-overflowing short/byte product, no i64 multiply) to stay libcall-free on
// the 32-bit targets.  x64 uses -mssse3 -Os; a64/arm32 use NEON at -Os.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo46RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo46RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo46RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo46RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo46RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo46RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec46TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Sign-bit bitmask gather at -Os (PMOVMSKB family, byte lanes).
    {p+"_mskpos",
     t+" "+p+"_mskpos("+t+" a) {\n"
     "  signed char v[128]; unsigned acc=0;\n"
     "  for (int i=0;i<128;i++) v[i]=(signed char)((a*(i+1))>>1);\n"
     "  for (int b=0;b<4;b++){ unsigned m=0; for(int i=0;i<32;i++) if(v[b*32+i]<0) m|=(1u<<i); acc=acc*131u+m; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1A2B3C4ULL}, "VectorAlgo46", opt, fl},

    // 16-bit sign-bit gather at -Os (halfword lanes).
    {p+"_msk16",
     t+" "+p+"_msk16("+t+" a) {\n"
     "  short v[128]; unsigned acc=0;\n"
     "  for (int i=0;i<128;i++) v[i]=(short)((a*(i+3))>>2);\n"
     "  for (int b=0;b<8;b++){ unsigned m=0; for(int i=0;i<16;i++) if(v[b*16+i]<0) m|=(1u<<i); acc=acc*131u+m; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2B3C4D5ULL}, "VectorAlgo46", opt, fl},

    // Two-array compare-greater bitmask gather at -Os.
    {p+"_mskcmp",
     t+" "+p+"_mskcmp("+t+" a) {\n"
     "  signed char x[128], y[128]; unsigned acc=0;\n"
     "  for (int i=0;i<128;i++){ x[i]=(signed char)((a*(i+1))>>1); y[i]=(signed char)((a*(i+7))>>2); }\n"
     "  for (int b=0;b<4;b++){ unsigned m=0; for(int i=0;i<32;i++) if(x[b*32+i]>y[b*32+i]) m|=(1u<<i); acc=acc*131u+m; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3C4D5E6ULL}, "VectorAlgo46", opt, fl},

    // Constant-coefficient unsigned Horner polynomial (wraparound-safe, the
    // coefficient set folds into an embedded pool when vectorized).
    {p+"_poly",
     t+" "+p+"_poly("+t+" a) {\n"
     "  unsigned x[128]; unsigned acc=0;\n"
     "  for (int i=0;i<128;i++) x[i]=(unsigned)(a*(i+1))>>10;\n"
     "  for (int i=0;i<128;i++){ unsigned v=x[i];\n"
     "    unsigned r=((((v*7u+13u)*v+17u)*v+23u)*v+29u); acc=acc*131u+r; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x4D5E6F7ULL}, "VectorAlgo46", opt, fl},

    // int16 constant-weight dot product (PMADDWD / SMLAL coefficient pool); i64
    // accumulate by += only (no i64 multiply) — mirrors VectorAlgo31 dotacc.
    {p+"_dot16",
     t+" "+p+"_dot16("+t+" a) {\n"
     "  short x[128]; long long acc=0;\n"
     "  static const short W[8]={101,-103,107,-109,113,-127,131,-137};\n"
     "  for (int i=0;i<128;i++) x[i]=(short)((a*(i+1))>>4);\n"
     "  for (int i=0;i<120;i++){ long long s=0; for(int k=0;k<8;k++) s+=(long long)((int)x[i+k]*(int)W[k]); acc+=s*(i+1); }\n"
     "  return ("+t+")(acc ^ (acc>>32));\n"
     "}\n",
     {0x5E6F708ULL}, "VectorAlgo46", opt, fl},

    // 8-tap constant-coefficient FIR (short input, small coeffs → no overflow).
    {p+"_fir8",
     t+" "+p+"_fir8("+t+" a) {\n"
     "  short x[256]; long long acc=0;\n"
     "  static const int C[8]={3,-7,11,-13,17,-19,23,-29};\n"
     "  for (int i=0;i<256;i++) x[i]=(short)((a*(i+1))>>5);\n"
     "  for (int i=0;i<248;i++){ long long s=0; for(int k=0;k<8;k++) s+=(long long)((int)x[i+k]*C[k]); acc+=s; }\n"
     "  return ("+t+")(acc ^ (acc>>32));\n"
     "}\n",
     {0x6F70819ULL}, "VectorAlgo46", opt, fl},

    // Byte constant-weight weighted sum (widening multiply-accumulate pool).
    {p+"_wsum8",
     t+" "+p+"_wsum8("+t+" a) {\n"
     "  unsigned char x[256]; unsigned acc=0;\n"
     "  static const unsigned char W[8]={3,5,7,11,13,17,19,23};\n"
     "  for (int i=0;i<256;i++) x[i]=(unsigned char)((a*(i+1))>>3);\n"
     "  for (int i=0;i<248;i++){ unsigned s=0; for(int k=0;k<8;k++) s+=(unsigned)x[i+k]*W[k]; acc=acc*131u+s; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x708192AULL}, "VectorAlgo46", opt, fl},

    // Per-block negative count at -Os (compare-to-mask folded to a popcount).
    {p+"_cntneg",
     t+" "+p+"_cntneg("+t+" a) {\n"
     "  signed char v[256]; unsigned acc=0;\n"
     "  for (int i=0;i<256;i++) v[i]=(signed char)((a*(i+5))>>1);\n"
     "  for (int b=0;b<8;b++){ unsigned c=0; for(int i=0;i<32;i++) if(v[b*32+i]<0) c++; acc=acc*131u+c; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x8192A3BULL}, "VectorAlgo46", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec46 =
    makeVec46TC("x64v46", "long", 2, "-mssse3 -Os");
static const std::vector<RoundTripTC> kA64Vec46 =
    makeVec46TC("a64v46", "long", 2, "-Os");
static const std::vector<RoundTripTC> kARM32Vec46 =
    makeVec46TC("armv46", "int", 2, "-Os");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo46, X64VectorAlgo46RT,
                         ::testing::ValuesIn(kX64Vec46), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo46, A64VectorAlgo46RT,
                         ::testing::ValuesIn(kA64Vec46), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo46, ARM32VectorAlgo46RT,
                         ::testing::ValuesIn(kARM32Vec46), rtTCName);
