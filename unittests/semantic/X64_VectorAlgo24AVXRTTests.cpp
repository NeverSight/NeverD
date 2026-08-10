//===- X64_VectorAlgo24AVXRTTests.cpp - AVX256 clang -O2 probes -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// x86_64-only kernels compiled with -mavx2 -O2 to reach 256-bit YMM paths:
// packed float/double add/mul, min/max, compare-blend, and byte shuffle hashes.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo24AVXRT : public SemanticRoundTripFixture,
                             public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo24AVXRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
#define V8F  "typedef float v8f __attribute__((vector_size(32)));\n"
#define V4D  "typedef double v4d __attribute__((vector_size(32)));\n"
#define V32UC "typedef unsigned char v32uc __attribute__((vector_size(32)));\n"

static const std::vector<RoundTripTC> kX64AVX24 = {
  {"ymm_fadd",
   V8F
   "long ymm_fadd(long a) {\n"
   "  v8f va = {(float)a,1,2,3,4,5,6,7};\n"
   "  v8f vb = {0.5f,1.5f,2.5f,3.5f,4.5f,5.5f,6.5f,7.5f};\n"
   "  v8f vr = va + vb;\n"
   "  float s = vr[0]+vr[1]+vr[2]+vr[3]+vr[4]+vr[5]+vr[6]+vr[7];\n"
   "  unsigned o; __builtin_memcpy(&o,&s,4); return (long)o;\n"
   "}\n",
   {0x12345ULL}, "VectorAlgo24AVX", 2, "-mavx2 -fno-math-errno"},

  {"ymm_fmul",
   V8F
   "long ymm_fmul(long a) {\n"
   "  v8f va = {(float)a,2,3,4,5,6,7,8};\n"
   "  v8f vb = {1.25f,1.5f,1.75f,2.0f,2.25f,2.5f,2.75f,3.0f};\n"
   "  v8f vr = va * vb;\n"
   "  float s = 0; for(int i=0;i<8;i++) s+=vr[i];\n"
   "  unsigned o; __builtin_memcpy(&o,&s,4); return (long)o;\n"
   "}\n",
   {42}, "VectorAlgo24AVX", 2, "-mavx2 -fno-math-errno"},

  {"ymm_dadd",
   V4D
   "long ymm_dadd(long a) {\n"
   "  v4d va = {(double)a, 1.0, 2.0, 3.0};\n"
   "  v4d vb = {0.25, 0.5, 0.75, 1.0};\n"
   "  v4d vr = va + vb;\n"
   "  double s = vr[0]+vr[1]+vr[2]+vr[3];\n"
   "  unsigned lo,hi; __builtin_memcpy(&lo,&s,4); __builtin_memcpy(&hi,((char*)&s)+4,4);\n"
   "  return (long)(lo*131u+hi);\n"
   "}\n",
   {99}, "VectorAlgo24AVX", 2, "-mavx2"},

  {"ymm_fminmax",
   V8F
   "long ymm_fminmax(long a, long b) {\n"
   "  v8f va = {(float)a,(float)b,3,4,5,6,7,8};\n"
   "  v8f vb = {(float)b,(float)a,1,2,9,10,11,12};\n"
   "  v8f mx, mn;\n"
   "  for(int i=0;i<8;i++){ mx[i]=(va[i]>vb[i])?va[i]:vb[i];\n"
   "    mn[i]=(va[i]<vb[i])?va[i]:vb[i]; }\n"
   "  float s = mx[0]+mx[1]+mn[2]+mn[3];\n"
   "  unsigned o; __builtin_memcpy(&o,&s,4); return (long)o;\n"
   "}\n",
   {10, 20}, "VectorAlgo24AVX", 2, "-mavx2 -fno-math-errno"},

  {"ymm_fcmp_blend",
   V8F
   "long ymm_fcmp_blend(long a) {\n"
   "  v8f va = {(float)a,1,-2,3,-4,5,-6,7};\n"
   "  v8f vb = {0,2,2,1,4,4,6,6};\n"
   "  v8f sel;\n"
   "  for(int i=0;i<8;i++) sel[i]=(va[i]>vb[i])?va[i]:vb[i];\n"
   "  float s = 0; for(int i=0;i<8;i++) s+=sel[i];\n"
   "  unsigned o; __builtin_memcpy(&o,&s,4); return (long)o;\n"
   "}\n",
   {7}, "VectorAlgo24AVX", 2, "-mavx2 -fno-math-errno"},

  {"ymm_hsum32",
   V8F
   "long ymm_hsum32(long a) {\n"
   "  v8f v = {(float)a,2,3,4,5,6,7,8};\n"
   "  float s=0; for(int i=0;i<8;i++) s+=v[i]*v[i];\n"
   "  unsigned o; __builtin_memcpy(&o,&s,4); return (long)o;\n"
   "}\n",
   {5}, "VectorAlgo24AVX", 2, "-mavx2 -fno-math-errno"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo24AVX, X64VectorAlgo24AVXRT,
                         ::testing::ValuesIn(kX64AVX24), rtTCName);
