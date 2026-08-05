//===- X64_SIMDRoundTripTests.cpp - SSE/AVX roundtrip tests ---*- C++ -*-===//
//
// Tests x86_64 SIMD instructions through the full lift pipeline.
// Uses 2-arg functions to avoid the ABI non-consecutive-register bug.
// All tests use -O1 for clean SIMD instruction output.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SIMDRT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SIMDRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

// 5.0=0x4014000000000000 3.0=0x4008000000000000
// 2.0=0x4000000000000000 1.0=0x3FF0000000000000

#define V2D_SETUP \
  "typedef double v2d __attribute__((vector_size(16)));\n"
#define V4F_SETUP \
  "typedef float v4f __attribute__((vector_size(16)));\n"
#define V4I_SETUP \
  "typedef int v4i __attribute__((vector_size(16)));\n"
#define V8S_SETUP \
  "typedef short v8s __attribute__((vector_size(16)));\n"
#define V16C_SETUP \
  "typedef char v16c __attribute__((vector_size(16)));\n"
#define V2Q_SETUP \
  "typedef long long v2q __attribute__((vector_size(16)));\n"
#define V4UI_SETUP \
  "typedef unsigned int v4ui __attribute__((vector_size(16)));\n"
#define V2UQ_SETUP \
  "typedef unsigned long long v2uq __attribute__((vector_size(16)));\n"

