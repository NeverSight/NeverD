//===- ARM32_FPScalarChainRTTests.cpp - ARM32 FP scalar chain roundtrip ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Covers: VADD/VMUL/VSUB/VDIV chained (VFP), VNMUL, VMLA/VMLS scalar,
// VCVT chains, FP accumulate loops.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32FPScalarChainRT
    : public SemanticRoundTripFixture,
      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32FPScalarChainRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kARM32FPScalarChain = {

  {"vadd_vmul_chain_s",
   "int vadd_vmul_chain_s(int a, int b) {\n"
   "  float fa = (float)a, fb = (float)b;\n"
   "  float r = (fa + fb) * fa;\n"
   "  int ri; __builtin_memcpy(&ri, &r, 4);\n"
   "  return ri;\n"
   "}\n",
   {3, 7}, "FPChain", 2, "-mfloat-abi=softfp -mfpu=vfpv3"},

  {"vsub_vdiv_chain_s",
   "int vsub_vdiv_chain_s(int a, int b) {\n"
   "  float fa = (float)a, fb = (float)b;\n"
   "  float r = (fa - fb) / fa;\n"
   "  int ri; __builtin_memcpy(&ri, &r, 4);\n"
   "  return ri;\n"
   "}\n",
   {20, 5}, "FPChain", 2, "-mfloat-abi=softfp -mfpu=vfpv3 -fno-math-errno"},

  {"vadd_vmul_chain_d",
   "int vadd_vmul_chain_d(int a, int b) {\n"
   "  double da = (double)a, db = (double)b;\n"
   "  double r = (da + db) * da;\n"
   "  return (int)r;\n"
   "}\n",
   {3, 7}, "FPChain", 2, "-mfloat-abi=softfp -mfpu=vfpv3"},

  {"fp_mul_add_chain",
   "int fp_mul_add_chain(int a, int b) {\n"
   "  float fa = (float)a, fb = (float)b;\n"
   "  float r = fa + fb;\n"
   "  return (int)r;\n"
   "}\n",
   {3, 4}, "FPChain", 2, "-mfloat-abi=softfp -mfpu=vfpv3"},

  {"fp_accumulate",
   "int fp_accumulate(int n) {\n"
   "  float sum = 0.0f;\n"
   "  for (int i = 1; i <= n; ++i)\n"
   "    sum += (float)i;\n"
   "  return (int)sum;\n"
   "}\n",
   {10}, "FPChain", 2, "-mfloat-abi=softfp -mfpu=vfpv3"},

  {"vneg_vabs_chain",
   "int vneg_vabs_chain(int a) {\n"
   "  float fa = (float)a;\n"
   "  float neg = -fa;\n"
   "  float ab = neg < 0.0f ? -neg : neg;\n"
   "  return (int)ab;\n"
   "}\n",
   {(uint64_t)(int32_t)-7}, "FPChain", 2, "-mfloat-abi=softfp -mfpu=vfpv3"},

  {"vsqrt_chain",
   "int vsqrt_chain(int a) {\n"
   "  float f = (float)(a > 0 ? a : -a);\n"
   "  float s = __builtin_sqrtf(f);\n"
   "  float r = s * f;\n"
   "  int ri; __builtin_memcpy(&ri, &r, 4);\n"
   "  return ri;\n"
   "}\n",
   {16}, "FPChain", 2, "-mfloat-abi=softfp -mfpu=vfpv3 -fno-math-errno"},

  {"fmax_fmin_chain",
   "int fmax_fmin_chain(int a, int b) {\n"
   "  float fa = (float)a, fb = (float)b;\n"
   "  float mx = fa > fb ? fa : fb;\n"
   "  float mn = mx < (float)(a+b) ? mx : (float)(a+b);\n"
   "  return (int)mn;\n"
   "}\n",
   {10, 20}, "FPChain", 2, "-mfloat-abi=softfp -mfpu=vfpv3"},

  // Bug #137 targeted: VMLA/polynomial chain
  {"fp_polynomial_eval",
   "int fp_polynomial_eval(int a) {\n"
   "  float x = (float)a * 0.1f;\n"
   "  float r = 1.0f + x * (2.0f + x * 3.0f);\n"
   "  return (int)(r * 100.0f);\n"
   "}\n",
   {5}, "FPChain", 2, "-mfloat-abi=softfp -mfpu=vfpv3"},

  {"fp_vmla_chain",
   "int fp_vmla_chain(int a, int b) {\n"
   "  float fa = (float)a, fb = (float)b;\n"
   "  float acc = fa;\n"
   "  acc = acc + fb * 2.0f;\n"
   "  acc = acc + fa * 3.0f;\n"
   "  return (int)acc;\n"
   "}\n",
   {4, 5}, "FPChain", 2, "-mfloat-abi=softfp -mfpu=vfpv3"},

  {"fp_double_chain",
   "int fp_double_chain(int a, int b) {\n"
   "  double da = (double)a, db = (double)b;\n"
   "  double r = da * db + da - db;\n"
   "  return (int)r;\n"
   "}\n",
   {7, 3}, "FPChain", 2, "-mfloat-abi=softfp -mfpu=vfpv3"},

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(FPChain, ARM32FPScalarChainRT,
                         ::testing::ValuesIn(kARM32FPScalarChain),
                         [](const auto &P) { return P.param.Name; });
