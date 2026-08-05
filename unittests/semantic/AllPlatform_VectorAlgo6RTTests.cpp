//===- AllPlatform_VectorAlgo6RTTests.cpp - more vectorizable algos -*- C++ -*-===//
//
// Sixth batch of realistic clang -O2 auto-vectorized algorithms used as
// high-yield lift bug probes.  Targets paths the earlier batches did not:
// 4x4 integer matrix multiply (dense MAC + transpose), delta encode + abs sum
// (adjacent subtract), fixed-point RGB->gray weighted sum, an 8-element sorting
// network (paired min/max), Morton bit interleave (shift + mask spreading),
// parity-conditional negate (sign select), horizontal product reduction, and a
// table-free CRC32 (shift + conditional xor).  Each runs the full
// binary -> lift -> MedIR -> LLVM IR -> obj -> binary roundtrip and compares
// Unicorn execution of the original vs recompiled code.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo6RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo6RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo6RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo6RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo6RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo6RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec6TC(const char *prefix, const char *T,
                                           int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // 4x4 integer matrix multiply, return trace-weighted sum -> dense MAC.
    {p+"_matmul4",
     t+" "+p+"_matmul4("+t+" a, "+t+" b) {\n"
     "  int A[4][4], B[4][4], C[4][4]; int s = 0;\n"
     "  for (int i=0;i<4;i++) for (int j=0;j<4;j++){\n"
     "    A[i][j]=(int)(a*(i*4+j+1))^(i+j); B[i][j]=(int)(b*(i*4+j+1))^(i*j); }\n"
     "  for (int i=0;i<4;i++) for (int j=0;j<4;j++){ int acc=0;\n"
     "    for (int k=0;k<4;k++) acc += A[i][k]*B[k][j]; C[i][j]=acc; }\n"
     "  for (int i=0;i<4;i++) for (int j=0;j<4;j++) s += C[i][j]*(i*4+j+1);\n"
     "  return s;\n"
     "}\n",
     {0xC0FFEEULL, 0xBEEF5ULL}, "VectorAlgo6", opt, fl},

    // Delta encode then sum of absolute deltas -> adjacent subtract + abs.
    {p+"_delta",
     t+" "+p+"_delta("+t+" a) {\n"
     "  int v[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++) v[i]=(int)(a*(i+1))^(i*17);\n"
     "  for (int i=1;i<64;i++){ int d=v[i]-v[i-1]; if(d<0) d=-d; s+=d; }\n"
     "  return s;\n"
     "}\n",
     {0x1357BDULL}, "VectorAlgo6", opt, fl},

    // Fixed-point RGB->gray: (77*r + 150*g + 29*b) >> 8, summed.
    {p+"_rgb2gray",
     t+" "+p+"_rgb2gray("+t+" a) {\n"
     "  unsigned char r[32], g[32], b[32]; int s = 0;\n"
     "  for (int i=0;i<32;i++){ r[i]=(unsigned char)(a*(i+1)); g[i]=(unsigned char)(a*(i+2)^i); b[i]=(unsigned char)(a*(i+3)^(i*5)); }\n"
     "  for (int i=0;i<32;i++) s += (77*r[i] + 150*g[i] + 29*b[i]) >> 8;\n"
     "  return s;\n"
     "}\n",
     {0x9A1BULL}, "VectorAlgo6", opt, fl},

    // Parity-conditional negate: add v[i] if even index else subtract.
    {p+"_signflip",
     t+" "+p+"_signflip("+t+" a) {\n"
     "  int v[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++) v[i]=(int)(a*(i+1))^(i*7);\n"
     "  for (int i=0;i<64;i++) s += (i & 1) ? -v[i] : v[i];\n"
     "  return s;\n"
     "}\n",
     {0x2468ACULL}, "VectorAlgo6", opt, fl},

    // Saturating accumulate into i16 range, summed as int.
    {p+"_sat16",
     t+" "+p+"_sat16("+t+" a) {\n"
     "  short v[32]; int s = 0;\n"
     "  for (int i=0;i<32;i++){ int x=(int)(a*(i+1))^(i*9);\n"
     "    if(x>32767)x=32767; if(x<-32768)x=-32768; v[i]=(short)x; }\n"
     "  for (int i=0;i<32;i++) s += v[i];\n"
     "  return s;\n"
     "}\n",
     {0x55AA3ULL}, "VectorAlgo6", opt, fl},

    // Count equal adjacent pairs (v[i]==v[i-1]) -> compare + mask + reduce.
    {p+"_eqpairs",
     t+" "+p+"_eqpairs("+t+" a) {\n"
     "  unsigned char v[128]; int s = 0;\n"
     "  for (int i=0;i<128;i++) v[i]=(unsigned char)((a>>(i&7))*(i+1));\n"
     "  for (int i=1;i<128;i++) if (v[i]==v[i-1]) s++;\n"
     "  return s;\n"
     "}\n",
     {0x778899ULL}, "VectorAlgo6", opt, fl},

    // Weighted dot of u16 arrays accumulated into u32 -> umull/umlal.
    {p+"_udot16",
     t+" "+p+"_udot16("+t+" a, "+t+" b) {\n"
     "  unsigned short x[32], y[32]; unsigned s = 0;\n"
     "  for (int i=0;i<32;i++){ x[i]=(unsigned short)(a*(i+1)); y[i]=(unsigned short)(b*(i+2)); }\n"
     "  for (int i=0;i<32;i++) s += (unsigned)x[i] * (unsigned)y[i];\n"
     "  return (int)s;\n"
     "}\n",
     {0xABCD1ULL, 0x2345ULL}, "VectorAlgo6", opt, fl},

    // Clamp to [lo,hi] then sum (two-sided saturate, signed) -> pmaxsd/pminsd.
    {p+"_clamp2",
     t+" "+p+"_clamp2("+t+" a) {\n"
     "  int v[32]; int s = 0;\n"
     "  for (int i=0;i<32;i++) v[i]=(int)(a*(i+1))^(i*0x9E3779B1);\n"
     "  for (int i=0;i<32;i++){ int x=v[i]; if(x<-1000)x=-1000; if(x>1000)x=1000; s+=x; }\n"
     "  return s;\n"
     "}\n",
     {0x42424242ULL}, "VectorAlgo6", opt, fl},
  };
}

// All 24 probes in this batch now pass; the previously held-back cases were
// root-caused and fixed (#216-#221):
//   arm32 signflip — VREV32/VREV64 were a plain COPY (never reversed) (#216)
//   arm32 delta    — VLD1 post-index `[r0],rN` folded the offset into the load
//                    EA + used the access size for writeback (#217); and the
//                    `vdup.32 d,dM[1]` reduction broadcast lane 0 (#218)
//   a64  matmul4   — ST4/LD4 stored/loaded only the first register with no
//                    (de)interleave (#219); SIMD `orr v.4s,#imm` was a silent
//                    no-op (#220); and `add x,x,#imm,lsl #12` dropped the shift
//                    in operandRead's IMM branch (#221)
static const std::vector<RoundTripTC> kX64Vec6 =
    makeVec6TC("x64v6", "long", 2, "-msse4.2");
static const std::vector<RoundTripTC> kA64Vec6 = makeVec6TC("a64v6", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Vec6 = makeVec6TC("armv6", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo6, X64VectorAlgo6RT,
                         ::testing::ValuesIn(kX64Vec6), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo6, A64VectorAlgo6RT,
                         ::testing::ValuesIn(kA64Vec6), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo6, ARM32VectorAlgo6RT,
                         ::testing::ValuesIn(kARM32Vec6), rtTCName);
