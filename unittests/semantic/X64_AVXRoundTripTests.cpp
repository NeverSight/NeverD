//===- X64_AVXRoundTripTests.cpp - AVX/VEX roundtrip tests ------*- C++ -*-===//
//
// Tests x86_64 AVX/VEX-encoded SIMD instructions through the full lift
// pipeline.  VEX 3-operand variants historically shared per-lane bugs with
// their SSE counterparts (#44-#47).  Uses -mavx2 -O1 to generate VEX code.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64AVXRT : public SemanticRoundTripFixture,
                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64AVXRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

#define V16C  "typedef char v16c __attribute__((vector_size(16)));\n"
#define V16UC "typedef unsigned char v16uc __attribute__((vector_size(16)));\n"
#define V8S   "typedef short v8s __attribute__((vector_size(16)));\n"
#define V8US  "typedef unsigned short v8us __attribute__((vector_size(16)));\n"
#define V4I   "typedef int v4i __attribute__((vector_size(16)));\n"
#define V4UI  "typedef unsigned int v4ui __attribute__((vector_size(16)));\n"
#define V2Q   "typedef long long v2q __attribute__((vector_size(16)));\n"
#define V4F   "typedef float v4f __attribute__((vector_size(16)));\n"
#define V2D   "typedef double v2d __attribute__((vector_size(16)));\n"

