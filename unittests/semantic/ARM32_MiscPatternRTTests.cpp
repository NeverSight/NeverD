//===- ARM32_MiscPatternRTTests.cpp - Misc ARM32 RT -----------*- C++ -*-===//
//
// Tests ARM32 miscellaneous patterns: REV, CLZ, BFI, UBFX, SMULL/UMULL,
// and conditional patterns exercising IT blocks.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32MiscRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32MiscRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kARM32Misc = {
  // ========== REV (byte swap) ==========
  {"arm_rev",
   "int arm_rev(int a) {\n"
   "  return (int)__builtin_bswap32((unsigned)a);\n"
   "}\n",
   {0x01020304}, "ARM32Misc"},

  // ========== REV16 (byte swap within halfwords, -O1 for clean codegen) ==========
  {"arm_rev16",
   "int arm_rev16(int a) {\n"
   "  unsigned x = (unsigned)a;\n"
   "  return (int)(((x & 0xFF00FF00u) >> 8) | ((x & 0x00FF00FFu) << 8));\n"
   "}\n",
   {0x01020304}, "ARM32Misc", /*OptLevel=*/1},

  // ========== BFI pattern ==========
  {"arm_bfi",
   "int arm_bfi(int a, int b) {\n"
   "  unsigned mask = 0xFF00u;\n"
   "  return (int)(((unsigned)a & ~mask) | (((unsigned)b << 8) & mask));\n"
   "}\n",
   {0xDEADBEEF, 0x42}, "ARM32Misc"},

  // ========== UBFX pattern ==========
  {"arm_ubfx",
   "int arm_ubfx(int a) {\n"
   "  return ((unsigned)a >> 8) & 0xFF;\n"
   "}\n",
   {0xDEADBEEF}, "ARM32Misc"},

  // ========== SMULL (signed widening multiply) ==========
  {"arm_smull",
   "int arm_smull_lo(int a, int b) {\n"
   "  long long r = (long long)a * b;\n"
   "  return (int)r;\n"
   "}\n",
   {100000, 200000}, "ARM32Misc"},

  {"arm_smull_hi",
   "int arm_smull_hi(int a, int b) {\n"
   "  long long r = (long long)a * b;\n"
   "  return (int)(r >> 32);\n"
   "}\n",
   {100000, 200000}, "ARM32Misc"},

  // ========== UMULL (unsigned widening multiply) ==========
  {"arm_umull",
   "int arm_umull_lo(int a, int b) {\n"
   "  unsigned long long r = (unsigned long long)(unsigned)a * (unsigned)b;\n"
   "  return (int)(unsigned)r;\n"
   "}\n",
   {0xFFFFFFFF, 2}, "ARM32Misc"},

  // ========== Conditional patterns (IT blocks) ==========
  {"arm_cond_add",
   "int arm_cond_add(int a, int b) {\n"
   "  if (a > 0) return a + b;\n"
   "  return a - b;\n"
   "}\n",
   {10, 5}, "ARM32Misc"},

  {"arm_cond_add_neg",
   "int arm_cond_add_neg(int a, int b) {\n"
   "  if (a > 0) return a + b;\n"
   "  return a - b;\n"
   "}\n",
   {(uint64_t)(int32_t)-10, 5}, "ARM32Misc"},

  // ========== Min/max ==========
  {"arm_min",
   "int arm_min(int a, int b) {\n"
   "  return a < b ? a : b;\n"
   "}\n",
   {42, 100}, "ARM32Misc"},

  {"arm_max",
   "int arm_max(int a, int b) {\n"
   "  return a > b ? a : b;\n"
   "}\n",
   {42, 100}, "ARM32Misc"},

  // ========== Unsigned min/max ==========
  {"arm_umin",
   "int arm_umin(int a, int b) {\n"
   "  return (unsigned)a < (unsigned)b ? a : b;\n"
   "}\n",
   {0xFFFFFF00, 0x100}, "ARM32Misc"},

  // ========== Multi-way branch ==========
  {"arm_switch4",
   "int arm_switch4(int a) {\n"
   "  switch (a & 3) {\n"
   "    case 0: return 10;\n"
   "    case 1: return 20;\n"
   "    case 2: return 30;\n"
   "    default: return 40;\n"
   "  }\n"
   "}\n",
   {2}, "ARM32Misc"},

  // ========== FP int->float->int roundtrip ==========
  {"arm_fp_roundtrip",
   "int arm_fp_roundtrip(int a) {\n"
   "  float f = (float)a;\n"
   "  return (int)f;\n"
   "}\n",
   {42}, "ARM32Misc"},

  // ========== Rotate right ==========
  {"arm_ror",
   "int arm_ror(int a, int n) {\n"
   "  unsigned x = (unsigned)a;\n"
   "  unsigned r = (x >> (n & 31)) | (x << (32 - (n & 31)));\n"
   "  return (int)r;\n"
   "}\n",
   {0xDEADBEEF, 8}, "ARM32Misc"},

  // ========== Saturating add 16-bit ==========
  {"arm_qadd16",
   "int arm_qadd16(int a, int b) {\n"
   "  int sum = (short)(a & 0xFFFF) + (short)(b & 0xFFFF);\n"
   "  if (sum > 32767) sum = 32767;\n"
   "  if (sum < -32768) sum = -32768;\n"
   "  return (int)(unsigned short)(short)sum;\n"
   "}\n",
   {30000, 10000}, "ARM32Misc"},

  // ========== Large constant (movw/movt, no literal pool) ==========
  {"arm_large_const",
   "int arm_large_const(int a) {\n"
   "  return a + (int)0xDEADBEEFu;\n"
   "}\n",
   {0x100}, "ARM32Misc", /*OptLevel=*/1},

  // ========== FP constant from literal pool (vldr [pc, #off]) ==========
  {"arm_fp_const_mul",
   "int arm_fp_const_mul(int a) {\n"
   "  float f = (float)a;\n"
   "  float g = f * 3.14159f;\n"
   "  return (int)g;\n"
   "}\n",
   {100}, "ARM32Misc", /*OptLevel=*/1},

  {"arm_double_const",
   "int arm_double_const(int a) {\n"
   "  double d = (double)a;\n"
   "  double r = d * 2.71828;\n"
   "  return (int)r;\n"
   "}\n",
   {37}, "ARM32Misc", /*OptLevel=*/1},

  // ========== VFP double precision with multiple D16+ registers ==========
  {"arm_lerp_double",
   "int arm_lerp_double(int a, int b, int t) {\n"
   "  double da = (double)a;\n"
   "  double db = (double)b;\n"
   "  double dt = (double)t * 0.01;\n"
   "  double r = da + (db - da) * dt;\n"
   "  return (int)r;\n"
   "}\n",
   {10, 50, 75}, "ARM32Misc", /*OptLevel=*/1},

  {"arm_fp_max",
   "int arm_fp_max(int a, int b) {\n"
   "  double da = (double)a;\n"
   "  double db = (double)b;\n"
   "  return (int)(da > db ? da : db);\n"
   "}\n",
   {42, 100}, "ARM32Misc", /*OptLevel=*/1},

  {"arm_fp_clamp",
   "int arm_fp_clamp(int val, int lo, int hi) {\n"
   "  double d = (double)val;\n"
   "  double dlo = (double)lo;\n"
   "  double dhi = (double)hi;\n"
   "  if (d < dlo) d = dlo;\n"
   "  if (d > dhi) d = dhi;\n"
   "  return (int)d;\n"
   "}\n",
   {200, 10, 100}, "ARM32Misc", /*OptLevel=*/1},

  // ========== Multi-arg integer patterns ==========
  {"arm_4arg_sum",
   "int arm_4arg_sum(int a, int b, int c, int d) {\n"
   "  return a + b + c + d;\n"
   "}\n",
   {10, 20, 30, 40}, "ARM32Misc"},

  {"arm_weighted_avg",
   "int arm_weighted_avg(int a, int b, int wa, int wb) {\n"
   "  return (a * wa + b * wb) / (wa + wb);\n"
   "}\n",
   {100, 200, 3, 7}, "ARM32Misc"},

  // ========== Nested conditional patterns ==========
  {"arm_sign",
   "int arm_sign(int x) {\n"
   "  if (x > 0) return 1;\n"
   "  if (x < 0) return -1;\n"
   "  return 0;\n"
   "}\n",
   {(uint64_t)(int32_t)-42}, "ARM32Misc"},

  {"arm_abs",
   "int arm_abs(int x) {\n"
   "  return x < 0 ? -x : x;\n"
   "}\n",
   {(uint64_t)(int32_t)-42}, "ARM32Misc"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(ARM32Misc, ARM32MiscRT,
                         ::testing::ValuesIn(kARM32Misc), rtTCName);
