//===- AArch64_NEONExtRTTests.cpp - AArch64 NEON ext roundtrip ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests AArch64 NEON MUL/MLA/FMLA/widening/narrowing/pairwise operations
// through the full lift → recompile → Unicorn pipeline.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64NEONExtRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64NEONExtRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

// ============================================================================
// NEON MUL/MLA vector operations
// ============================================================================
static const std::vector<RoundTripTC> kA64NEONMul = {
  {"neon_mul_v4s",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long neon_mul_v4s(long a, long b) {\n"
   "  v4i va = {(int)a, 1, 2, 3};\n"
   "  v4i vb = {(int)b, 4, 5, 6};\n"
   "  v4i vr = va * vb;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {7, 8}, "NEONMul", 1},

  {"neon_mla_v4s",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long neon_mla_v4s(long a, long b) {\n"
   "  v4i va = {(int)a, 1, 0, 0};\n"
   "  v4i vb = {(int)b, 2, 0, 0};\n"
   "  v4i vc = {10, 20, 0, 0};\n"
   "  v4i vr = va * vb + vc;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {5, 3}, "NEONMul", 1},

  {"neon_mul_v8h",
   "typedef short v8s __attribute__((vector_size(16)));\n"
   "long neon_mul_v8h(long a, long b) {\n"
   "  v8s va = {(short)a, 0, 0, 0, 0, 0, 0, 0};\n"
   "  v8s vb = {(short)b, 0, 0, 0, 0, 0, 0, 0};\n"
   "  v8s vr = va * vb;\n"
   "  return (long)(short)vr[0];\n"
   "}\n",
   {100, 200}, "NEONMul", 1},
};

// ============================================================================
// NEON FMLA/FMLS (fused multiply-add/sub for vectors)
// ============================================================================
static const std::vector<RoundTripTC> kA64NEONFma = {
  {"neon_muladd_int",
   "long neon_muladd_int(long a, long b) {\n"
   "  return a * b + a;\n"
   "}\n",
   {5, 3}, "NEONFma", 1},

  {"neon_mulsub_int",
   "long neon_mulsub_int(long a, long b) {\n"
   "  return a * b - b;\n"
   "}\n",
   {7, 3}, "NEONFma", 1},
};

// ============================================================================
// NEON widening arithmetic: SADDL, SMULL, UADDL, etc.
// ============================================================================
static const std::vector<RoundTripTC> kA64NEONWidening = {
  {"neon_smull_i32_to_i64",
   "long neon_smull_i32_to_i64(long a, long b) {\n"
   "  int ia = (int)a, ib = (int)b;\n"
   "  return (long)ia * (long)ib;\n"
   "}\n",
   {100000, (uint64_t)(int64_t)-200000}, "NEONWiden", 1},

  {"neon_umull_u32_to_u64",
   "long neon_umull_u32_to_u64(long a, long b) {\n"
   "  unsigned int ua = (unsigned int)a, ub = (unsigned int)b;\n"
   "  return (long)((unsigned long)ua * (unsigned long)ub);\n"
   "}\n",
   {0xFFFFFFFFULL, 2}, "NEONWiden", 1},
};

// ============================================================================
// NEON pairwise operations: ADDP, SMINP, SMAXP, FADDP
// ============================================================================
static const std::vector<RoundTripTC> kA64NEONPairwise = {
  {"neon_hadd_scalar",
   "long neon_hadd_scalar(long a, long b) {\n"
   "  int lo = (int)a, hi = (int)(a >> 32);\n"
   "  return (long)(lo + hi);\n"
   "}\n",
   {(3ULL | (7ULL << 32)), 0}, "NEONPairwise", 1},
};

// ============================================================================
// NEON comparison: CMEQ, CMGT, CMGE, CMHI
// ============================================================================
static const std::vector<RoundTripTC> kA64NEONCmp = {
  {"neon_cmeq_v4s",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long neon_cmeq_v4s(long a, long b) {\n"
   "  v4i va = {(int)a, 2, 3, 4};\n"
   "  v4i vb = {(int)b, 2, 5, 4};\n"
   "  v4i vr = (va == vb);\n"
   "  return (long)(unsigned)vr[0] | ((unsigned long)(unsigned)vr[1] << 32);\n"
   "}\n",
   {42, 42}, "NEONCmp", 1},

  {"neon_cmgt_v4s",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long neon_cmgt_v4s(long a, long b) {\n"
   "  v4i va = {(int)a, 10, 3, 4};\n"
   "  v4i vb = {(int)b, 5, 5, 4};\n"
   "  v4i vr = (va > vb);\n"
   "  return (long)(unsigned)vr[0] | ((unsigned long)(unsigned)vr[1] << 32);\n"
   "}\n",
   {100, 50}, "NEONCmp", 1},
};

// ============================================================================
// NEON bit manipulation: BIC, ORN, BSL, BIT, BIF
// ============================================================================
static const std::vector<RoundTripTC> kA64NEONBit = {
  {"neon_bic_v2d",
   "typedef long long v2q __attribute__((vector_size(16)));\n"
   "long neon_bic_v2d(long a, long b) {\n"
   "  v2q va = {a, 0};\n"
   "  v2q vb = {b, 0};\n"
   "  v2q vr = va & ~vb;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0xFFFF0000FFFF0000ULL, 0xFF00FF00FF00FF00ULL}, "NEONBit", 1},

  {"neon_orn_v2d",
   "typedef long long v2q __attribute__((vector_size(16)));\n"
   "long neon_orn_v2d(long a, long b) {\n"
   "  v2q va = {a, 0};\n"
   "  v2q vb = {b, 0};\n"
   "  v2q vr = va | ~vb;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0x0F0F0F0F0F0F0F0FULL, 0xF0F0F0F0F0F0F0F0ULL}, "NEONBit", 1},
};

// ============================================================================
// AArch64 sub-register edge cases
// ============================================================================
static const std::vector<RoundTripTC> kA64SubregEdge = {
  {"a64_sxtb_chain",
   "long a64_sxtb_chain(long a, long b) {\n"
   "  signed char s8 = (signed char)a;\n"
   "  short s16 = (short)s8;\n"
   "  int s32 = (int)s16;\n"
   "  return (long)s32;\n"
   "}\n",
   {0x80, 0}, "A64Subreg", 1},

  {"a64_uxtb_chain",
   "long a64_uxtb_chain(long a, long b) {\n"
   "  unsigned char u8 = (unsigned char)a;\n"
   "  unsigned short u16 = (unsigned short)u8;\n"
   "  unsigned int u32 = (unsigned int)u16;\n"
   "  return (long)u32;\n"
   "}\n",
   {0x1FF, 0}, "A64Subreg", 1},

  {"a64_w_to_x_zext",
   "long a64_w_to_x_zext(long a, long b) {\n"
   "  unsigned int w = (unsigned int)a;\n"
   "  w = w + (unsigned int)b;\n"
   "  return (long)w;\n"
   "}\n",
   {0xFFFFFFFFULL, 2}, "A64Subreg", 1},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(NEONMul, A64NEONExtRT, ::testing::ValuesIn(kA64NEONMul), rtTCName);
INSTANTIATE_TEST_SUITE_P(NEONFma, A64NEONExtRT, ::testing::ValuesIn(kA64NEONFma), rtTCName);
INSTANTIATE_TEST_SUITE_P(NEONWiden, A64NEONExtRT, ::testing::ValuesIn(kA64NEONWidening), rtTCName);
INSTANTIATE_TEST_SUITE_P(NEONPairwise, A64NEONExtRT, ::testing::ValuesIn(kA64NEONPairwise), rtTCName);
INSTANTIATE_TEST_SUITE_P(NEONCmp, A64NEONExtRT, ::testing::ValuesIn(kA64NEONCmp), rtTCName);
INSTANTIATE_TEST_SUITE_P(NEONBit, A64NEONExtRT, ::testing::ValuesIn(kA64NEONBit), rtTCName);
INSTANTIATE_TEST_SUITE_P(A64Subreg, A64NEONExtRT, ::testing::ValuesIn(kA64SubregEdge), rtTCName);
