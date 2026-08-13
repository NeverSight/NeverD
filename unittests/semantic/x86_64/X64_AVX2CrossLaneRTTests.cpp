//===- X64_AVX2CrossLaneRTTests.cpp - 256-bit cross-lane permute ------*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// x86_64-only probes for the AVX2-256 frontier #432/#433 carried forward: the
// cross-lane permutes that gen_sse_256 did not yet decode (VPERMD/VPERMPS
// 0f38 36/16, VPERM2I128/VPERM2F128 0f3a 06/46) and byte SAD reduction
// (VPSADBW 256-bit).  The cross-lane forms are forced via the AVX2 intrinsic
// builtins (clang scalarizes a plain GCC-vector shuffle), seeded from the
// runtime argument and folded to a single integer for bit-exact
// original-vs-lifted comparison.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64AVX2CrossLaneRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64AVX2CrossLaneRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeCrossLaneTC() {
  std::string p = "x64xl";
  return {
    // i32 x8 cross-lane variable gather -> VPERMD ymm (0f38 36).  Both the
    // permuted data and the gather index are freshly-computed 256-bit vectors
    // (vpmulld / vpaddd / vpand), so the lifted i256 lane-reconstruction feeds
    // the gather directly -- this is the form that exposed the 256-bit YMM
    // mis-legalization under the baseline x86-64 CPU model (see #436).
    {p+"_permd",
     "long "+p+"_permd(long a){\n"
     "  typedef int v8i __attribute__((vector_size(32)));\n"
     "  v8i b={(int)a,(int)a,(int)a,(int)a,(int)a,(int)a,(int)a,(int)a};\n"
     "  v8i io={0,1,2,3,4,5,6,7};\n"
     "  v8i x=b*7+io; v8i idx=(b+io)&7;\n"
     "  v8i z=__builtin_ia32_permvarsi256(x,idx);\n"
     "  return (long)(unsigned)(z[0]+z[1]*2+z[2]*3+z[3]*4+z[4]*5+z[5]*6+z[6]*7+z[7]*8); }\n",
     {0x4dULL}, "AVX2CrossLane", 2, "-mavx2"},

    // f32 x8 cross-lane variable gather -> VPERMPS ymm (0f38 16), computed data
    // (vcvtdq2ps of a runtime sequence) and computed gather index.
    {p+"_permps",
     "long "+p+"_permps(long a){\n"
     "  typedef int v8i __attribute__((vector_size(32)));\n"
     "  typedef float v8f __attribute__((vector_size(32)));\n"
     "  v8f x; v8i idx; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<8;i++){ s=s*1103515245u+12345u; x[i]=(float)(int)(s>>8); idx[i]=(int)((s>>3)&7u); }\n"
     "  v8f z=__builtin_ia32_permvarsf256(x,idx);\n"
     "  v8i zi=*(v8i*)&z;\n"
     "  unsigned r=0; for(int i=0;i<8;i++) r=r*1000003u+(unsigned)zi[i];\n"
     "  return (long)r; }\n",
     {0x81ULL}, "AVX2CrossLane", 2, "-mavx2"},

    // 128-bit lane select across two vectors -> VPERM2I128/VPERM2F128 (0f3a 06/46),
    // imm8=0x21 picks {x.hi, y.lo}.
    {p+"_perm2",
     "long "+p+"_perm2(long a){\n"
     "  typedef long long v4q __attribute__((vector_size(32)));\n"
     "  v4q x,y; unsigned long s=(unsigned long)a|1u;\n"
     "  for(int i=0;i<4;i++){ s=s*6364136223846793005ul+1442695040888963407ul; x[i]=(long long)s;\n"
     "    s=s*6364136223846793005ul+1442695040888963407ul; y[i]=(long long)s; }\n"
     "  v4q z=__builtin_ia32_permti256(x,y,0x21);\n"
     "  unsigned long r=0; for(int i=0;i<4;i++) r^=(unsigned long)z[i]*(unsigned long)(i+1);\n"
     "  return (long)r; }\n",
     {0x6fULL}, "AVX2CrossLane", 2, "-mavx2"},

    // 128-bit lane swap of a single vector -> VPERM2I128 imm8=0x01.
    {p+"_perm2swap",
     "long "+p+"_perm2swap(long a){\n"
     "  typedef long long v4q __attribute__((vector_size(32)));\n"
     "  v4q x; unsigned long s=(unsigned long)a|1u;\n"
     "  for(int i=0;i<4;i++){ s=s*6364136223846793005ul+1442695040888963407ul; x[i]=(long long)s; }\n"
     "  v4q z=__builtin_ia32_permti256(x,x,0x01);\n"
     "  unsigned long r=0; for(int i=0;i<4;i++) r+=(unsigned long)z[i]*(unsigned long)(i+1);\n"
     "  return (long)r; }\n",
     {0x70ULL}, "AVX2CrossLane", 2, "-mavx2"},

    // u8 x32 sum-of-absolute-differences -> VPSADBW ymm (lane-wise byte SAD).
    {p+"_sadbw",
     "long "+p+"_sadbw(long a){\n"
     "  typedef unsigned char v32b __attribute__((vector_size(32)));\n"
     "  v32b x,y; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<32;i++){ s=s*1103515245u+12345u; x[i]=(unsigned char)(s>>16);\n"
     "    s=s*1103515245u+12345u; y[i]=(unsigned char)(s>>16); }\n"
     "  unsigned r=0;\n"
     "  for(int i=0;i<32;i++){ int d=(int)x[i]-(int)y[i]; r+=(unsigned)(d<0?-d:d); }\n"
     "  return (long)r; }\n",
     {0x92ULL}, "AVX2CrossLane", 3, "-mavx2"},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kXL = makeCrossLaneTC();
INSTANTIATE_TEST_SUITE_P(AVX2CrossLane, X64AVX2CrossLaneRT,
                         ::testing::ValuesIn(kXL), rtTCName);
