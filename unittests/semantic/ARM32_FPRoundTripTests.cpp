//===- ARM32_FPRoundTripTests.cpp - ARM32 VFP roundtrip tests ---*- C++ -*-===//
//
// Tests ARM32 VFP floating-point instructions through the lift pipeline.
// Uses integer bitcast to avoid VFP register ABI issues.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32FPRoundTrip : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32FPRoundTrip, FPVerify) { roundTripARM32(GetParam()); }

// clang-format off

#define ARM32_FP_RET \
  "  int r;\n" \
  "  __builtin_memcpy(&r, &result, 4);\n" \
  "  return r;\n"

static const std::vector<RoundTripTC> kARM32FPRoundTrip = {
  {"fp_float_add",
   "int fp_float_add(int a, int b) {\n"
   "  float af, bf;\n"
   "  __builtin_memcpy(&af, &a, 4);\n"
   "  __builtin_memcpy(&bf, &b, 4);\n"
   "  float result = af + bf;\n"
   ARM32_FP_RET
   "}\n",
   {0x40A00000, 0x40400000}, "FPRT"},

  {"fp_float_sub",
   "int fp_float_sub(int a, int b) {\n"
   "  float af, bf;\n"
   "  __builtin_memcpy(&af, &a, 4);\n"
   "  __builtin_memcpy(&bf, &b, 4);\n"
   "  float result = af - bf;\n"
   ARM32_FP_RET
   "}\n",
   {0x40A00000, 0x40400000}, "FPRT"},

  {"fp_float_mul",
   "int fp_float_mul(int a, int b) {\n"
   "  float af, bf;\n"
   "  __builtin_memcpy(&af, &a, 4);\n"
   "  __builtin_memcpy(&bf, &b, 4);\n"
   "  float result = af * bf;\n"
   ARM32_FP_RET
   "}\n",
   {0x40A00000, 0x40400000}, "FPRT"},

  {"fp_float_div",
   "int fp_float_div(int a, int b) {\n"
   "  float af, bf;\n"
   "  __builtin_memcpy(&af, &a, 4);\n"
   "  __builtin_memcpy(&bf, &b, 4);\n"
   "  float result = af / bf;\n"
   ARM32_FP_RET
   "}\n",
   {0x41200000, 0x40000000}, "FPRT"},

  {"fp_float_neg",
   "int fp_float_neg(int a) {\n"
   "  float af;\n"
   "  __builtin_memcpy(&af, &a, 4);\n"
   "  float result = -af;\n"
   ARM32_FP_RET
   "}\n",
   {0x40A00000}, "FPRT"},

  {"fp_float_abs",
   "int fp_float_abs(int a) {\n"
   "  float af;\n"
   "  __builtin_memcpy(&af, &a, 4);\n"
   "  float result = af < 0 ? -af : af;\n"
   ARM32_FP_RET
   "}\n",
   {0xC0A00000}, "FPRT"},

  {"fp_int_to_float",
   "int fp_int_to_float(int a) {\n"
   "  float result = (float)a;\n"
   ARM32_FP_RET
   "}\n",
   {42}, "FPRT"},

  {"fp_float_to_int",
   "int fp_float_to_int(int a) {\n"
   "  float af;\n"
   "  __builtin_memcpy(&af, &a, 4);\n"
   "  return (int)af;\n"
   "}\n",
   {0x42280000}, "FPRT"},

  {"fp_float_cmp_lt",
   "int fp_float_cmp_lt(int a, int b) {\n"
   "  float af, bf;\n"
   "  __builtin_memcpy(&af, &a, 4);\n"
   "  __builtin_memcpy(&bf, &b, 4);\n"
   "  return af < bf;\n"
   "}\n",
   {0x40400000, 0x40A00000}, "FPRT"},

  {"fp_float_select",
   "int fp_float_select(int cond, int a, int b) {\n"
   "  float af, bf;\n"
   "  __builtin_memcpy(&af, &a, 4);\n"
   "  __builtin_memcpy(&bf, &b, 4);\n"
   "  float result = cond ? af : bf;\n"
   ARM32_FP_RET
   "}\n",
   {1, 0x40A00000, 0x40400000}, "FPRT"},

  {"fp_float_muladd",
   "int fp_float_muladd(int a, int b, int c) {\n"
   "  float af, bf, cf;\n"
   "  __builtin_memcpy(&af, &a, 4);\n"
   "  __builtin_memcpy(&bf, &b, 4);\n"
   "  __builtin_memcpy(&cf, &c, 4);\n"
   "  float result = af * bf + cf;\n"
   ARM32_FP_RET
   "}\n",
   {0x40A00000, 0x40400000, 0x3F800000}, "FPRT"},

  {"fp_sum_loop",
   "int fp_sum_loop(int n) {\n"
   "  float sum = 0.0f;\n"
   "  for (int i = 1; i <= n; ++i)\n"
   "    sum += (float)i;\n"
   "  float result = sum;\n"
   ARM32_FP_RET
   "}\n",
   {10}, "FPRT"},

  // ========== VFP Double-precision tests (exercises D16-D31) ==========
  {"fp_double_add",
   "int fp_double_add(int a) {\n"
   "  double d = (double)a;\n"
   "  double r = d + 1.5;\n"
   "  return (int)r;\n"
   "}\n",
   {10}, "FPRT", /*OptLevel=*/1},

  {"fp_double_mul",
   "int fp_double_mul(int a) {\n"
   "  double d = (double)a;\n"
   "  double r = d * 3.14159;\n"
   "  return (int)r;\n"
   "}\n",
   {10}, "FPRT", /*OptLevel=*/1},

  {"fp_double_div",
   "int fp_double_div(int a) {\n"
   "  double d = (double)a;\n"
   "  double r = d / 3.0;\n"
   "  return (int)r;\n"
   "}\n",
   {100}, "FPRT", /*OptLevel=*/1},

  {"fp_double_neg",
   "int fp_double_neg(int a) {\n"
   "  double d = (double)a;\n"
   "  double r = -d;\n"
   "  return (int)r;\n"
   "}\n",
   {42}, "FPRT", /*OptLevel=*/1},

  {"fp_double_to_float",
   "int fp_double_to_float(int a) {\n"
   "  double d = (double)a;\n"
   "  float f = (float)d;\n"
   "  float result = f;\n"
   ARM32_FP_RET
   "}\n",
   {42}, "FPRT", /*OptLevel=*/1},

  {"fp_double_cmp",
   "int fp_double_cmp(int a, int b) {\n"
   "  double da = (double)a;\n"
   "  double db = (double)b;\n"
   "  return da < db ? 1 : 0;\n"
   "}\n",
   {3, 7}, "FPRT", /*OptLevel=*/1},

  {"fp_double_chain",
   "int fp_double_chain(int a, int b) {\n"
   "  double da = (double)a;\n"
   "  double db = (double)b;\n"
   "  double r = (da + db) * (da - db);\n"
   "  return (int)r;\n"
   "}\n",
   {10, 3}, "FPRT", /*OptLevel=*/1},

  {"fp_double_sqrt",
   "int fp_double_sqrt(int a) {\n"
   "  double d = (double)a;\n"
   "  double r = 0;\n"
   "  if (d > 0) {\n"
   "    double x = d;\n"
   "    for (int i = 0; i < 20; ++i)\n"
   "      x = (x + d / x) * 0.5;\n"
   "    r = x;\n"
   "  }\n"
   "  return (int)r;\n"
   "}\n",
   {100}, "FPRT", /*OptLevel=*/1},

  {"fp_double_polynomial",
   "int fp_double_polynomial(int x_int) {\n"
   "  double x = (double)x_int;\n"
   "  double r = x * x * x - 2.0 * x * x + 3.0 * x - 4.0;\n"
   "  return (int)r;\n"
   "}\n",
   {5}, "FPRT", /*OptLevel=*/1},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(FPRT, ARM32FPRoundTrip,
                         ::testing::ValuesIn(kARM32FPRoundTrip), rtTCName);
