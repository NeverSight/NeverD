//===- EdgeCaseNEONTests.cpp - AArch64/ARM32 NEON edge cases -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests for NEON patterns that previously caused issues:
// - 8B/4H/2S arrangements (D-register operations + Q zero-extend)
// - BFM/BFXIL with ImmR=0
// - INS d[1], d[0] (self-referencing lane copy)
// - Vector accumulation loops
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64NeonEdgeRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64NeonEdgeRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32NeonEdgeRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32NeonEdgeRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kA64NeonEdge = {
  // 8B arrangements (D-register operations)
  {"a64_add_8b",
   "typedef signed char v8b __attribute__((vector_size(8)));\n"
   "long a64_add_8b(long a, long b) {\n"
   "  v8b va, vb;\n"
   "  __builtin_memcpy(&va, &a, 8);\n"
   "  __builtin_memcpy(&vb, &b, 8);\n"
   "  v8b vc = va + vb;\n"
   "  long r; __builtin_memcpy(&r, &vc, 8);\n"
   "  return r;\n"
   "}\n",
   {0x0102030405060708ULL, 0x0102030405060708ULL}, "NeonEdge", 1},

  {"a64_sub_8b",
   "typedef signed char v8b __attribute__((vector_size(8)));\n"
   "long a64_sub_8b(long a, long b) {\n"
   "  v8b va, vb;\n"
   "  __builtin_memcpy(&va, &a, 8);\n"
   "  __builtin_memcpy(&vb, &b, 8);\n"
   "  v8b vc = va - vb;\n"
   "  long r; __builtin_memcpy(&r, &vc, 8);\n"
   "  return r;\n"
   "}\n",
   {0x0A0B0C0D0E0F1011ULL, 0x0102030405060708ULL}, "NeonEdge", 1},

  // 4H arrangements
  {"a64_add_4h",
   "typedef short v4h __attribute__((vector_size(8)));\n"
   "long a64_add_4h(long a, long b) {\n"
   "  v4h va, vb;\n"
   "  __builtin_memcpy(&va, &a, 8);\n"
   "  __builtin_memcpy(&vb, &b, 8);\n"
   "  v4h vc = va + vb;\n"
   "  long r; __builtin_memcpy(&r, &vc, 8);\n"
   "  return r;\n"
   "}\n",
   {0x0001000200030004ULL, 0x0010002000300040ULL}, "NeonEdge", 1},

  // 2S arrangements
  {"a64_add_2s",
   "typedef int v2s __attribute__((vector_size(8)));\n"
   "long a64_add_2s(long a, long b) {\n"
   "  v2s va, vb;\n"
   "  __builtin_memcpy(&va, &a, 8);\n"
   "  __builtin_memcpy(&vb, &b, 8);\n"
   "  v2s vc = va + vb;\n"
   "  long r; __builtin_memcpy(&r, &vc, 8);\n"
   "  return r;\n"
   "}\n",
   {0x0000000100000002ULL, 0x0000001000000020ULL}, "NeonEdge", 1},

  // BFXIL with various ImmR values
  {"a64_bfxil_0",
   "long a64_bfxil_0(long a, long b) {\n"
   "  return (a & ~0xFULL) | (b & 0xFULL);\n"
   "}\n",
   {0xDEADBEEF12345670ULL, 0x000000000000000AULL}, "NeonEdge"},

  {"a64_bfi_insert",
   "long a64_bfi(long a, long b) {\n"
   "  return (a & ~(0xFFULL << 8)) | ((b & 0xFFULL) << 8);\n"
   "}\n",
   {0x1234567890ABCDEFULL, 0x42}, "NeonEdge"},

  // Vector reduce with loop (exercises Q register store/load patterns)
  {"a64_vec_sum_4s",
   "typedef int v4s __attribute__((vector_size(16)));\n"
   "long a64_vec_sum(long a, long b) {\n"
   "  v4s v = {(int)a, (int)(a>>16), (int)b, (int)(b>>16)};\n"
   "  return (long)(v[0] + v[1] + v[2] + v[3]);\n"
   "}\n",
   {0x0001000200030004ULL, 0x0005000600070008ULL}, "NeonEdge", 1},

  // Multi-register NEON operations
  {"a64_neon_mul_acc",
   "typedef int v4s __attribute__((vector_size(16)));\n"
   "long a64_neon_mul_acc(long a, long b) {\n"
   "  v4s va = {(int)a, (int)(a>>16), (int)(a>>32), (int)(a>>48)};\n"
   "  v4s vb = {(int)b, (int)(b>>16), (int)(b>>32), (int)(b>>48)};\n"
   "  v4s vc = va * vb;\n"
   "  return (long)(vc[0] + vc[1] + vc[2] + vc[3]);\n"
   "}\n",
   {0x0001000200030004ULL, 0x0002000300040005ULL}, "NeonEdge", 1},

  // Widening operations
  {"a64_saddl",
   "typedef short v4h __attribute__((vector_size(8)));\n"
   "typedef int v4s __attribute__((vector_size(16)));\n"
   "long a64_saddl(long a, long b) {\n"
   "  v4h va, vb;\n"
   "  __builtin_memcpy(&va, &a, 8);\n"
   "  __builtin_memcpy(&vb, &b, 8);\n"
   "  v4s vc = __builtin_convertvector(va, v4s) + __builtin_convertvector(vb, v4s);\n"
   "  return (long)(vc[0] + vc[1]);\n"
   "}\n",
   {0x7FFF800000010002ULL, 0x0001FFFF00030004ULL}, "NeonEdge", 1},

  // NEON compare
  {"a64_cmpeq_4s",
   "typedef int v4s __attribute__((vector_size(16)));\n"
   "long a64_cmpeq(long a) {\n"
   "  v4s va = {(int)a, (int)(a>>16), 0, (int)a};\n"
   "  v4s vb = {(int)a, 0, 0, (int)a};\n"
   "  v4s vc = (va == vb);\n"
   "  return (long)(vc[0] & vc[2] & vc[3]);\n"
   "}\n",
   {42}, "NeonEdge", 1},
};

