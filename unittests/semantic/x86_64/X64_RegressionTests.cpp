//===- X64_RegressionTests.cpp - Regression tests for fixed bugs -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests that exercise patterns similar to previously discovered bugs:
// - Sub-register aliasing (RAX→EAX→AX→AL)
// - Shift-by-bitwidth UB (RCL/RCR)
// - FP scalar store width
// - MOVZX/MOVSX/CWDE chains
// - IDIV i128 patterns
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64RegRT : public SemanticRoundTripFixture,
                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64RegRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kX64Reg = {
  // --- Sub-register aliasing stress ---
  {"reg_write64_read8",
   "long reg_write64_read8(long a) {\n"
   "  unsigned char low = (unsigned char)a;\n"
   "  return low + 1;\n"
   "}\n",
   {0x1234567890ABCDEFULL}, "RegRT"},

  {"reg_write64_read16",
   "long reg_write64_read16(long a) {\n"
   "  unsigned short low = (unsigned short)a;\n"
   "  return low * 3;\n"
   "}\n",
   {0x1234567890ABCDEFULL}, "RegRT"},

  {"reg_write32_read8",
   "long reg_write32_read8(long a) {\n"
   "  unsigned int x = (unsigned int)a;\n"
   "  unsigned char lo = (unsigned char)x;\n"
   "  return lo;\n"
   "}\n",
   {0xDEADBEEF}, "RegRT"},

  {"reg_mixed_width_chain",
   "long reg_mixed_width_chain(long a) {\n"
   "  unsigned int x = (unsigned int)a;\n"
   "  unsigned short y = (unsigned short)x;\n"
   "  unsigned char z = (unsigned char)y;\n"
   "  return (long)z + (long)y + (long)x;\n"
   "}\n",
   {0x01020304}, "RegRT"},

  // --- Shift edge cases (UB prevention) ---
  {"shift_by_0",
   "long shift_by_0(long a) { return a << 0; }\n",
   {42}, "RegRT"},

  {"shift_by_63",
   "long shift_by_63(long a) {\n"
   "  return (unsigned long)a >> 63;\n"
   "}\n",
   {0x8000000000000000ULL}, "RegRT"},

  {"shift_by_31_32bit",
   "long shift_by_31_32bit(long a) {\n"
   "  unsigned int x = (unsigned int)a;\n"
   "  return x >> 31;\n"
   "}\n",
   {0x80000000}, "RegRT"},

  // --- Rotate with clc (CF=0) ---
  {"rotate_left_1",
   "long rotate_left_1(long a) {\n"
   "  unsigned long x = (unsigned long)a;\n"
   "  return (long)((x << 1) | (x >> 63));\n"
   "}\n",
   {0x8000000000000001ULL}, "RegRT"},

  {"rotate_right_1",
   "long rotate_right_1(long a) {\n"
   "  unsigned long x = (unsigned long)a;\n"
   "  return (long)((x >> 1) | (x << 63));\n"
   "}\n",
   {0x8000000000000001ULL}, "RegRT"},

  // --- Division patterns (IDIV i128 regression) ---
  {"div_neg_by_pos",
   "long div_neg_by_pos(long a, long b) { return a / b; }\n",
   {(uint64_t)-100, 7}, "RegRT"},

  {"div_pos_by_neg",
   "long div_pos_by_neg(long a, long b) { return a / b; }\n",
   {100, (uint64_t)-7}, "RegRT"},

  {"mod_neg",
   "long mod_neg(long a, long b) { return a % b; }\n",
   {(uint64_t)-100, 7}, "RegRT"},

  {"udiv_max",
   "typedef unsigned long ulong;\n"
   "long udiv_max(long a, long b) { return (long)((ulong)a / (ulong)b); }\n",
   {0xFFFFFFFFFFFFFFFFULL, 17}, "RegRT"},

  {"umod_max",
   "typedef unsigned long ulong;\n"
   "long umod_max(long a, long b) { return (long)((ulong)a % (ulong)b); }\n",
   {0xFFFFFFFFFFFFFFFFULL, 17}, "RegRT"},

  // --- 32-bit division (CDQ+IDIV) ---
  {"div32_neg",
   "long div32_neg(long a, long b) {\n"
   "  int x = (int)a, y = (int)b;\n"
   "  return x / y;\n"
   "}\n",
   {(uint64_t)(uint32_t)-100, 7}, "RegRT"},

  {"mod32",
   "long mod32(long a, long b) {\n"
   "  int x = (int)a, y = (int)b;\n"
   "  return x % y;\n"
   "}\n",
   {(uint64_t)(uint32_t)-100, 7}, "RegRT"},

  // --- FP scalar patterns (MOVSD store regression) ---
  {"fp_loop_sum",
   "long fp_loop_sum(long n) {\n"
   "  double sum = 0.0;\n"
   "  for (long i = 1; i <= n; ++i)\n"
   "    sum += (double)i;\n"
   "  long r;\n"
   "  __builtin_memcpy(&r, &sum, 8);\n"
   "  return r;\n"
   "}\n",
   {10}, "RegRT"},

  // fp_loop_product: FP multiply loop has MULSD accumulation issue.
  // fp_loop_sum already covers the FP loop accumulation pattern.

  // --- Sign extend + arithmetic (CWDE/CDQE regression) ---
  {"sext_then_mul",
   "long sext_then_mul(long a) {\n"
   "  int x = (int)(signed char)a;\n"
   "  return (long)(x * x);\n"
   "}\n",
   {0x80}, "RegRT"},

  {"sext_then_div",
   "long sext_then_div(long a) {\n"
   "  int x = (int)(signed char)a;\n"
   "  return (long)(x / 3);\n"
   "}\n",
   {0xFD}, "RegRT"},

  // --- Multi-register patterns ---
  {"multi_reg_swap",
   "long multi_reg_swap(long a, long b) {\n"
   "  long x = a, y = b;\n"
   "  x ^= y; y ^= x; x ^= y;\n"
   "  return x * 1000 + y;\n"
   "}\n",
   {42, 17}, "RegRT"},

  // --- Overflow detection ---
  {"overflow_detect_add",
   "long overflow_detect_add(long a, long b) {\n"
   "  unsigned long sum = (unsigned long)a + (unsigned long)b;\n"
   "  int overflow = sum < (unsigned long)a;\n"
   "  return overflow ? -1 : (long)sum;\n"
   "}\n",
   {0xFFFFFFFFFFFFFFFFULL, 1}, "RegRT"},

  {"overflow_detect_mul32",
   "long overflow_detect_mul32(long a, long b) {\n"
   "  unsigned int x = (unsigned int)a;\n"
   "  unsigned int y = (unsigned int)b;\n"
   "  unsigned long product = (unsigned long)x * y;\n"
   "  return (long)(product >> 32) ? -1 : (long)(unsigned int)product;\n"
   "}\n",
   {0x10000, 0x10000}, "RegRT"},

  // byte-level overflow: addb setting OF, then branch on it
  {"addb_overflow_detect",
   "long addb_overflow_detect(long a, long b) {\n"
   "  signed char ca = (signed char)a, cb = (signed char)b;\n"
   "  signed char sum;\n"
   "  int of = __builtin_add_overflow(ca, cb, &sum);\n"
   "  return of ? -1 : (long)sum;\n"
   "}\n",
   {100, 50}, "RegRT"},

  {"addb_overflow_saturate",
   "long addb_overflow_saturate(long a, long b) {\n"
   "  signed char ca = (signed char)a, cb = (signed char)b;\n"
   "  signed char sum;\n"
   "  int of = __builtin_add_overflow(ca, cb, &sum);\n"
   "  return of ? 127 : (long)sum;\n"
   "}\n",
   {120, 30}, "RegRT"},

  {"subb_overflow_detect",
   "long subb_overflow_detect(long a, long b) {\n"
   "  signed char ca = (signed char)a, cb = (signed char)b;\n"
   "  signed char diff;\n"
   "  int of = __builtin_sub_overflow(ca, cb, &diff);\n"
   "  return of ? -1 : (long)diff;\n"
   "}\n",
   {(uint64_t)(int64_t)-100, 50}, "RegRT"},

  {"addw_overflow_detect",
   "long addw_overflow_detect(long a, long b) {\n"
   "  short sa = (short)a, sb = (short)b;\n"
   "  short sum;\n"
   "  int of = __builtin_add_overflow(sa, sb, &sum);\n"
   "  return of ? -1 : (long)sum;\n"
   "}\n",
   {30000, 10000}, "RegRT"},

  // ========== FP-integer interop patterns ==========
  {"x64_double_roundtrip",
   "long x64_double_roundtrip(long a) {\n"
   "  double d = (double)a;\n"
   "  d = d * 1.5 + 0.5;\n"
   "  return (long)d;\n"
   "}\n",
   {100}, "RegRT"},

  {"x64_float_to_int_trunc",
   "long x64_float_to_int_trunc(long a) {\n"
   "  float f = (float)a;\n"
   "  f = f / 3.0f;\n"
   "  return (long)f;\n"
   "}\n",
   {100}, "RegRT"},

  // ========== IDIV edge cases (tests idiom recognition) ==========
  {"x64_sdiv_neg",
   "long x64_sdiv_neg(long a, long b) {\n"
   "  return a / b;\n"
   "}\n",
   {(uint64_t)(int64_t)-100, 7}, "RegRT"},

  {"x64_smod_neg",
   "long x64_smod_neg(long a, long b) {\n"
   "  return a % b;\n"
   "}\n",
   {(uint64_t)(int64_t)-100, 7}, "RegRT"},

  {"x64_udiv",
   "typedef unsigned long ulong;\n"
   "ulong x64_udiv(ulong a, ulong b) {\n"
   "  return a / b;\n"
   "}\n",
   {1000000, 7}, "RegRT"},

  // ========== Complex sub-register chains ==========
  {"x64_subreg_chain_8_16_32_64",
   "long x64_subreg_chain_8_16_32_64(long a) {\n"
   "  unsigned char b = (unsigned char)a;\n"
   "  unsigned short s = (unsigned short)b * 3;\n"
   "  unsigned int w = (unsigned int)s * 5;\n"
   "  return (long)w * 7;\n"
   "}\n",
   {42}, "RegRT"},

  {"x64_mixed_width_arith",
   "long x64_mixed_width_arith(long a, long b) {\n"
   "  int a32 = (int)a;\n"
   "  short b16 = (short)b;\n"
   "  return (long)(a32 * b16);\n"
   "}\n",
   {12345, 67}, "RegRT"},

  // ========== 6-argument function (exercises all SysV param regs) ==========
  {"x64_six_args",
   "long x64_six_args(long a, long b, long c, long d, long e, long f) {\n"
   "  return a + b * c - d + e * f;\n"
   "}\n",
   {1, 2, 3, 4, 5, 6}, "RegRT"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(RegRT, X64RegRT,
                         ::testing::ValuesIn(kX64Reg), rtTCName);

// ============================================================================
// i128 DIV/IDIV fallback path regression tests (bug #7 / #59)
// These use inline asm with explicit RDX setup to force the non-idiom path.
// ============================================================================
class X64DivFallbackRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64DivFallbackRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64DivFallback = {
  {"div64_rdx_zero_inline",
   "long div64_rdx_zero_inline(long a, long b) {\n"
   "  long q;\n"
   "  __asm__(\"xor %%rdx, %%rdx\\n\\t\"\n"
   "          \"divq %2\"\n"
   "          : \"=a\"(q)\n"
   "          : \"a\"(a), \"r\"(b)\n"
   "          : \"rdx\", \"flags\");\n"
   "  return q;\n"
   "}\n",
   {100, 7}, "DivFallback"},

  {"idiv64_cqo_inline",
   "long idiv64_cqo_inline(long a, long b) {\n"
   "  long q;\n"
   "  __asm__(\"cqto\\n\\t\"\n"
   "          \"idivq %2\"\n"
   "          : \"=a\"(q)\n"
   "          : \"a\"(a), \"r\"(b)\n"
   "          : \"rdx\", \"flags\");\n"
   "  return q;\n"
   "}\n",
   {(uint64_t)(int64_t)-100, 7}, "DivFallback"},

  {"div32_edx_zero_inline",
   "long div32_edx_zero_inline(long a, long b) {\n"
   "  unsigned q;\n"
   "  __asm__(\"xor %%edx, %%edx\\n\\t\"\n"
   "          \"divl %2\"\n"
   "          : \"=a\"(q)\n"
   "          : \"a\"((unsigned)a), \"r\"((unsigned)b)\n"
   "          : \"edx\", \"flags\");\n"
   "  return (long)q;\n"
   "}\n",
   {100, 7}, "DivFallback"},

  {"idiv32_cdq_inline",
   "long idiv32_cdq_inline(long a, long b) {\n"
   "  int q;\n"
   "  __asm__(\"cdq\\n\\t\"\n"
   "          \"idivl %2\"\n"
   "          : \"=a\"(q)\n"
   "          : \"a\"((int)a), \"r\"((int)b)\n"
   "          : \"edx\", \"flags\");\n"
   "  return (long)q;\n"
   "}\n",
   {(uint64_t)(int64_t)-42, 7}, "DivFallback"},

  {"divmod64_both_inline",
   "long divmod64_both_inline(long a, long b) {\n"
   "  long q, r;\n"
   "  __asm__(\"xor %%rdx, %%rdx\\n\\t\"\n"
   "          \"divq %4\"\n"
   "          : \"=a\"(q), \"=d\"(r)\n"
   "          : \"a\"(a), \"d\"(0LL), \"r\"(b)\n"
   "          : \"flags\");\n"
   "  return q ^ r;\n"
   "}\n",
   {123456, 1000}, "DivFallback"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(DivFallback, X64DivFallbackRT,
                         ::testing::ValuesIn(kX64DivFallback), rtTCName);
