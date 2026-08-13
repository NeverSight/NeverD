//===- X64_MassiveTests.cpp - Mass coverage x86_64 tests ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// High-volume x86_64 roundtrip tests. Each test exercises a specific
// instruction pattern or combination. Grouped by instruction class.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64MassRT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64MassRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kX64Mass = {
  // ============================================================
  // Group 1: Integer multiply forms
  // ============================================================
  {"imul_reg_imm", "long imul_reg_imm(long a) { return a * 37; }\n", {42}, "Mass"},
  {"imul_2op", "long imul_2op(long a, long b) { return a * b; }\n", {13, 17}, "Mass"},
  {"imul_3op", "long imul_3op(long a) { return a * 127; }\n", {100}, "Mass"},
  {"imul32", "long imul32(long a, long b) { return (long)((int)a * (int)b); }\n", {(uint64_t)(uint32_t)-7, 11}, "Mass"},
  {"umul_hi",
   "long umul_hi(long a, long b) {\n"
   "  return (long)(((unsigned __int128)(unsigned long)a * (unsigned long)b) >> 64);\n"
   "}\n",
   {0x1234567890ABCDEFULL, 0xFEDCBA0987654321ULL}, "Mass"},

  // ============================================================
  // Group 2: Shift varieties
  // ============================================================
  {"shl_by_1", "long shl_by_1(long a) { return a << 1; }\n", {42}, "Mass"},
  {"shl_by_cl", "long shl_by_cl(long a, long b) { return a << (b & 63); }\n", {42, 4}, "Mass"},
  {"shr_by_1", "long shr_by_1(long a) { return (unsigned long)a >> 1; }\n", {0x8000000000000000ULL}, "Mass"},
  {"sar_by_cl", "long sar_by_cl(long a, long b) { return a >> (b & 63); }\n", {0xFFFFFFFFFFFFFC00ULL, 3}, "Mass"},
  {"shl32_by_cl", "long shl32_by_cl(long a, long b) { return (long)((unsigned int)a << ((int)b & 31)); }\n", {1, 20}, "Mass"},

  // ============================================================
  // Group 3: Comparison + conditional
  // ============================================================
  {"setcc_eq", "long setcc_eq(long a, long b) { return a == b; }\n", {42, 42}, "Mass"},
  {"setcc_ne", "long setcc_ne(long a, long b) { return a != b; }\n", {42, 43}, "Mass"},
  {"setcc_lt", "long setcc_lt(long a, long b) { return a < b; }\n", {0xFFFFFFFFFFFFFFFFULL, 1}, "Mass"},
  {"setcc_ge", "long setcc_ge(long a, long b) { return a >= b; }\n", {5, 5}, "Mass"},
  {"setcc_below", "long setcc_below(long a, long b) { return (unsigned long)a < (unsigned long)b; }\n", {5, 100}, "Mass"},
  {"setcc_above", "long setcc_above(long a, long b) { return (unsigned long)a > (unsigned long)b; }\n", {100, 5}, "Mass"},
  {"cmov_min_u", "long cmov_min_u(long a, long b) { unsigned long ua = a, ub = b; return ua < ub ? a : b; }\n", {100, 50}, "Mass"},

  // ============================================================
  // Group 4: LEA addressing forms
  // ============================================================
  {"lea_base_disp", "long lea_bd(long a) { return a + 100; }\n", {42}, "Mass"},
  {"lea_base_idx", "long lea_bi(long a, long b) { return a + b; }\n", {42, 17}, "Mass"},
  {"lea_base_idx_s2", "long lea_bis2(long a, long b) { return a + b * 2; }\n", {10, 20}, "Mass"},
  {"lea_base_idx_s4", "long lea_bis4(long a, long b) { return a + b * 4; }\n", {10, 20}, "Mass"},
  {"lea_base_idx_s8", "long lea_bis8(long a, long b) { return a + b * 8; }\n", {10, 20}, "Mass"},
  {"lea_idx_s4_disp", "long lea_is4d(long a) { return a * 4 + 7; }\n", {10}, "Mass"},

  // ============================================================
  // Group 5: Conversion chains
  // ============================================================
  {"cvt_i8_to_i64", "long cvt_i8(long a) { return (long)(signed char)a; }\n", {0x80}, "Mass"},
  {"cvt_u8_to_i64", "long cvt_u8(long a) { return (long)(unsigned char)a; }\n", {0xFF}, "Mass"},
  {"cvt_i16_to_i64", "long cvt_i16(long a) { return (long)(short)a; }\n", {0x8000}, "Mass"},
  {"cvt_u16_to_i64", "long cvt_u16(long a) { return (long)(unsigned short)a; }\n", {0xFFFF}, "Mass"},
  {"cvt_i32_to_i64", "long cvt_i32(long a) { return (long)(int)a; }\n", {0x80000000ULL}, "Mass"},
  {"cvt_u32_to_i64", "long cvt_u32(long a) { return (long)(unsigned int)a; }\n", {0xFFFFFFFFULL}, "Mass"},
  {"cvt_chain_8_16_32_64",
   "long cvt_chain(long a) {\n"
   "  unsigned char b = (unsigned char)a;\n"
   "  short c = (signed char)b;\n"
   "  int d = c;\n"
   "  return d;\n"
   "}\n",
   {0x80}, "Mass"},

  // ============================================================
  // Group 6: Memory store/load patterns
  // ============================================================
  {"mem_store_load", "long mem_sl(long a) { volatile long v = a; return v; }\n", {42}, "Mass"},
  {"mem_store_load32", "long mem_sl32(long a) { volatile int v = (int)a; return v; }\n", {0x12345678}, "Mass"},
  {"mem_store_load16", "long mem_sl16(long a) { volatile short v = (short)a; return v; }\n", {0x1234}, "Mass"},
  {"mem_store_load8", "long mem_sl8(long a) { volatile signed char v = (signed char)a; return v; }\n", {0x42}, "Mass"},

  // ============================================================
  // Group 7: Complex ALU chains
  // ============================================================
  {"alu_horner",
   "long alu_horner(long x) {\n"
   "  return ((((x + 1) * x + 2) * x + 3) * x + 4) * x + 5;\n"
   "}\n",
   {3}, "Mass"},

  {"alu_euclidean",
   "long alu_euclidean(long a, long b) {\n"
   "  unsigned long x = (unsigned long)(a > 0 ? a : -a);\n"
   "  unsigned long y = (unsigned long)(b > 0 ? b : -b);\n"
   "  while (y) { unsigned long t = y; y = x % y; x = t; }\n"
   "  return (long)x;\n"
   "}\n",
   {252, 105}, "Mass"},

  {"alu_fibonacci_pair",
   "long alu_fibonacci_pair(long n) {\n"
   "  long a = 0, b = 1;\n"
   "  for (long i = 0; i < n; ++i) { long t = a + b; a = b; b = t; }\n"
   "  return a * 1000 + b;\n"
   "}\n",
   {10}, "Mass"},

  {"alu_sum_of_digits",
   "long alu_sum_of_digits(long n) {\n"
   "  long s = 0;\n"
   "  unsigned long u = (unsigned long)(n > 0 ? n : -n);\n"
   "  while (u > 0) { s += u % 10; u /= 10; }\n"
   "  return s;\n"
   "}\n",
   {123456789}, "Mass"},

  {"alu_count_set_bits_pairs",
   "long alu_count_pairs(long a) {\n"
   "  unsigned long x = (unsigned long)a;\n"
   "  long count = 0;\n"
   "  while (x) {\n"
   "    if ((x & 3) == 3) ++count;\n"
   "    x >>= 1;\n"
   "  }\n"
   "  return count;\n"
   "}\n",
   {0xABCDEF01ULL}, "Mass"},

  // ============================================================
  // Group 8: Branch-heavy patterns
  // ============================================================
  {"branch_classify5",
   "long branch_classify5(long a) {\n"
   "  if (a < -100) return -3;\n"
   "  if (a < -10) return -2;\n"
   "  if (a < 0) return -1;\n"
   "  if (a == 0) return 0;\n"
   "  if (a < 10) return 1;\n"
   "  if (a < 100) return 2;\n"
   "  return 3;\n"
   "}\n",
   {42}, "Mass"},

  {"branch_binary_search",
   "long branch_binary_search(long target) {\n"
   "  long arr_vals[8];\n"
   "  for (int i = 0; i < 8; ++i) arr_vals[i] = i * 10;\n"
   "  long lo = 0, hi = 7;\n"
   "  while (lo <= hi) {\n"
   "    long mid = lo + (hi - lo) / 2;\n"
   "    if (arr_vals[mid] == target) return mid;\n"
   "    if (arr_vals[mid] < target) lo = mid + 1;\n"
   "    else hi = mid - 1;\n"
   "  }\n"
   "  return -1;\n"
   "}\n",
   {30}, "Mass"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(Mass, X64MassRT,
                         ::testing::ValuesIn(kX64Mass), rtTCName);
