//===- ARM32_MassiveTests.cpp - Mass coverage ARM32 tests ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// High-volume ARM32 roundtrip tests covering multiply, shift, conditional,
// extension, memory, ALU chains, and branch patterns.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32MassRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32MassRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kARM32Mass = {
  // --- Multiply forms ---
  {"arm_mul_imm", "int arm_mul_imm(int a) { return a * 37; }\n", {42}, "ARMMass"},
  {"arm_mul_2op", "int arm_mul_2op(int a, int b) { return a * b; }\n", {13, 17}, "ARMMass"},
  {"arm_mla2", "int arm_mla2(int a, int b, int c) { return a*b+c; }\n", {5, 7, 3}, "ARMMass"},
  {"arm_mls2", "int arm_mls2(int a, int b, int c) { return c-a*b; }\n", {5, 7, 100}, "ARMMass"},
  {"arm_smull",
   "int arm_smull(int a, int b) {\n"
   "  long long r = (long long)a * b;\n"
   "  return (int)(r >> 16);\n"
   "}\n",
   {0x1234, 0x5678}, "ARMMass"},

  // --- Shift varieties ---
  {"arm_lsl1", "int arm_lsl1(int a) { return a << 1; }\n", {42}, "ARMMass"},
  {"arm_lsl_cl", "int arm_lsl_cl(int a, int b) { return a << (b & 31); }\n", {42, 4}, "ARMMass"},
  {"arm_lsr1", "int arm_lsr1(int a) { return (unsigned int)a >> 1; }\n", {0x80000000}, "ARMMass"},
  {"arm_asr_cl", "int arm_asr_cl(int a, int b) { return a >> (b & 31); }\n", {0xFFFFFC00, 3}, "ARMMass"},

  // --- Comparison ---
  {"arm_eq", "int arm_eq(int a, int b) { return a == b; }\n", {42, 42}, "ARMMass"},
  {"arm_ne", "int arm_ne(int a, int b) { return a != b; }\n", {42, 43}, "ARMMass"},
  {"arm_slt", "int arm_slt(int a, int b) { return a < b; }\n", {0xFFFFFFFF, 1}, "ARMMass"},
  {"arm_uge", "int arm_uge(int a, int b) { return (unsigned int)a >= (unsigned int)b; }\n", {100, 50}, "ARMMass"},

  // --- Conversion ---
  {"arm_cvt_i8", "int arm_cvt_i8(int a) { return (int)(signed char)a; }\n", {0x80}, "ARMMass"},
  {"arm_cvt_u8", "int arm_cvt_u8(int a) { return (int)(unsigned char)a; }\n", {0xFF}, "ARMMass"},
  {"arm_cvt_i16", "int arm_cvt_i16(int a) { return (int)(short)a; }\n", {0x8000}, "ARMMass"},
  {"arm_cvt_u16", "int arm_cvt_u16(int a) { return (int)(unsigned short)a; }\n", {0xFFFF}, "ARMMass"},

  // --- Memory ---
  {"arm_volatile", "int arm_volatile(int a) { volatile int v = a; return v; }\n", {42}, "ARMMass"},
  {"arm_vol16", "int arm_vol16(int a) { volatile short v = (short)a; return v; }\n", {0x1234}, "ARMMass"},
  {"arm_vol8", "int arm_vol8(int a) { volatile signed char v = (signed char)a; return v; }\n", {0x42}, "ARMMass"},

  // --- ALU chains ---
  {"arm_horner",
   "int arm_horner(int x) {\n"
   "  return ((((x+1)*x+2)*x+3)*x+4)*x+5;\n"
   "}\n",
   {3}, "ARMMass"},

  {"arm_sum_digits",
   "int arm_sum_digits(int n) {\n"
   "  int s = 0;\n"
   "  unsigned int u = (unsigned int)(n > 0 ? n : -n);\n"
   "  while (u > 0) { s += u % 10; u /= 10; }\n"
   "  return s;\n"
   "}\n",
   {123456789}, "ARMMass"},

  {"arm_fib_pair",
   "int arm_fib_pair(int n) {\n"
   "  int a=0, b=1;\n"
   "  for (int i=0; i<n; ++i) { int t=a+b; a=b; b=t; }\n"
   "  return a*1000+b;\n"
   "}\n",
   {10}, "ARMMass"},

  {"arm_count_pairs",
   "int arm_count_pairs(int a) {\n"
   "  unsigned int x = (unsigned int)a;\n"
   "  int count = 0;\n"
   "  while (x) { if ((x & 3) == 3) ++count; x >>= 1; }\n"
   "  return count;\n"
   "}\n",
   {0xABCDEF01}, "ARMMass"},

  // --- Branch-heavy ---
  {"arm_classify5",
   "int arm_classify5(int a) {\n"
   "  if (a < -100) return -3;\n"
   "  if (a < -10) return -2;\n"
   "  if (a < 0) return -1;\n"
   "  if (a == 0) return 0;\n"
   "  if (a < 10) return 1;\n"
   "  if (a < 100) return 2;\n"
   "  return 3;\n"
   "}\n",
   {42}, "ARMMass"},

  {"arm_bsearch",
   "int arm_bsearch(int target) {\n"
   "  int vals[8];\n"
   "  for (int i=0; i<8; ++i) vals[i] = i*10;\n"
   "  int lo=0, hi=7;\n"
   "  while (lo <= hi) {\n"
   "    int mid = lo + (hi-lo)/2;\n"
   "    if (vals[mid] == target) return mid;\n"
   "    if (vals[mid] < target) lo = mid+1;\n"
   "    else hi = mid-1;\n"
   "  }\n"
   "  return -1;\n"
   "}\n",
   {30}, "ARMMass"},

  // --- Rotate ---
  {"arm_ror2", "int arm_ror2(int a, int b) { unsigned int x=(unsigned int)a; int s=b&31; return (int)((x>>s)|(x<<(32-s))); }\n", {0xDEADBEEF, 8}, "ARMMass"},

  // --- Division ---
  {"arm_div_neg", "int arm_div_neg(int a, int b) { return a / b; }\n", {(uint64_t)(uint32_t)-100, 7}, "ARMMass"},
  {"arm_mod_neg", "int arm_mod_neg(int a, int b) { return a % b; }\n", {(uint64_t)(uint32_t)-100, 7}, "ARMMass"},

  // --- Power ---
  {"arm_ipow3",
   "int arm_ipow3(int b, int e) {\n"
   "  int r = 1;\n"
   "  while (e > 0) { if (e&1) r*=b; b*=b; e>>=1; }\n"
   "  return r;\n"
   "}\n",
   {2, 15}, "ARMMass"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(ARMMass, ARM32MassRT,
                         ::testing::ValuesIn(kARM32Mass), rtTCName);
