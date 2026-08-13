//===- AArch64_NEONPackedRTTests.cpp - NEON packed roundtrip ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests AArch64 NEON packed vector operations through lift pipeline.
// Past bugs #28 (FADD V4S), #30-31 (Sn/Dn sub-register) were in this area.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64NEONPackRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64NEONPackRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kA64NEONPack = {
  // ========== Packed i32 add (ADD V.4S) ==========
  {"neon_add_v4s",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long neon_add_v4s(long a, long b) {\n"
   "  v4i va = {(int)a, (int)(a>>32), 0, 0};\n"
   "  v4i vb = {(int)b, (int)(b>>32), 0, 0};\n"
   "  v4i vr = va + vb;\n"
   "  return (unsigned)vr[0] | ((unsigned long)(unsigned)vr[1] << 32);\n"
   "}\n",
   {0x0000000100000002ULL, 0x0000000300000004ULL}, "NEONPacked", 1},

  // ========== Packed i16 add via C (scalar fallback, avoids INS) ==========
  {"neon_add_v8h",
   "long neon_add_v8h(long a, long b) {\n"
   "  unsigned short a0 = (unsigned short)a, b0 = (unsigned short)b;\n"
   "  unsigned short a1 = (unsigned short)(a>>16), b1 = (unsigned short)(b>>16);\n"
   "  unsigned short r0 = (unsigned short)(a0 + b0);\n"
   "  unsigned short r1 = (unsigned short)(a1 + b1);\n"
   "  return (unsigned long)r0 | ((unsigned long)r1 << 16);\n"
   "}\n",
   {(3 | (4ULL << 16)), (5 | (6ULL << 16))}, "NEONPacked"},

  // ========== Packed i8 add via C (scalar fallback, avoids INS) ==========
  {"neon_add_v16b",
   "long neon_add_v16b(long a, long b) {\n"
   "  unsigned char a0 = (unsigned char)a, b0 = (unsigned char)b;\n"
   "  unsigned char a1 = (unsigned char)(a>>8), b1 = (unsigned char)(b>>8);\n"
   "  unsigned char r0 = (unsigned char)(a0 + b0);\n"
   "  unsigned char r1 = (unsigned char)(a1 + b1);\n"
   "  return (unsigned long)r0 | ((unsigned long)r1 << 8);\n"
   "}\n",
   {0x7F01, 0x0102}, "NEONPacked"},

  // ========== Packed i32 sub (SUB V.4S) ==========
  {"neon_sub_v4s",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long neon_sub_v4s(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i vr = va - vb;\n"
   "  return (long)(unsigned)vr[0];\n"
   "}\n",
   {100, 42}, "NEONPacked", 1},

  // ========== Packed i32 mul (MUL V.4S) ==========
  {"neon_mul_v4s",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long neon_mul_v4s(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i vr = va * vb;\n"
   "  return (long)(unsigned)vr[0];\n"
   "}\n",
   {7, 6}, "NEONPacked", 1},

  // ========== Packed bitwise (AND/ORR/EOR V.16B) ==========
  {"neon_and",
   "typedef long long v2q __attribute__((vector_size(16)));\n"
   "long neon_and(long a, long b) {\n"
   "  v2q va = {a, 0};\n"
   "  v2q vb = {b, 0};\n"
   "  v2q vr = va & vb;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0xFF00FF00FF00FF00ULL, 0x0F0F0F0F0F0F0F0FULL}, "NEONPacked", 1},

  {"neon_orr",
   "typedef long long v2q __attribute__((vector_size(16)));\n"
   "long neon_orr(long a, long b) {\n"
   "  v2q va = {a, 0};\n"
   "  v2q vb = {b, 0};\n"
   "  v2q vr = va | vb;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0xFF00000000000000ULL, 0x00000000000000FFULL}, "NEONPacked", 1},

  {"neon_eor",
   "typedef long long v2q __attribute__((vector_size(16)));\n"
   "long neon_eor(long a, long b) {\n"
   "  v2q va = {a, 0};\n"
   "  v2q vb = {b, 0};\n"
   "  v2q vr = va ^ vb;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0xDEADBEEFCAFEBABEULL, 0xFFFFFFFFFFFFFFFFULL}, "NEONPacked", 1},

  // ========== Packed float add (FADD V.4S) — regression for #28 ==========
  {"neon_fadd_v4s",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long neon_fadd_v4s(long a, long b) {\n"
   "  float fa, fb;\n"
   "  int ai = (int)a, bi = (int)b;\n"
   "  __builtin_memcpy(&fa, &ai, 4); __builtin_memcpy(&fb, &bi, 4);\n"
   "  v4f va = {fa, 0, 0, 0};\n"
   "  v4f vb = {fb, 0, 0, 0};\n"
   "  v4f vr = va + vb;\n"
   "  float r0 = vr[0];\n"
   "  int ri; __builtin_memcpy(&ri, &r0, 4);\n"
   "  return (long)(unsigned)ri;\n"
   "}\n",
   {0x40A00000ULL, 0x40400000ULL}, "NEONPacked", 1},  // 5.0f + 3.0f

  // ========== Packed float sub (FSUB V.4S) ==========
  {"neon_fsub_v4s",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long neon_fsub_v4s(long a, long b) {\n"
   "  float fa, fb;\n"
   "  int ai = (int)a, bi = (int)b;\n"
   "  __builtin_memcpy(&fa, &ai, 4); __builtin_memcpy(&fb, &bi, 4);\n"
   "  v4f va = {fa, 0, 0, 0};\n"
   "  v4f vb = {fb, 0, 0, 0};\n"
   "  v4f vr = va - vb;\n"
   "  float r0 = vr[0];\n"
   "  int ri; __builtin_memcpy(&ri, &r0, 4);\n"
   "  return (long)(unsigned)ri;\n"
   "}\n",
   {0x40A00000ULL, 0x40400000ULL}, "NEONPacked", 1},

  // ========== Packed float mul (FMUL V.4S) ==========
  {"neon_fmul_v4s",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long neon_fmul_v4s(long a, long b) {\n"
   "  float fa, fb;\n"
   "  int ai = (int)a, bi = (int)b;\n"
   "  __builtin_memcpy(&fa, &ai, 4); __builtin_memcpy(&fb, &bi, 4);\n"
   "  v4f va = {fa, 0, 0, 0};\n"
   "  v4f vb = {fb, 0, 0, 0};\n"
   "  v4f vr = va * vb;\n"
   "  float r0 = vr[0];\n"
   "  int ri; __builtin_memcpy(&ri, &r0, 4);\n"
   "  return (long)(unsigned)ri;\n"
   "}\n",
   {0x40A00000ULL, 0x40400000ULL}, "NEONPacked", 1},

  // ========== Packed double add (FADD V.2D) ==========
  {"neon_fadd_v2d",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long neon_fadd_v2d(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  v2d va = {da, 0};\n"
   "  v2d vb = {db, 0};\n"
   "  v2d vr = va + vb;\n"
   "  double r0 = vr[0];\n"
   "  long ret; __builtin_memcpy(&ret, &r0, 8); return ret;\n"
   "}\n",
   {0x4008000000000000ULL, 0x4014000000000000ULL}, "NEONPacked", 1},

  // ========== Vector shift (SHL/USHR V.4S) ==========
  {"neon_shl_v4s",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long neon_shl_v4s(long a) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vr = va << 8;\n"
   "  return (long)(unsigned)vr[0];\n"
   "}\n",
   {0x12345678}, "NEONPacked", 1},

  {"neon_ushr_v4s",
   "typedef unsigned v4ui __attribute__((vector_size(16)));\n"
   "long neon_ushr_v4s(long a) {\n"
   "  v4ui va = {(unsigned)a, 0, 0, 0};\n"
   "  v4ui vr = va >> 8;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0x12345678}, "NEONPacked", 1},

  // ========== Vector compare (CMEQ V.4S) ==========
  {"neon_cmeq_v4s",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long neon_cmeq_v4s(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i vr = (va == vb);\n"
   "  return (long)(unsigned)vr[0];\n"
   "}\n",
   {42, 42}, "NEONPacked", 1},

  {"neon_cmgt_v4s",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long neon_cmgt_v4s(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i vr = (va > vb);\n"
   "  return (long)(unsigned)vr[0];\n"
   "}\n",
   {100, 42}, "NEONPacked", 1},

  // ========== Scalar FP operations (FMOV Sn regression for #31) ==========
  {"neon_scalar_fadd",
   "long neon_scalar_fadd(long a, long b) {\n"
   "  float fa, fb;\n"
   "  int ai = (int)a, bi = (int)b;\n"
   "  __builtin_memcpy(&fa, &ai, 4); __builtin_memcpy(&fb, &bi, 4);\n"
   "  float r = fa + fb;\n"
   "  int ri; __builtin_memcpy(&ri, &r, 4);\n"
   "  return (long)(unsigned)ri;\n"
   "}\n",
   {0x42280000ULL, 0x42280000ULL}, "NEONPacked"},  // 42.0f + 42.0f

  {"neon_scalar_fmul",
   "long neon_scalar_fmul(long a, long b) {\n"
   "  float fa, fb;\n"
   "  int ai = (int)a, bi = (int)b;\n"
   "  __builtin_memcpy(&fa, &ai, 4); __builtin_memcpy(&fb, &bi, 4);\n"
   "  float r = fa * fb;\n"
   "  int ri; __builtin_memcpy(&ri, &r, 4);\n"
   "  return (long)(unsigned)ri;\n"
   "}\n",
   {0x40A00000ULL, 0x40400000ULL}, "NEONPacked"},  // 5.0f * 3.0f

  {"neon_scalar_fdiv",
   "long neon_scalar_fdiv(long a, long b) {\n"
   "  float fa, fb;\n"
   "  int ai = (int)a, bi = (int)b;\n"
   "  __builtin_memcpy(&fa, &ai, 4); __builtin_memcpy(&fb, &bi, 4);\n"
   "  float r = fa / fb;\n"
   "  int ri; __builtin_memcpy(&ri, &r, 4);\n"
   "  return (long)(unsigned)ri;\n"
   "}\n",
   {0x41200000ULL, 0x40000000ULL}, "NEONPacked"},  // 10.0f / 2.0f
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(NEONPacked, A64NEONPackRT,
                         ::testing::ValuesIn(kA64NEONPack), rtTCName);