static const std::vector<RoundTripTC> kX64SIMD = {
  // ========== Packed double (SSE2 ADDPD/SUBPD/MULPD/DIVPD) ==========
  {"simd_addpd",
   V2D_SETUP
   "long simd_addpd(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da,&a,8); __builtin_memcpy(&db,&b,8);\n"
   "  v2d va = {da, 0.0}; v2d vb = {db, 0.0};\n"
   "  v2d vr = va + vb;\n"
   "  double r0 = vr[0];\n"
   "  long r; __builtin_memcpy(&r,&r0,8); return r;\n"
   "}\n",
   {0x4014000000000000ULL, 0x3FF0000000000000ULL}, "SIMDRT", 1},

  {"simd_subpd",
   V2D_SETUP
   "long simd_subpd(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da,&a,8); __builtin_memcpy(&db,&b,8);\n"
   "  v2d va = {da, 0.0}; v2d vb = {db, 0.0};\n"
   "  v2d vr = va - vb;\n"
   "  double r0 = vr[0];\n"
   "  long r; __builtin_memcpy(&r,&r0,8); return r;\n"
   "}\n",
   {0x4024000000000000ULL, 0x4008000000000000ULL}, "SIMDRT", 1},

  {"simd_mulpd",
   V2D_SETUP
   "long simd_mulpd(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da,&a,8); __builtin_memcpy(&db,&b,8);\n"
   "  v2d va = {da, 0.0}; v2d vb = {db, 0.0};\n"
   "  v2d vr = va * vb;\n"
   "  double r0 = vr[0];\n"
   "  long r; __builtin_memcpy(&r,&r0,8); return r;\n"
   "}\n",
   {0x4014000000000000ULL, 0x4008000000000000ULL}, "SIMDRT", 1},

  {"simd_divpd",
   V2D_SETUP
   "long simd_divpd(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da,&a,8); __builtin_memcpy(&db,&b,8);\n"
   "  v2d va = {da, 0.0}; v2d vb = {db, 0.0};\n"
   "  v2d vr = va / vb;\n"
   "  double r0 = vr[0];\n"
   "  long r; __builtin_memcpy(&r,&r0,8); return r;\n"
   "}\n",
   {0x4024000000000000ULL, 0x4000000000000000ULL}, "SIMDRT", 1},

  // ========== Packed float (SSE ADDPS/MULPS) ==========
  {"simd_addps",
   V4F_SETUP
   "long simd_addps(long a, long b) {\n"
   "  int ai=(int)a, bi=(int)b;\n"
   "  float fa,fb;\n"
   "  __builtin_memcpy(&fa,&ai,4); __builtin_memcpy(&fb,&bi,4);\n"
   "  v4f va = {fa,0,0,0}; v4f vb = {fb,0,0,0};\n"
   "  v4f vr = va + vb;\n"
   "  float r0 = vr[0];\n"
   "  int ri; __builtin_memcpy(&ri,&r0,4);\n"
   "  return (long)(unsigned int)ri;\n"
   "}\n",
   {0x40A00000ULL, 0x3F800000ULL}, "SIMDRT", 1},

  {"simd_mulps",
   V4F_SETUP
   "long simd_mulps(long a, long b) {\n"
   "  int ai=(int)a, bi=(int)b;\n"
   "  float fa,fb;\n"
   "  __builtin_memcpy(&fa,&ai,4); __builtin_memcpy(&fb,&bi,4);\n"
   "  v4f va = {fa,0,0,0}; v4f vb = {fb,0,0,0};\n"
   "  v4f vr = va * vb;\n"
   "  float r0 = vr[0];\n"
   "  int ri; __builtin_memcpy(&ri,&r0,4);\n"
   "  return (long)(unsigned int)ri;\n"
   "}\n",
   {0x40A00000ULL, 0x40000000ULL}, "SIMDRT", 1},

  // ========== Packed integer 32-bit (SSE2 PADDD/PSUBD) ==========
  {"simd_paddd",
   V4I_SETUP
   "long simd_paddd(long a, long b) {\n"
   "  v4i va = {(int)a, (int)(a>>32), 0, 0};\n"
   "  v4i vb = {(int)b, (int)(b>>32), 0, 0};\n"
   "  v4i vr = va + vb;\n"
   "  return (long)(unsigned int)vr[0];\n"
   "}\n",
   {100, 42}, "SIMDRT", 1},

  {"simd_psubd",
   V4I_SETUP
   "long simd_psubd(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i vr = va - vb;\n"
   "  return (long)(unsigned int)vr[0];\n"
   "}\n",
   {100, 42}, "SIMDRT", 1},

  // ========== Packed integer 16-bit (PADDW) ==========
  {"simd_paddw",
   V8S_SETUP
   "long simd_paddw(long a, long b) {\n"
   "  v8s va = {(short)a, 0,0,0,0,0,0,0};\n"
   "  v8s vb = {(short)b, 0,0,0,0,0,0,0};\n"
   "  v8s vr = va + vb;\n"
   "  return (long)(unsigned short)vr[0];\n"
   "}\n",
   {100, 42}, "SIMDRT", 1},

  // ========== Packed integer 8-bit (PADDB) ==========
  {"simd_paddb",
   V16C_SETUP
   "long simd_paddb(long a, long b) {\n"
   "  v16c va = {(char)a, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0};\n"
   "  v16c vb = {(char)b, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0};\n"
   "  v16c vr = va + vb;\n"
   "  return (long)(unsigned char)vr[0];\n"
   "}\n",
   {100, 42}, "SIMDRT", 1},

  // ========== Packed integer 64-bit (PADDQ) ==========
  {"simd_paddq",
   V2Q_SETUP
   "long simd_paddq(long a, long b) {\n"
   "  v2q va = {a, 0}; v2q vb = {b, 0};\n"
   "  v2q vr = va + vb;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0x123456789ABCDEF0ULL, 0x0FEDCBA987654321ULL}, "SIMDRT", 1},

  // ========== Packed bitwise (PAND/POR/PXOR) ==========
  {"simd_pand",
   V2Q_SETUP
   "long simd_pand(long a, long b) {\n"
   "  v2q va = {a, 0}; v2q vb = {b, 0};\n"
   "  v2q vr = va & vb;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0xFF00FF00FF00FF00ULL, 0x0FF00FF00FF00FF0ULL}, "SIMDRT", 1},

  {"simd_por",
   V2Q_SETUP
   "long simd_por(long a, long b) {\n"
   "  v2q va = {a, 0}; v2q vb = {b, 0};\n"
   "  v2q vr = va | vb;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0xFF00FF00FF00FF00ULL, 0x0FF00FF00FF00FF0ULL}, "SIMDRT", 1},

  {"simd_pxor",
   V2Q_SETUP
   "long simd_pxor(long a, long b) {\n"
   "  v2q va = {a, 0}; v2q vb = {b, 0};\n"
   "  v2q vr = va ^ vb;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0xFF00FF00FF00FF00ULL, 0x0FF00FF00FF00FF0ULL}, "SIMDRT", 1},

  // ========== Packed shift (PSLLQ/PSRLQ/PSLLD) ==========
  {"simd_psllq",
   V2Q_SETUP
   "long simd_psllq(long a) {\n"
   "  v2q va = {a, 0};\n"
   "  v2q vr = va << 4;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0xDEADBEEFULL}, "SIMDRT", 1},

  {"simd_psrlq",
   V2UQ_SETUP
   "long simd_psrlq(long a) {\n"
   "  v2uq va = {(unsigned long long)a, 0};\n"
   "  v2uq vr = va >> 4;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0xDEADBEEFCAFEBABEULL}, "SIMDRT", 1},

  {"simd_pslld",
   V4I_SETUP
   "long simd_pslld(long a) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vr = va << 8;\n"
   "  return (long)(unsigned int)vr[0];\n"
   "}\n",
   {0xDEADBEEFULL}, "SIMDRT", 1},

  // ========== Packed compare (PCMPEQD/PCMPGTD) ==========
  {"simd_pcmpeqd",
   V4I_SETUP
   "long simd_pcmpeqd(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i vr = (va == vb);\n"
   "  return (long)(unsigned int)vr[0];\n"
   "}\n",
   {42, 42}, "SIMDRT", 1},

  {"simd_pcmpgtd",
   V4I_SETUP
   "long simd_pcmpgtd(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i vr = (va > vb);\n"
   "  return (long)(unsigned int)vr[0];\n"
   "}\n",
   {100, 42}, "SIMDRT", 1},

  // ========== Vector negate ==========
  {"simd_pnegd",
   V4I_SETUP
   "long simd_pnegd(long a) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vr = -va;\n"
   "  return (long)(unsigned int)vr[0];\n"
   "}\n",
   {42}, "SIMDRT", 1},

  {"simd_pabsd",
   V4I_SETUP
   "long simd_pabsd(long a) {\n"
   "  int x = (int)a;\n"
   "  return (long)(unsigned int)(x < 0 ? -x : x);\n"
   "}\n",
   {(uint64_t)(int64_t)-42}, "SIMDRT", 1},

  // ========== Packed multiply (PMULLD) ==========
  {"simd_pmulld",
   V4I_SETUP
   "long simd_pmulld(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0}; v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i vr = va * vb;\n"
   "  return (long)(unsigned int)vr[0];\n"
   "}\n",
   {7, 6}, "SIMDRT", 1},

  // ========== PMADDWD (multiply-add) pattern ==========
  {"simd_muladdw",
   V8S_SETUP
   "long simd_muladdw(long a, long b) {\n"
   "  v8s va = {(short)a, 0,0,0,0,0,0,0};\n"
   "  v8s vb = {(short)b, 0,0,0,0,0,0,0};\n"
   "  v8s vr = va * vb;\n"
   "  return (long)(unsigned short)vr[0];\n"
   "}\n",
   {7, 6}, "SIMDRT", 1},

  // ========== PMINSD/PMAXSD pattern ==========
  {"simd_min",
   "long simd_min(long a, long b) {\n"
   "  int x = (int)a, y = (int)b;\n"
   "  return (long)(unsigned int)(x < y ? x : y);\n"
   "}\n",
   {42, 100}, "SIMDRT", 1},

  {"simd_max",
   "long simd_max(long a, long b) {\n"
   "  int x = (int)a, y = (int)b;\n"
   "  return (long)(unsigned int)(x > y ? x : y);\n"
   "}\n",
   {42, 100}, "SIMDRT", 1},

  // ========== Dot product (MULPD+extract+add) ==========
  {"simd_dot2d",
   V2D_SETUP
   "long simd_dot2d(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da,&a,8); __builtin_memcpy(&db,&b,8);\n"
   "  v2d va = {da, da}; v2d vb = {db, db};\n"
   "  v2d prod = va * vb;\n"
   "  double result = prod[0] + prod[1];\n"
   "  long r; __builtin_memcpy(&r,&result,8); return r;\n"
   "}\n",
   {0x4008000000000000ULL, 0x4000000000000000ULL}, "SIMDRT", 1},

  // ========== Horizontal sum (simple 2-element to avoid return type issue) ==========
  {"simd_hsum2q",
   V2Q_SETUP
   "long simd_hsum2q(long a) {\n"
   "  v2q v = {a, a};\n"
   "  return (long)(v[0]+v[1]);\n"
   "}\n",
   {42}, "SIMDRT", 1},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(SIMDRT, X64SIMDRT,
                         ::testing::ValuesIn(kX64SIMD), rtTCName);