static const std::vector<RoundTripTC> kX64AVX = {

  // ===== VPADDD — VEX packed 32-bit add =====
  {"vpaddd_4i",
   V4I
   "long vpaddd_4i(long a, long b) {\n"
   "  v4i va = {(int)a, 10, 20, 30};\n"
   "  v4i vb = {(int)b, 5, 7, 9};\n"
   "  v4i vr = va + vb;\n"
   "  return (unsigned int)vr[0] | ((unsigned long)(unsigned int)vr[1] << 32);\n"
   "}\n",
   {100, 42}, "AVX", 1, "-mavx2"},

  // ===== VPSUBD — VEX packed 32-bit sub =====
  {"vpsubd_4i",
   V4I
   "long vpsubd_4i(long a, long b) {\n"
   "  v4i va = {(int)a, 100, 0, 0};\n"
   "  v4i vb = {(int)b, 30, 0, 0};\n"
   "  v4i vr = va - vb;\n"
   "  return (unsigned int)vr[0] | ((unsigned long)(unsigned int)vr[1] << 32);\n"
   "}\n",
   {100, 42}, "AVX", 1, "-mavx2"},

  // ===== VPADDW — VEX packed 16-bit add =====
  {"vpaddw_8s",
   V8S
   "long vpaddw_8s(long a, long b) {\n"
   "  v8s va = {(short)a, 10, 20, 30, 0, 0, 0, 0};\n"
   "  v8s vb = {(short)b, 5, 3, 7, 0, 0, 0, 0};\n"
   "  v8s vr = va + vb;\n"
   "  return (unsigned short)vr[0] | ((unsigned long)(unsigned short)vr[1] << 16);\n"
   "}\n",
   {100, 42}, "AVX", 1, "-mavx2"},

  // ===== VPADDB — VEX packed 8-bit add =====
  {"vpaddb_16c",
   V16UC
   "long vpaddb_16c(long a, long b) {\n"
   "  v16uc va = {(unsigned char)a, 10, 0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16uc vb = {(unsigned char)b, 5, 0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16uc vr = va + vb;\n"
   "  return (unsigned char)vr[0] | ((unsigned long)(unsigned char)vr[1] << 8);\n"
   "}\n",
   {10, 20}, "AVX", 1, "-mavx2"},

  // ===== VPCMPEQD — VEX packed 32-bit equality =====
  {"vpcmpeqd_hit",
   V4I
   "long vpcmpeqd_hit(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i cmp = (va == vb);\n"
   "  return (unsigned int)cmp[0];\n"
   "}\n",
   {42, 42}, "AVX", 1, "-mavx2"},

  {"vpcmpeqd_miss",
   V4I
   "long vpcmpeqd_miss(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i cmp = (va == vb);\n"
   "  return (unsigned int)cmp[0];\n"
   "}\n",
   {42, 99}, "AVX", 1, "-mavx2"},

  // ===== VPCMPGTD — VEX packed 32-bit signed greater-than =====
  {"vpcmpgtd_hit",
   V4I
   "long vpcmpgtd_hit(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i cmp = (va > vb);\n"
   "  return (unsigned int)cmp[0];\n"
   "}\n",
   {99, 42}, "AVX", 1, "-mavx2"},

  // ===== VPMULLD — VEX packed 32-bit multiply low =====
  {"vpmulld_4i",
   V4I
   "long vpmulld_4i(long a, long b) {\n"
   "  v4i va = {(int)a, 3, 0, 0};\n"
   "  v4i vb = {(int)b, 7, 0, 0};\n"
   "  v4i vr = va * vb;\n"
   "  return (unsigned int)vr[0] | ((unsigned long)(unsigned int)vr[1] << 32);\n"
   "}\n",
   {6, 7}, "AVX", 1, "-mavx2"},

  // ===== VPMULLW — VEX packed 16-bit multiply low =====
  {"vpmullw_8s",
   V8S
   "long vpmullw_8s(long a, long b) {\n"
   "  v8s va = {(short)a, 5, 0,0,0,0,0,0};\n"
   "  v8s vb = {(short)b, 3, 0,0,0,0,0,0};\n"
   "  v8s vr = va * vb;\n"
   "  return (unsigned short)vr[0] | ((unsigned long)(unsigned short)vr[1] << 16);\n"
   "}\n",
   {6, 7}, "AVX", 1, "-mavx2"},

  // ===== VPAND/VPOR/VPXOR — VEX bitwise =====
  {"vpand_4i",
   V4I
   "long vpand_4i(long a, long b) {\n"
   "  v4i va = {(int)a, (int)~0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i vr = va & vb;\n"
   "  return (unsigned int)vr[0] | ((unsigned long)(unsigned int)vr[1] << 32);\n"
   "}\n",
   {0xFF00FF00ULL, 0x0F0F0F0FULL}, "AVX", 1, "-mavx2"},

  {"vpor_4i",
   V4I
   "long vpor_4i(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i vr = va | vb;\n"
   "  return (unsigned int)vr[0];\n"
   "}\n",
   {0xAAAA5555ULL, 0x5555AAAAULL}, "AVX", 1, "-mavx2"},

  {"vpxor_4i",
   V4I
   "long vpxor_4i(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i vr = va ^ vb;\n"
   "  return (unsigned int)vr[0];\n"
   "}\n",
   {0xDEADBEEFULL, 0xCAFEBABEULL}, "AVX", 1, "-mavx2"},

  // ===== VPSLLD/VPSRLD — VEX packed shift =====
  {"vpslld_4ui",
   V4UI
   "long vpslld_4ui(long a, long b) {\n"
   "  v4ui va = {(unsigned int)a, (unsigned int)b, 0, 0};\n"
   "  v4ui vr = va << 4;\n"
   "  return (unsigned int)vr[0] | ((unsigned long)(unsigned int)vr[1] << 32);\n"
   "}\n",
   {0xFF, 0x80000001ULL}, "AVX", 1, "-mavx2"},

  {"vpsrld_4ui",
   V4UI
   "long vpsrld_4ui(long a, long b) {\n"
   "  v4ui va = {(unsigned int)a, (unsigned int)b, 0, 0};\n"
   "  v4ui vr = va >> 4;\n"
   "  return (unsigned int)vr[0] | ((unsigned long)(unsigned int)vr[1] << 32);\n"
   "}\n",
   {0xFF0, 0x80000010ULL}, "AVX", 1, "-mavx2"},

  // ===== VPMINSD/VPMAXSD — scalar C expression (avoids vectorizer) =====
  {"vpminsd_c",
   "long vpminsd_c(long a, long b) {\n"
   "  int x = (int)a, y = (int)b;\n"
   "  return (long)(x < y ? x : y);\n"
   "}\n",
   {42, 99}, "AVX", 0, "-mavx2"},

  {"vpmaxsd_c",
   "long vpmaxsd_c(long a, long b) {\n"
   "  int x = (int)a, y = (int)b;\n"
   "  return (long)(x > y ? x : y);\n"
   "}\n",
   {42, 99}, "AVX", 0, "-mavx2"},

  // ===== abs scalar =====
  {"vpabsd_c",
   "long vpabsd_c(long a) {\n"
   "  int x = (int)a;\n"
   "  return (long)(x < 0 ? -x : x);\n"
   "}\n",
   {(uint64_t)(int64_t)-99}, "AVX", 0, "-mavx2"},

  // ===== VEX packed float =====
  {"vaddps_4f",
   V4F
   "long vaddps_4f(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  v4f va = {fa, 1.0f, 0.0f, 0.0f};\n"
   "  v4f vb = {fb, 2.0f, 0.0f, 0.0f};\n"
   "  v4f vr = va + vb;\n"
   "  float r = vr[0]; long ret; __builtin_memcpy(&ret, &r, 4);\n"
   "  return ret;\n"
   "}\n",
   {0x40A00000ULL, 0x40400000ULL}, "AVX", 1, "-mavx2"},

  {"vmulps_4f",
   V4F
   "long vmulps_4f(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  v4f va = {fa, 2.0f, 0.0f, 0.0f};\n"
   "  v4f vb = {fb, 3.0f, 0.0f, 0.0f};\n"
   "  v4f vr = va * vb;\n"
   "  float r = vr[0]; long ret; __builtin_memcpy(&ret, &r, 4);\n"
   "  return ret;\n"
   "}\n",
   {0x40A00000ULL, 0x40400000ULL}, "AVX", 1, "-mavx2"},

  // ===== True 3-operand VEX (src1 != dst): exercises the Unicorn VEX.vvvv fix.
  // Reusing both source vectors forces clang to keep them in fixed registers
  // and emit "vop dst, src1, src2" with src1 distinct from dst.  The -mavx
  // probes build vectors via vpblendw, the -mavx2 ones via vpblendd /
  // vbroadcastss (all now supported by the Unicorn fork). =====
  {"vsubmul_3op",
   V4F
   "long vsubmul_3op(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  v4f va = {fa, 1.0f, 2.0f, 3.0f};\n"
   "  v4f vb = {fb, 4.0f, 5.0f, 6.0f};\n"
   "  v4f d = va - vb; v4f p = va * vb; v4f r = d + p;\n"
   "  float s = r[0] + r[1] + r[2] + r[3];\n"
   "  long ret; __builtin_memcpy(&ret, &s, 4); return ret;\n"
   "}\n",
   {0x40A00000ULL, 0x40400000ULL}, "AVX", 1, "-mavx"},

  {"vminmax_3op",
   V4F
   "long vminmax_3op(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  v4f va = {fa, 1.0f, 9.0f, 4.0f};\n"
   "  v4f vb = {fb, 3.0f, 2.0f, 4.0f};\n"
   "  v4f mn = __builtin_elementwise_min(va, vb);\n"
   "  v4f mx = __builtin_elementwise_max(va, vb);\n"
   "  v4f r = mx - mn;\n"
   "  float s = r[0] + r[1] + r[2] + r[3];\n"
   "  long ret; __builtin_memcpy(&ret, &s, 4); return ret;\n"
   "}\n",
   {0x40A00000ULL, 0x40400000ULL}, "AVX", 1, "-mavx"},

  {"vshuf_3op",
   V4F
   "long vshuf_3op(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  v4f va = {fa, 1.0f, 2.0f, 3.0f};\n"
   "  v4f vb = {fb, 4.0f, 5.0f, 6.0f};\n"
   "  v4f s = __builtin_shufflevector(va, vb, 0, 4, 1, 5);\n"
   "  v4f t = __builtin_shufflevector(va, vb, 2, 6, 3, 7);\n"
   "  v4f r = s + t;\n"
   "  float sum = r[0] + r[1] + r[2] + r[3];\n"
   "  long ret; __builtin_memcpy(&ret, &sum, 4); return ret;\n"
   "}\n",
   {0x40A00000ULL, 0x40400000ULL}, "AVX", 1, "-mavx2"},

  {"vdiv_3op",
   V4F
   "long vdiv_3op(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  v4f va = {fa, 8.0f, 9.0f, 12.0f};\n"
   "  v4f vb = {fb, 2.0f, 3.0f, 4.0f};\n"
   "  v4f q = va / vb; v4f s = va + vb; v4f r = q + s;\n"
   "  float sum = r[0] + r[1] + r[2] + r[3];\n"
   "  long ret; __builtin_memcpy(&ret, &sum, 4); return ret;\n"
   "}\n",
   {0x41200000ULL, 0x40000000ULL}, "AVX", 1, "-mavx2"},

  // VEX FP compare producing a mask (vcmpltps, 3-operand) plus the AVX2
  // vbroadcastss its reduction emits -- both now execute correctly in Unicorn.
  {"vcmp_count_3op",
   V4F V4I
   "long vcmp_count_3op(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  v4f va = {fa, 1.0f, 9.0f, 4.0f};\n"
   "  v4f vb = {fb, 3.0f, 2.0f, 4.0f};\n"
   "  v4i lt = (va < vb); v4i gt = (va > vb);\n"
   "  int c = -(lt[0]+lt[1]+lt[2]+lt[3] + gt[0]+gt[1]+gt[2]+gt[3]);\n"
   "  return (long)c;\n"
   "}\n",
   {0x40000000ULL, 0x40A00000ULL}, "AVX", 1, "-mavx2"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(AVX, X64AVXRT,
                         ::testing::ValuesIn(kX64AVX), rtTCName);
