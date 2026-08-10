//===- X64_FPCompareRTTests.cpp - FP comparison roundtrip ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests x86_64 FP comparison patterns that exercise UCOMISD/COMISS flag chains.
// These are specifically designed to catch flag folding bugs like #32
// (TESTB+CMOVNE producing NE(a,b) instead of (a&b)!=0).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64FPCmpRT : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64FPCmpRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

// 42.0 = 0x4045000000000000
// 5.0  = 0x4014000000000000
// 3.0  = 0x4008000000000000
// -42.0 = 0xC045000000000000
// 0.0  = 0x0000000000000000
// 42.0f = 0x42280000
// 5.0f  = 0x40A00000
// 3.0f  = 0x40400000

static const std::vector<RoundTripTC> kX64FPCmp = {
  // ========== Double equality (UCOMISD + SETE + SETNP + TEST + CMOVNE) ==========
  {"fpcmp_d_eq_true",
   "long fpcmp_d_eq_true(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  return da == db ? 1 : 0;\n"
   "}\n",
   {0x4045000000000000ULL, 0x4045000000000000ULL}, "FPCmpRT"},

  {"fpcmp_d_eq_false",
   "long fpcmp_d_eq_false(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  return da == db ? 1 : 0;\n"
   "}\n",
   {0x4045000000000000ULL, 0x4014000000000000ULL}, "FPCmpRT"},

  // ========== Double not-equal ==========
  {"fpcmp_d_ne_true",
   "long fpcmp_d_ne_true(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  return da != db ? 1 : 0;\n"
   "}\n",
   {0x4045000000000000ULL, 0x4014000000000000ULL}, "FPCmpRT"},

  {"fpcmp_d_ne_false",
   "long fpcmp_d_ne_false(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  return da != db ? 1 : 0;\n"
   "}\n",
   {0x4045000000000000ULL, 0x4045000000000000ULL}, "FPCmpRT"},

  // ========== Double less-than ==========
  {"fpcmp_d_lt_true",
   "long fpcmp_d_lt_true(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  return da < db ? 1 : 0;\n"
   "}\n",
   {0x4008000000000000ULL, 0x4014000000000000ULL}, "FPCmpRT"},

  {"fpcmp_d_lt_false",
   "long fpcmp_d_lt_false(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  return da < db ? 1 : 0;\n"
   "}\n",
   {0x4014000000000000ULL, 0x4008000000000000ULL}, "FPCmpRT"},

  // ========== Double greater-than ==========
  {"fpcmp_d_gt_true",
   "long fpcmp_d_gt_true(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  return da > db ? 1 : 0;\n"
   "}\n",
   {0x4014000000000000ULL, 0x4008000000000000ULL}, "FPCmpRT"},

  {"fpcmp_d_gt_false",
   "long fpcmp_d_gt_false(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  return da > db ? 1 : 0;\n"
   "}\n",
   {0x4008000000000000ULL, 0x4014000000000000ULL}, "FPCmpRT"},

  // ========== Double less-or-equal ==========
  {"fpcmp_d_le_true",
   "long fpcmp_d_le_true(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  return da <= db ? 1 : 0;\n"
   "}\n",
   {0x4045000000000000ULL, 0x4045000000000000ULL}, "FPCmpRT"},

  {"fpcmp_d_le_equal",
   "long fpcmp_d_le_equal(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  return da <= db ? 1 : 0;\n"
   "}\n",
   {0x4008000000000000ULL, 0x4014000000000000ULL}, "FPCmpRT"},

  // ========== Double greater-or-equal ==========
  {"fpcmp_d_ge_true",
   "long fpcmp_d_ge_true(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  return da >= db ? 1 : 0;\n"
   "}\n",
   {0x4045000000000000ULL, 0x4045000000000000ULL}, "FPCmpRT"},

  {"fpcmp_d_ge_gt",
   "long fpcmp_d_ge_gt(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  return da >= db ? 1 : 0;\n"
   "}\n",
   {0x4014000000000000ULL, 0x4008000000000000ULL}, "FPCmpRT"},

  // ========== Negative value comparisons ==========
  {"fpcmp_d_neg_lt",
   "long fpcmp_d_neg_lt(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  return da < db ? 1 : 0;\n"
   "}\n",
   {0xC045000000000000ULL, 0x4045000000000000ULL}, "FPCmpRT"},  // -42 < 42

  // ========== FP compare with branch (generates JA/JB instead of CMOV) ==========
  {"fpcmp_d_select_gt",
   "long fpcmp_d_select_gt(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  double r = da > db ? da : db;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4014000000000000ULL, 0x4008000000000000ULL}, "FPCmpRT"},

  {"fpcmp_d_select_lt",
   "long fpcmp_d_select_lt(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  double r = da < db ? da : db;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4014000000000000ULL, 0x4008000000000000ULL}, "FPCmpRT"},

  // ========== Integer comparison with TEST pattern ==========
  {"icmp_test_and_eq",
   "long icmp_test_and_eq(long a, long b) {\n"
   "  return (a & b) != 0 ? 1 : 0;\n"
   "}\n",
   {0xFF, 0x0F}, "FPCmpRT"},

  {"icmp_test_and_zero",
   "long icmp_test_and_zero(long a, long b) {\n"
   "  return (a & b) != 0 ? 1 : 0;\n"
   "}\n",
   {0xF0, 0x0F}, "FPCmpRT"},

  {"icmp_test_self",
   "long icmp_test_self(long a) {\n"
   "  return a != 0 ? 42 : 0;\n"
   "}\n",
   {100}, "FPCmpRT"},

  {"icmp_test_self_zero",
   "long icmp_test_self_zero(long a) {\n"
   "  return a != 0 ? 42 : 0;\n"
   "}\n",
   {0}, "FPCmpRT"},

  // ========== Chained comparisons ==========
  {"fpcmp_chain",
   "long fpcmp_chain(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  if (da > db) return 1;\n"
   "  if (da < db) return -1;\n"
   "  return 0;\n"
   "}\n",
   {0x4045000000000000ULL, 0x4045000000000000ULL}, "FPCmpRT"},

  {"fpcmp_chain_gt",
   "long fpcmp_chain_gt(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  if (da > db) return 1;\n"
   "  if (da < db) return -1;\n"
   "  return 0;\n"
   "}\n",
   {0x4014000000000000ULL, 0x4008000000000000ULL}, "FPCmpRT"},

  // ========== Float (32-bit) comparison ==========
  {"fpcmp_f_eq",
   "long fpcmp_f_eq(long a, long b) {\n"
   "  int ai = (int)a, bi = (int)b;\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &ai, 4); __builtin_memcpy(&fb, &bi, 4);\n"
   "  return fa == fb ? 1 : 0;\n"
   "}\n",
   {0x42280000ULL, 0x42280000ULL}, "FPCmpRT"},  // 42.0f == 42.0f

  {"fpcmp_f_lt",
   "long fpcmp_f_lt(long a, long b) {\n"
   "  int ai = (int)a, bi = (int)b;\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &ai, 4); __builtin_memcpy(&fb, &bi, 4);\n"
   "  return fa < fb ? 1 : 0;\n"
   "}\n",
   {0x40400000ULL, 0x40A00000ULL}, "FPCmpRT"},  // 3.0f < 5.0f
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(FPCmpRT, X64FPCmpRT,
                         ::testing::ValuesIn(kX64FPCmp), rtTCName);
