//===- X64_AVX2VarShiftPermRTTests.cpp - 256-bit var-shift / permute ---*-C++*-=//
//
// x86_64-only probes for the AVX2-256 frontier #432 carried forward: per-element
// variable shifts (VPSLLVD/VPSRLVD/VPSRAVD/VPSLLVQ/VPSRLVQ) and the cross-lane
// qword permute (VPERMQ).  Both are forced via GCC vector extensions
// (vector_size(32)) at -O3 -mavx2 and folded to a single integer for bit-exact
// original-vs-lifted comparison.  These exercise the new unicorn-fork gen_sse_256
// paths (inline element-wise shift, 0f3a 00/01 cross-lane qword permute) plus
// NeverD's per-lane variable-shift lift and the VPERMQ cross-lane lift fix
// (previously mis-lifted as an in-lane VPSHUFD).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64AVX2VarShiftPermRT : public SemanticRoundTripFixture,
                              public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64AVX2VarShiftPermRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeVarShiftPermTC() {
  std::string p = "x64vsp";
  return {
    // i32 x8 per-element logical left shift -> VPSLLVD ymm.
    {p+"_sllvd",
     "long "+p+"_sllvd(long a){\n"
     "  typedef unsigned v8u __attribute__((vector_size(32)));\n"
     "  v8u x,sh; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<8;i++){ s=s*1103515245u+12345u; x[i]=s; sh[i]=(s>>7)&31u; }\n"
     "  v8u z=x<<sh;\n"
     "  unsigned r=0; for(int i=0;i<8;i++) r^=z[i];\n"
     "  return (long)r; }\n",
     {0x4dULL}, "AVX2VarShiftPerm", 3, "-mavx2"},

    // i32 x8 per-element logical right shift -> VPSRLVD ymm.
    {p+"_srlvd",
     "long "+p+"_srlvd(long a){\n"
     "  typedef unsigned v8u __attribute__((vector_size(32)));\n"
     "  v8u x,sh; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<8;i++){ s=s*1103515245u+12345u; x[i]=s; sh[i]=(s>>7)&31u; }\n"
     "  v8u z=x>>sh;\n"
     "  unsigned r=0; for(int i=0;i<8;i++) r+=z[i];\n"
     "  return (long)r; }\n",
     {0x6bULL}, "AVX2VarShiftPerm", 3, "-mavx2"},

    // i32 x8 per-element arithmetic right shift -> VPSRAVD ymm.
    {p+"_sravd",
     "long "+p+"_sravd(long a){\n"
     "  typedef int v8i __attribute__((vector_size(32)));\n"
     "  typedef unsigned v8u __attribute__((vector_size(32)));\n"
     "  v8i x; v8u sh; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<8;i++){ s=s*1103515245u+12345u; x[i]=(int)s; sh[i]=(s>>7)&31u; }\n"
     "  v8i z=x>>(v8i)sh;\n"
     "  int r=0; for(int i=0;i<8;i++) r+=z[i];\n"
     "  return (long)(unsigned)r; }\n",
     {0x77ULL}, "AVX2VarShiftPerm", 3, "-mavx2"},

    // i64 x4 per-element logical left shift -> VPSLLVQ ymm.
    {p+"_sllvq",
     "long "+p+"_sllvq(long a){\n"
     "  typedef unsigned long v4q __attribute__((vector_size(32)));\n"
     "  v4q x,sh; unsigned long s=(unsigned long)a|1u;\n"
     "  for(int i=0;i<4;i++){ s=s*6364136223846793005ul+1442695040888963407ul;\n"
     "    x[i]=s; sh[i]=(s>>9)&63u; }\n"
     "  v4q z=x<<sh;\n"
     "  unsigned long r=0; for(int i=0;i<4;i++) r^=z[i];\n"
     "  return (long)r; }\n",
     {0x88ULL}, "AVX2VarShiftPerm", 3, "-mavx2"},

    // i64 x4 per-element logical right shift -> VPSRLVQ ymm.
    {p+"_srlvq",
     "long "+p+"_srlvq(long a){\n"
     "  typedef unsigned long v4q __attribute__((vector_size(32)));\n"
     "  v4q x,sh; unsigned long s=(unsigned long)a|1u;\n"
     "  for(int i=0;i<4;i++){ s=s*6364136223846793005ul+1442695040888963407ul;\n"
     "    x[i]=s; sh[i]=(s>>9)&63u; }\n"
     "  v4q z=x>>sh;\n"
     "  unsigned long r=0; for(int i=0;i<4;i++) r+=z[i];\n"
     "  return (long)r; }\n",
     {0x99ULL}, "AVX2VarShiftPerm", 3, "-mavx2"},

    // ISOLATION: i32 x8 constant right shift -> VPSRLD imm ymm.
    {p+"_srld7",
     "long "+p+"_srld7(long a){\n"
     "  typedef unsigned v8u __attribute__((vector_size(32)));\n"
     "  v8u x; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<8;i++){ s=s*1103515245u+12345u; x[i]=s; }\n"
     "  v8u z=x>>7;\n"
     "  unsigned r=0; for(int i=0;i<8;i++) r+=z[i];\n"
     "  return (long)r; }\n",
     {0x11ULL}, "AVX2VarShiftPerm", 3, "-mavx2"},

    // ISOLATION: i32 x8 constant left shift -> VPSLLD imm ymm.
    {p+"_slld5",
     "long "+p+"_slld5(long a){\n"
     "  typedef unsigned v8u __attribute__((vector_size(32)));\n"
     "  v8u x; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<8;i++){ s=s*1103515245u+12345u; x[i]=s; }\n"
     "  v8u z=x<<5;\n"
     "  unsigned r=0; for(int i=0;i<8;i++) r+=z[i];\n"
     "  return (long)r; }\n",
     {0x22ULL}, "AVX2VarShiftPerm", 3, "-mavx2"},

    // ISOLATION: i32 x8 AND with a runtime scalar -> VPBROADCASTD + VPAND.
    {p+"_bcastd",
     "long "+p+"_bcastd(long a){\n"
     "  typedef unsigned v8u __attribute__((vector_size(32)));\n"
     "  v8u x; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<8;i++){ s=s*1103515245u+12345u; x[i]=s; }\n"
     "  unsigned m=(unsigned)a*2654435761u;\n"
     "  v8u mv={m,m,m,m,m,m,m,m};\n"
     "  v8u z=x&mv;\n"
     "  unsigned r=0; for(int i=0;i<8;i++) r+=z[i];\n"
     "  return (long)r; }\n",
     {0x33ULL}, "AVX2VarShiftPerm", 3, "-mavx2"},

    // i32 x8 per-element variable left shift, counts from a separate masked
    // sequence (exercises VPSLLVD with a distinct count vector + VPBLENDD).
    {p+"_sllvmix",
     "long "+p+"_sllvmix(long a){\n"
     "  typedef unsigned v8u __attribute__((vector_size(32)));\n"
     "  v8u x,sh; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<8;i++){ s=s*1103515245u+12345u; x[i]=s;\n"
     "    s=s*1103515245u+12345u; sh[i]=(s>>11)&15u; }\n"
     "  v8u z=(x<<sh)|(x>>((32u-sh)&31u));\n"
     "  unsigned r=0; for(int i=0;i<8;i++) r^=z[i];\n"
     "  return (long)r; }\n",
     {0x44ULL}, "AVX2VarShiftPerm", 3, "-mavx2"},

    // VEX.128 per-element variable shifts (xmm): force the 4-dword / 2-qword
    // forms the unicorn fork now decodes alongside the 256-bit ones.
    {p+"_sllvd128",
     "long "+p+"_sllvd128(long a){\n"
     "  typedef unsigned v4u __attribute__((vector_size(16)));\n"
     "  v4u x,sh; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<4;i++){ s=s*1103515245u+12345u; x[i]=s; sh[i]=(s>>7)&31u; }\n"
     "  v4u z=x<<sh;\n"
     "  unsigned r=0; for(int i=0;i<4;i++) r^=z[i];\n"
     "  return (long)r; }\n",
     {0xb1ULL}, "AVX2VarShiftPerm", 3, "-mavx2"},

    {p+"_srlvd128",
     "long "+p+"_srlvd128(long a){\n"
     "  typedef unsigned v4u __attribute__((vector_size(16)));\n"
     "  v4u x,sh; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<4;i++){ s=s*1103515245u+12345u; x[i]=s; sh[i]=(s>>7)&31u; }\n"
     "  v4u z=x>>sh;\n"
     "  unsigned r=0; for(int i=0;i<4;i++) r+=z[i];\n"
     "  return (long)r; }\n",
     {0xb2ULL}, "AVX2VarShiftPerm", 3, "-mavx2"},

    {p+"_sravd128",
     "long "+p+"_sravd128(long a){\n"
     "  typedef int v4i __attribute__((vector_size(16)));\n"
     "  typedef unsigned v4u __attribute__((vector_size(16)));\n"
     "  v4i x; v4u sh; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<4;i++){ s=s*1103515245u+12345u; x[i]=(int)s; sh[i]=(s>>7)&31u; }\n"
     "  v4i z=x>>(v4i)sh;\n"
     "  int r=0; for(int i=0;i<4;i++) r+=z[i];\n"
     "  return (long)(unsigned)r; }\n",
     {0xb3ULL}, "AVX2VarShiftPerm", 3, "-mavx2"},

    {p+"_sllvq128",
     "long "+p+"_sllvq128(long a){\n"
     "  typedef unsigned long v2q __attribute__((vector_size(16)));\n"
     "  v2q x,sh; unsigned long s=(unsigned long)a|1u;\n"
     "  for(int i=0;i<2;i++){ s=s*6364136223846793005ul+1442695040888963407ul;\n"
     "    x[i]=s; sh[i]=(s>>9)&63u; }\n"
     "  v2q z=x<<sh;\n"
     "  unsigned long r=0; for(int i=0;i<2;i++) r^=z[i];\n"
     "  return (long)r; }\n",
     {0xb4ULL}, "AVX2VarShiftPerm", 3, "-mavx2"},

    {p+"_srlvq128",
     "long "+p+"_srlvq128(long a){\n"
     "  typedef unsigned long v2q __attribute__((vector_size(16)));\n"
     "  v2q x,sh; unsigned long s=(unsigned long)a|1u;\n"
     "  for(int i=0;i<2;i++){ s=s*6364136223846793005ul+1442695040888963407ul;\n"
     "    x[i]=s; sh[i]=(s>>9)&63u; }\n"
     "  v2q z=x>>sh;\n"
     "  unsigned long r=0; for(int i=0;i<2;i++) r+=z[i];\n"
     "  return (long)r; }\n",
     {0xb5ULL}, "AVX2VarShiftPerm", 3, "-mavx2"},

    // i64 x4 cross-lane reverse -> VPERMQ ymm, imm8=0x1b.
    {p+"_permq",
     "long "+p+"_permq(long a){\n"
     "  typedef unsigned long v4q __attribute__((vector_size(32)));\n"
     "  v4q x; unsigned long s=(unsigned long)a|1u;\n"
     "  for(int i=0;i<4;i++){ s=s*6364136223846793005ul+1442695040888963407ul; x[i]=s; }\n"
     "  v4q z=__builtin_shufflevector(x,x,3,2,1,0);\n"
     "  unsigned long r=0; for(int i=0;i<4;i++) r+=z[i]*(unsigned long)(i+1);\n"
     "  return (long)r; }\n",
     {0xa5ULL}, "AVX2VarShiftPerm", 3, "-mavx2"},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kVSP = makeVarShiftPermTC();
INSTANTIATE_TEST_SUITE_P(AVX2VarShiftPerm, X64AVX2VarShiftPermRT,
                         ::testing::ValuesIn(kVSP), rtTCName);
