//===- X64_PackedFPRTTests.cpp - SSE packed FP roundtrip -------*- C++ -*-===//
//
// Tests x86_64 SSE/SSE2 packed floating-point operations.
// Bug #28 (FADD V4S on AArch64) showed that packed FP can be mislifted
// as full-width ops. These tests verify per-lane FP correctness on x86.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64PackedFPRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64PackedFPRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

// Helper: interpret long bits as double
#define DBITS(x) x##ULL

static const std::vector<RoundTripTC> kX64PackedFP = {
  // ========== Packed float add (ADDPS) ==========
  {"addps_lane0",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long addps_lane0(long a, long b) {\n"
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
   {0x40A00000ULL, 0x40400000ULL}, "PackedFP", 1},  // 5.0f + 3.0f

  // ========== Packed float sub (SUBPS) ==========
  {"subps_lane0",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long subps_lane0(long a, long b) {\n"
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
   {0x40A00000ULL, 0x40400000ULL}, "PackedFP", 1},  // 5.0f - 3.0f

  // ========== Packed float mul (MULPS) ==========
  {"mulps_lane0",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long mulps_lane0(long a, long b) {\n"
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
   {0x40A00000ULL, 0x40400000ULL}, "PackedFP", 1},  // 5.0f * 3.0f

  // ========== Packed double add (ADDPD) ==========
  {"addpd_lane0",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long addpd_lane0(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  v2d va = {da, 0};\n"
   "  v2d vb = {db, 0};\n"
   "  v2d vr = va + vb;\n"
   "  double r0 = vr[0];\n"
   "  long ret; __builtin_memcpy(&ret, &r0, 8); return ret;\n"
   "}\n",
   {0x4008000000000000ULL, 0x4014000000000000ULL}, "PackedFP", 1},

  // ========== Packed double sub (SUBPD) ==========
  {"subpd_lane0",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long subpd_lane0(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  v2d va = {da, 0};\n"
   "  v2d vb = {db, 0};\n"
   "  v2d vr = va - vb;\n"
   "  double r0 = vr[0];\n"
   "  long ret; __builtin_memcpy(&ret, &r0, 8); return ret;\n"
   "}\n",
   {0x4014000000000000ULL, 0x4008000000000000ULL}, "PackedFP", 1},

  // ========== Packed double mul (MULPD) ==========
  {"mulpd_lane0",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long mulpd_lane0(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  v2d va = {da, 0};\n"
   "  v2d vb = {db, 0};\n"
   "  v2d vr = va * vb;\n"
   "  double r0 = vr[0];\n"
   "  long ret; __builtin_memcpy(&ret, &r0, 8); return ret;\n"
   "}\n",
   {0x4008000000000000ULL, 0x4014000000000000ULL}, "PackedFP", 1},

  // ========== Packed float neg (XORPS with sign mask) ==========
  {"xorps_negate",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long xorps_negate(long a) {\n"
   "  float fa;\n"
   "  int ai = (int)a;\n"
   "  __builtin_memcpy(&fa, &ai, 4);\n"
   "  v4f va = {fa, 0, 0, 0};\n"
   "  v4f vr = -va;\n"
   "  float r0 = vr[0];\n"
   "  int ri; __builtin_memcpy(&ri, &r0, 4);\n"
   "  return (long)(unsigned)ri;\n"
   "}\n",
   {0x42280000ULL}, "PackedFP", 1},  // -42.0f

  // ========== SSE shift operations (PSLLW/PSRLW/PSRAW) ==========
  {"psllw_imm",
   "typedef short v8s __attribute__((vector_size(16)));\n"
   "long psllw_imm(long a) {\n"
   "  v8s va = {(short)a, (short)(a>>16), 0,0,0,0,0,0};\n"
   "  v8s vr = va << 4;\n"
   "  return (unsigned short)vr[0] | ((unsigned long)(unsigned short)vr[1] << 16);\n"
   "}\n",
   {(0x0F | (0x1FUL << 16))}, "PackedFP", 1},

  {"psrlw_imm",
   "typedef unsigned short v8us __attribute__((vector_size(16)));\n"
   "long psrlw_imm(long a) {\n"
   "  v8us va = {(unsigned short)a, (unsigned short)(a>>16), 0,0,0,0,0,0};\n"
   "  v8us vr = va >> 4;\n"
   "  return vr[0] | ((unsigned long)vr[1] << 16);\n"
   "}\n",
   {(0xFF00 | (0x1234UL << 16))}, "PackedFP", 1},

  {"pslld_imm",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long pslld_imm(long a) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vr = va << 8;\n"
   "  return (long)(unsigned)vr[0];\n"
   "}\n",
   {0x12345678}, "PackedFP", 1},

  {"psrld_imm",
   "typedef unsigned v4ui __attribute__((vector_size(16)));\n"
   "long psrld_imm(long a) {\n"
   "  v4ui va = {(unsigned)a, 0, 0, 0};\n"
   "  v4ui vr = va >> 8;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0x12345678}, "PackedFP", 1},

  // ========== MOVDQA/MOVDQU patterns (vector load/store) ==========
  {"vec_copy",
   "typedef long long v2q __attribute__((vector_size(16)));\n"
   "long vec_copy(long a) {\n"
   "  v2q va = {a, 0};\n"
   "  v2q vr = va;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0xDEADBEEFCAFEBABEULL}, "PackedFP", 1},

  // ========== PUNPCKLQDQ (interleave 64-bit) ==========
  {"punpcklqdq",
   "typedef long long v2q __attribute__((vector_size(16)));\n"
   "long punpcklqdq(long a, long b) {\n"
   "  v2q va = {a, 0};\n"
   "  v2q vb = {b, 0};\n"
   "  v2q vr = __builtin_shufflevector(va, vb, 0, 2);\n"
   "  return (long)(vr[0] ^ vr[1]);\n"
   "}\n",
   {0xDEADBEEFULL, 0xCAFEBABEULL}, "PackedFP", 1},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(PackedFP, X64PackedFPRT,
                         ::testing::ValuesIn(kX64PackedFP), rtTCName);
