//===- AArch64_NEONRoundTripTests.cpp - NEON roundtrip tests --*- C++ -*-===//
//
// Tests AArch64 NEON/SIMD instructions through the full lift pipeline.
// Uses max 2 args to avoid ABI non-consecutive-register detection bug.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64NEONRT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64NEONRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kA64NEON = {
  // ========== Packed 32-bit integer (ADD/SUB/MUL) ==========
  {"neon_add4i",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long neon_add4i(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0}; v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i vr = va + vb;\n"
   "  return (long)(unsigned int)vr[0];\n"
   "}\n",
   {100, 42}, "NEONRT", 1},

  {"neon_sub4i",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long neon_sub4i(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0}; v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i vr = va - vb;\n"
   "  return (long)(unsigned int)vr[0];\n"
   "}\n",
   {100, 42}, "NEONRT", 1},

  {"neon_mul4i",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long neon_mul4i(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0}; v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i vr = va * vb;\n"
   "  return (long)(unsigned int)vr[0];\n"
   "}\n",
   {7, 6}, "NEONRT", 1},

  // ========== Packed 16-bit ==========
  {"neon_add8h",
   "typedef short v8h __attribute__((vector_size(16)));\n"
   "long neon_add8h(long a, long b) {\n"
   "  v8h va = {(short)a, 0,0,0,0,0,0,0};\n"
   "  v8h vb = {(short)b, 0,0,0,0,0,0,0};\n"
   "  v8h vr = va + vb;\n"
   "  return (long)(unsigned short)vr[0];\n"
   "}\n",
   {100, 42}, "NEONRT", 1},

  // ========== Packed 8-bit ==========
  {"neon_add16b",
   "typedef char v16b __attribute__((vector_size(16)));\n"
   "long neon_add16b(long a, long b) {\n"
   "  v16b va = {(char)a, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0};\n"
   "  v16b vb = {(char)b, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0};\n"
   "  v16b vr = va + vb;\n"
   "  return (long)(unsigned char)vr[0];\n"
   "}\n",
   {100, 42}, "NEONRT", 1},

  // ========== Packed 64-bit ==========
  {"neon_add2q",
   "typedef long long v2q __attribute__((vector_size(16)));\n"
   "long neon_add2q(long a, long b) {\n"
   "  v2q va = {a, 0}; v2q vb = {b, 0};\n"
   "  v2q vr = va + vb;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0x123456789ABCDEF0ULL, 0x0FEDCBA987654321ULL}, "NEONRT", 1},

  // ========== Bitwise (AND/ORR/EOR) ==========
  {"neon_and",
   "typedef long long v2q __attribute__((vector_size(16)));\n"
   "long neon_and(long a, long b) {\n"
   "  v2q va = {a, 0}; v2q vb = {b, 0};\n"
   "  v2q vr = va & vb;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0xFF00FF00FF00FF00ULL, 0x0FF00FF00FF00FF0ULL}, "NEONRT", 1},

  {"neon_orr",
   "typedef long long v2q __attribute__((vector_size(16)));\n"
   "long neon_orr(long a, long b) {\n"
   "  v2q va = {a, 0}; v2q vb = {b, 0};\n"
   "  v2q vr = va | vb;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0xFF00FF00FF00FF00ULL, 0x0FF00FF00FF00FF0ULL}, "NEONRT", 1},

  {"neon_eor",
   "typedef long long v2q __attribute__((vector_size(16)));\n"
   "long neon_eor(long a, long b) {\n"
   "  v2q va = {a, 0}; v2q vb = {b, 0};\n"
   "  v2q vr = va ^ vb;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0xFF00FF00FF00FF00ULL, 0x0FF00FF00FF00FF0ULL}, "NEONRT", 1},

  // ========== Shift ==========
  {"neon_shl4i",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long neon_shl4i(long a) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vr = va << 8;\n"
   "  return (long)(unsigned int)vr[0];\n"
   "}\n",
   {0xDEADBEEFULL}, "NEONRT", 1},

  {"neon_ushr4i",
   "typedef unsigned int v4ui __attribute__((vector_size(16)));\n"
   "long neon_ushr4i(long a) {\n"
   "  v4ui va = {(unsigned int)a, 0, 0, 0};\n"
   "  v4ui vr = va >> 8;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0xDEADBEEFULL}, "NEONRT", 1},

  // ========== Compare ==========
  {"neon_cmeq4i",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long neon_cmeq4i(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0}; v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i vr = (va == vb);\n"
   "  return (long)(unsigned int)vr[0];\n"
   "}\n",
   {42, 42}, "NEONRT", 1},

  {"neon_cmgt4i",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long neon_cmgt4i(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0}; v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i vr = (va > vb);\n"
   "  return (long)(unsigned int)vr[0];\n"
   "}\n",
   {100, 42}, "NEONRT", 1},

  // ========== Negate ==========
  {"neon_neg4i",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long neon_neg4i(long a) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vr = -va;\n"
   "  return (long)(unsigned int)vr[0];\n"
   "}\n",
   {42}, "NEONRT", 1},

  // ========== Packed double FP ==========
  {"neon_fadd2d",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long neon_fadd2d(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  v2d va = {da, 0.0}; v2d vb = {db, 0.0};\n"
   "  v2d vr = va + vb;\n"
   "  double r0 = vr[0];\n"
   "  long r; __builtin_memcpy(&r, &r0, 8); return r;\n"
   "}\n",
   {0x4014000000000000ULL, 0x4008000000000000ULL}, "NEONRT", 1},

  {"neon_fmul2d",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long neon_fmul2d(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  v2d va = {da, 0.0}; v2d vb = {db, 0.0};\n"
   "  v2d vr = va * vb;\n"
   "  double r0 = vr[0];\n"
   "  long r; __builtin_memcpy(&r, &r0, 8); return r;\n"
   "}\n",
   {0x4014000000000000ULL, 0x4008000000000000ULL}, "NEONRT", 1},

  {"neon_fadd4s",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long neon_fadd4s(long a, long b) {\n"
   "  int ai = (int)a, bi = (int)b;\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &ai, 4); __builtin_memcpy(&fb, &bi, 4);\n"
   "  v4f va = {fa, 0, 0, 0}; v4f vb = {fb, 0, 0, 0};\n"
   "  v4f vr = va + vb;\n"
   "  float r0 = vr[0];\n"
   "  int ri; __builtin_memcpy(&ri, &r0, 4);\n"
   "  return (long)(unsigned int)ri;\n"
   "}\n",
   {0x40A00000ULL, 0x3F800000ULL}, "NEONRT", 1},

  {"neon_fmul4s",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long neon_fmul4s(long a, long b) {\n"
   "  int ai = (int)a, bi = (int)b;\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &ai, 4); __builtin_memcpy(&fb, &bi, 4);\n"
   "  v4f va = {fa, 0, 0, 0}; v4f vb = {fb, 0, 0, 0};\n"
   "  v4f vr = va * vb;\n"
   "  float r0 = vr[0];\n"
   "  int ri; __builtin_memcpy(&ri, &r0, 4);\n"
   "  return (long)(unsigned int)ri;\n"
   "}\n",
   {0x40A00000ULL, 0x40000000ULL}, "NEONRT", 1},

  {"neon_fsub2d",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long neon_fsub2d(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  v2d va = {da, 0.0}; v2d vb = {db, 0.0};\n"
   "  v2d vr = va - vb;\n"
   "  double r0 = vr[0];\n"
   "  long r; __builtin_memcpy(&r, &r0, 8); return r;\n"
   "}\n",
   {0x4024000000000000ULL, 0x4008000000000000ULL}, "NEONRT", 1},

  // ========== Horizontal sum ==========
  {"neon_hsum",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long neon_hsum(long ab) {\n"
   "  v4i v = {(int)ab, (int)(ab>>32), 0, 0};\n"
   "  return (long)(v[0]+v[1]);\n"
   "}\n",
   {((uint64_t)20<<32)|10}, "NEONRT", 1},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(NEONRT, A64NEONRT,
                         ::testing::ValuesIn(kA64NEON), rtTCName);
