//===- ARM32_VFPNEONChainRTTests.cpp - ARM32 VFP/NEON chain roundtrip -----===//
#include "SemanticRoundTripFixture.h"

class ARM32VFPNEONChainRT
    : public SemanticRoundTripFixture,
      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VFPNEONChainRT, Verify) {
  roundTripARM32(GetParam());
}

// clang-format off
static const std::vector<RoundTripTC> kARM32VFPNEONChain = {

  {"vmla_f32_chain",
   "int vmla_f32_chain(int a, int b) {\n"
   "  float fa = (float)a, fb = (float)b;\n"
   "  float acc = 1.0f;\n"
   "  acc = acc + fa * 2.0f;\n"
   "  acc = acc + fb * 3.0f;\n"
   "  return (int)acc;\n"
   "}\n",
   {4, 5}, "VFPChain", 2, "-mfloat-abi=softfp -mfpu=vfpv3"},

  {"vmls_f32_chain",
   "int vmls_f32_chain(int a, int b) {\n"
   "  float fa = (float)a, fb = (float)b;\n"
   "  float acc = 100.0f;\n"
   "  acc = acc - fa * 2.0f;\n"
   "  acc = acc - fb * 3.0f;\n"
   "  return (int)acc;\n"
   "}\n",
   {10, 5}, "VFPChain", 2, "-mfloat-abi=softfp -mfpu=vfpv3"},

  {"vmla_f64_chain",
   "int vmla_f64_chain(int a, int b) {\n"
   "  double da = (double)a, db = (double)b;\n"
   "  double acc = da;\n"
   "  acc = acc + db * 2.0;\n"
   "  return (int)acc;\n"
   "}\n",
   {3, 5}, "VFPChain", 2, "-mfloat-abi=softfp -mfpu=vfpv3"},

  {"fp_horner_poly",
   "int fp_horner_poly(int a) {\n"
   "  float x = (float)a * 0.1f;\n"
   "  float r = 5.0f;\n"
   "  r = r * x + 4.0f;\n"
   "  r = r * x + 3.0f;\n"
   "  r = r * x + 2.0f;\n"
   "  r = r * x + 1.0f;\n"
   "  return (int)(r * 10.0f);\n"
   "}\n",
   {5}, "VFPChain", 2, "-mfloat-abi=softfp -mfpu=vfpv3"},

  {"fp_newton_step",
   "int fp_newton_step(int a) {\n"
   "  float x = (float)a;\n"
   "  float y = x * 0.5f;\n"
   "  y = y - (y * y - x) / (2.0f * y);\n"
   "  return (int)(y * 100.0f);\n"
   "}\n",
   {16}, "VFPChain", 2, "-mfloat-abi=softfp -mfpu=vfpv3"},

  {"neon_v4i32_chain",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "int neon_v4i32_chain(int a, int b) {\n"
   "  v4i va = {a, a+1, a+2, a+3};\n"
   "  v4i vb = {b, b+1, b+2, b+3};\n"
   "  v4i sum = va + vb;\n"
   "  v4i prod = sum * va;\n"
   "  return prod[0] + prod[1];\n"
   "}\n",
   {3, 5}, "VFPChain", 1, "-mfpu=neon"},

  {"neon_v8i16_shift_add",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "int neon_v8i16_shift_add(int a) {\n"
   "  v8hi va = {(short)a, (short)(a+1), (short)(a+2), (short)(a+3),\n"
   "             (short)(a+4), (short)(a+5), (short)(a+6), (short)(a+7)};\n"
   "  v8hi shifted = va << 1;\n"
   "  v8hi sum = shifted + va;\n"
   "  return (int)sum[0] + (int)sum[4];\n"
   "}\n",
   {10}, "VFPChain", 1, "-mfpu=neon"},

  {"neon_neg_abs_v4i32",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "int neon_neg_abs_v4i32(int a) {\n"
   "  v4i va = {a, -a, a-5, 5-a};\n"
   "  v4i neg = -va;\n"
   "  v4i mask = neg >> 31;\n"
   "  v4i ab = (neg ^ mask) - mask;\n"
   "  return ab[0] + ab[1] + ab[2] + ab[3];\n"
   "}\n",
   {3}, "VFPChain", 1, "-mfpu=neon"},

  {"vmov_f32_imm",
   "int vmov_f32_imm(int a) {\n"
   "  float fa = (float)a;\n"
   "  float r = fa * 2.0f + 3.0f;\n"
   "  return (int)r;\n"
   "}\n",
   {7}, "VFPChain", 2, "-mfloat-abi=softfp -mfpu=vfpv3"},

  {"vmov_f64_imm",
   "int vmov_f64_imm(int a) {\n"
   "  double da = (double)a;\n"
   "  double r = da * 1.5 + 0.5;\n"
   "  return (int)r;\n"
   "}\n",
   {10}, "VFPChain", 2, "-mfloat-abi=softfp -mfpu=vfpv3"},

};
// clang-format on

INSTANTIATE_TEST_SUITE_P(VFPChain, ARM32VFPNEONChainRT,
                         ::testing::ValuesIn(kARM32VFPNEONChain),
                         [](const auto &P) { return P.param.Name; });