static const std::vector<RoundTripTC> kARM32NeonEdge = {
  // ARM32 NEON lane insert (vmov.8) requires per-element handling.
  // These tests exercise ARM32 vector patterns via scalar C code.
  {"arm_vec_sum",
   "int arm_vec_sum(int a, int b) {\n"
   "  int s = 0;\n"
   "  s += (signed char)(a) + (signed char)(b);\n"
   "  s += (signed char)(a>>8) + (signed char)(b>>8);\n"
   "  s += (signed char)(a>>16) + (signed char)(b>>16);\n"
   "  s += (signed char)(a>>24) + (signed char)(b>>24);\n"
   "  return s;\n"
   "}\n",
   {0x01020304, 0x05060708}, "NeonEdge"},

  {"arm_mul_acc",
   "int arm_mul_acc(int a, int b) {\n"
   "  int s = 0;\n"
   "  s += (short)(a) * (short)(b);\n"
   "  s += (short)(a>>16) * (short)(b>>16);\n"
   "  return s;\n"
   "}\n",
   {0x00030005, 0x00020004}, "NeonEdge"},

  {"arm_vadd_f32",
   "int arm_vadd_f32(int a, int b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4);\n"
   "  __builtin_memcpy(&fb, &b, 4);\n"
   "  float fc = fa + fb;\n"
   "  int r;\n"
   "  __builtin_memcpy(&r, &fc, 4);\n"
   "  return r;\n"
   "}\n",
   {0x40A00000, 0x40400000}, "NeonEdge", 1, "-mfpu=vfpv3"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(NeonEdge, A64NeonEdgeRT,
                         ::testing::ValuesIn(kA64NeonEdge), rtTCName);
INSTANTIATE_TEST_SUITE_P(NeonEdge, ARM32NeonEdgeRT,
                         ::testing::ValuesIn(kARM32NeonEdge), rtTCName);
