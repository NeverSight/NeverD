//===- AArch64_MassiveTests.cpp - Mass coverage AArch64 tests --*- C++ -*-===//
//
// High-volume AArch64 roundtrip tests covering multiply forms, shift
// varieties, conditional select, extension chains, memory patterns,
// complex ALU, and branch-heavy patterns.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64MassRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64MassRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kA64Mass = {
  // --- Multiply forms ---
  {"a64_imul_imm", "long a64_imul_imm(long a) { return a * 37; }\n", {42}, "A64Mass"},
  {"a64_imul_2op", "long a64_imul_2op(long a, long b) { return a * b; }\n", {13, 17}, "A64Mass"},
  {"a64_madd2", "long a64_madd2(long a, long b, long c) { return a*b+c; }\n", {5, 7, 3}, "A64Mass"},
  {"a64_msub2", "long a64_msub2(long a, long b, long c) { return c-a*b; }\n", {5, 7, 100}, "A64Mass"},
  {"a64_smull", "long a64_smull(long a, long b) { return (long)((int)a * (int)b); }\n", {(uint64_t)(uint32_t)-7, 11}, "A64Mass"},
  {"a64_umull_hi",
   "long a64_umull_hi(long a, long b) {\n"
   "  return (long)(((unsigned __int128)(unsigned long)a * (unsigned long)b) >> 64);\n"
   "}\n",
   {0x1234567890ABCDEFULL, 0xFEDCBA0987654321ULL}, "A64Mass"},

  // --- Shift varieties ---
  {"a64_lsl1", "long a64_lsl1(long a) { return a << 1; }\n", {42}, "A64Mass"},
  {"a64_lsl_cl", "long a64_lsl_cl(long a, long b) { return a << (b & 63); }\n", {42, 4}, "A64Mass"},
  {"a64_lsr1", "long a64_lsr1(long a) { return (unsigned long)a >> 1; }\n", {0x8000000000000000ULL}, "A64Mass"},
  {"a64_asr_cl", "long a64_asr_cl(long a, long b) { return a >> (b & 63); }\n", {0xFFFFFFFFFFFFFC00ULL, 3}, "A64Mass"},
  {"a64_lsl32", "long a64_lsl32(long a, long b) { return (long)((unsigned int)a << ((int)b & 31)); }\n", {1, 20}, "A64Mass"},

  // --- Comparison + conditional ---
  {"a64_eq", "long a64_eq(long a, long b) { return a == b; }\n", {42, 42}, "A64Mass"},
  {"a64_ne", "long a64_ne(long a, long b) { return a != b; }\n", {42, 43}, "A64Mass"},
  {"a64_slt", "long a64_slt(long a, long b) { return a < b; }\n", {0xFFFFFFFFFFFFFFFFULL, 1}, "A64Mass"},
  {"a64_uge", "long a64_uge(long a, long b) { return (unsigned long)a >= (unsigned long)b; }\n", {100, 50}, "A64Mass"},
  {"a64_min_u", "long a64_min_u(long a, long b) { unsigned long ua=a, ub=b; return ua < ub ? a : b; }\n", {100, 50}, "A64Mass"},

  // --- Conversion chains ---
  {"a64_cvt_i8", "long a64_cvt_i8(long a) { return (long)(signed char)a; }\n", {0x80}, "A64Mass"},
  {"a64_cvt_u8", "long a64_cvt_u8(long a) { return (long)(unsigned char)a; }\n", {0xFF}, "A64Mass"},
  {"a64_cvt_i16", "long a64_cvt_i16(long a) { return (long)(short)a; }\n", {0x8000}, "A64Mass"},
  {"a64_cvt_u32", "long a64_cvt_u32(long a) { return (long)(unsigned int)a; }\n", {0xFFFFFFFFULL}, "A64Mass"},
  {"a64_cvt_chain",
   "long a64_cvt_chain(long a) {\n"
   "  unsigned char b = (unsigned char)a;\n"
   "  short c = (signed char)b;\n"
   "  int d = c;\n"
   "  return d;\n"
   "}\n",
   {0x80}, "A64Mass"},

  // --- Memory patterns ---
  {"a64_volatile", "long a64_volatile(long a) { volatile long v = a; return v; }\n", {42}, "A64Mass"},
  {"a64_vol32", "long a64_vol32(long a) { volatile int v = (int)a; return v; }\n", {0x12345678}, "A64Mass"},

  // --- ALU chains ---
  {"a64_horner",
   "long a64_horner(long x) {\n"
   "  return ((((x+1)*x+2)*x+3)*x+4)*x+5;\n"
   "}\n",
   {3}, "A64Mass"},

  {"a64_sum_digits",
   "long a64_sum_digits(long n) {\n"
   "  long s = 0;\n"
   "  unsigned long u = (unsigned long)(n > 0 ? n : -n);\n"
   "  while (u > 0) { s += u % 10; u /= 10; }\n"
   "  return s;\n"
   "}\n",
   {123456789}, "A64Mass"},

  {"a64_fib_pair",
   "long a64_fib_pair(long n) {\n"
   "  long a=0, b=1;\n"
   "  for (long i=0; i<n; ++i) { long t=a+b; a=b; b=t; }\n"
   "  return a*1000+b;\n"
   "}\n",
   {10}, "A64Mass"},

  {"a64_count_pairs",
   "long a64_count_pairs(long a) {\n"
   "  unsigned long x = (unsigned long)a;\n"
   "  long count = 0;\n"
   "  while (x) { if ((x & 3) == 3) ++count; x >>= 1; }\n"
   "  return count;\n"
   "}\n",
   {0xABCDEF01ULL}, "A64Mass"},

  // --- Branch-heavy ---
  {"a64_classify5",
   "long a64_classify5(long a) {\n"
   "  if (a < -100) return -3;\n"
   "  if (a < -10) return -2;\n"
   "  if (a < 0) return -1;\n"
   "  if (a == 0) return 0;\n"
   "  if (a < 10) return 1;\n"
   "  if (a < 100) return 2;\n"
   "  return 3;\n"
   "}\n",
   {42}, "A64Mass"},

  {"a64_bsearch",
   "long a64_bsearch(long lo, long hi) {\n"
   "  long target = 50;\n"
   "  while (lo <= hi) {\n"
   "    long mid = lo + (hi - lo) / 2;\n"
   "    if (mid == target) return mid;\n"
   "    if (mid < target) lo = mid + 1;\n"
   "    else hi = mid - 1;\n"
   "  }\n"
   "  return -1;\n"
   "}\n",
   {1, 100}, "A64Mass"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(A64Mass, AArch64MassRT,
                         ::testing::ValuesIn(kA64Mass), rtTCName);
