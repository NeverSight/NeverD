//===- ARM32_IntCExprExtRTTests.cpp - ARM32 C expression roundtrip --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "SemanticRoundTripFixture.h"

class ARM32IntCExprExtRT
    : public SemanticRoundTripFixture,
      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32IntCExprExtRT, Verify) {
  roundTripARM32(GetParam());
}

// clang-format off
static const std::vector<RoundTripTC> kARM32IntCExprExt = {

  {"c_bitfield_insert_arm",
   "int c_bitfield_insert_arm(int val, int field) {\n"
   "  return (val & ~0xFF00) | ((field & 0xFF) << 8);\n"
   "}\n",
   {0xDEADBEEF, 0x42}, "IntCExpr", 2, ""},

  {"c_bswap32_arm",
   "int c_bswap32_arm(int a) {\n"
   "  return __builtin_bswap32(a);\n"
   "}\n",
   {0x01020304}, "IntCExpr", 2, ""},

  {"c_clz_arm",
   "int c_clz_arm(int a) {\n"
   "  return a ? __builtin_clz(a) : 32;\n"
   "}\n",
   {0x100}, "IntCExpr", 2, ""},

  {"c_abs_diff_arm",
   "int c_abs_diff_arm(int a, int b) {\n"
   "  int d = a - b;\n"
   "  return d < 0 ? -d : d;\n"
   "}\n",
   {30, 50}, "IntCExpr", 2, ""},

  {"c_fibonacci_arm",
   "int c_fibonacci_arm(int n) {\n"
   "  int a = 0, b = 1;\n"
   "  for (int i = 0; i < n; ++i) { int t = a + b; a = b; b = t; }\n"
   "  return a;\n"
   "}\n",
   {20}, "IntCExpr", 2, ""},

  {"c_gcd_arm",
   "int c_gcd_arm(int a, int b) {\n"
   "  while (b) { int t = b; b = a % b; a = t; }\n"
   "  return a;\n"
   "}\n",
   {48, 36}, "IntCExpr", 2, ""},

  {"c_power_arm",
   "int c_power_arm(int base, int exp) {\n"
   "  int result = 1;\n"
   "  while (exp > 0) {\n"
   "    if (exp & 1) result *= base;\n"
   "    base *= base;\n"
   "    exp >>= 1;\n"
   "  }\n"
   "  return result;\n"
   "}\n",
   {3, 10}, "IntCExpr", 2, ""},

  {"c_clamp_arm",
   "int c_clamp_arm(int val, int lo, int hi) {\n"
   "  if (val < lo) return lo;\n"
   "  if (val > hi) return hi;\n"
   "  return val;\n"
   "}\n",
   {100, 10, 50}, "IntCExpr", 2, ""},

  {"c_sext8_arm",
   "int c_sext8_arm(int a) {\n"
   "  return (int)(signed char)a;\n"
   "}\n",
   {0x80}, "IntCExpr", 2, ""},

  {"c_zext16_arm",
   "int c_zext16_arm(int a) {\n"
   "  return (int)(unsigned short)a;\n"
   "}\n",
   {0xDEADBEEF}, "IntCExpr", 2, ""},

  {"c_mul_add_arm",
   "int c_mul_add_arm(int a, int b, int c) {\n"
   "  return a * b + c;\n"
   "}\n",
   {7, 11, 3}, "IntCExpr", 2, ""},

  {"c_umull_arm",
   "int c_umull_arm(int a, int b) {\n"
   "  unsigned long long r = (unsigned long long)(unsigned)a * (unsigned)b;\n"
   "  return (int)(r >> 32);\n"
   "}\n",
   {0x80000000, 3}, "IntCExpr", 2, ""},

};
// clang-format on

INSTANTIATE_TEST_SUITE_P(IntCExpr, ARM32IntCExprExtRT,
                         ::testing::ValuesIn(kARM32IntCExprExt),
                         [](const auto &P) { return P.param.Name; });
