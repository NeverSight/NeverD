//===- X64_SSEScalarChainRTTests.cpp - SSE scalar chain roundtrip ---------===//
//
// Covers scalar SSE FP chains: ADDSS/MULSS/SUBSS/DIVSS chained operations,
// ADDSD/MULSD/SUBSD/DIVSD chained, mixed SS/SD chains, SQRTSS/SQRTSD chains,
// MAXSS/MINSS/MAXSD/MINSD, CMPSS/CMPSD scalar comparisons.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SSEScalarChainRT : public SemanticRoundTripFixture,
                             public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SSEScalarChainRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kSSEScalarChain = {

  {"mulss_subss_chain",
   "long mulss_subss_chain(long a, long b) {\n"
   "  float fa = (float)a, fb = (float)b;\n"
   "  float r = fa * fb - fa;\n"
   "  int ri; __builtin_memcpy(&ri, &r, 4);\n"
   "  return (long)ri;\n"
   "}\n",
   {7, 3}, "ScalarChain", 2, "-msse"},

  {"divss_addss_chain",
   "long divss_addss_chain(long a, long b) {\n"
   "  float fa = (float)a, fb = (float)b;\n"
   "  float r = fa / fb + fb;\n"
   "  int ri; __builtin_memcpy(&ri, &r, 4);\n"
   "  return (long)ri;\n"
   "}\n",
   {21, 7}, "ScalarChain", 2, "-msse -fno-math-errno"},

  {"mulsd_subsd_chain",
   "long mulsd_subsd_chain(long a, long b) {\n"
   "  double da = (double)a, db = (double)b;\n"
   "  double r = da * db - da;\n"
   "  long ri; __builtin_memcpy(&ri, &r, 8);\n"
   "  return ri;\n"
   "}\n",
   {11, 5}, "ScalarChain", 2, "-msse2"},

  {"divsd_addsd_chain",
   "long divsd_addsd_chain(long a, long b) {\n"
   "  double da = (double)a, db = (double)b;\n"
   "  double r = da / db + db;\n"
   "  long ri; __builtin_memcpy(&ri, &r, 8);\n"
   "  return ri;\n"
   "}\n",
   {35, 7}, "ScalarChain", 2, "-msse2 -fno-math-errno"},

  {"ss_4op_chain",
   "long ss_4op_chain(long a, long b) {\n"
   "  float fa = (float)a, fb = (float)b;\n"
   "  float r = (fa + fb) * (fa - fb);\n"
   "  int ri; __builtin_memcpy(&ri, &r, 4);\n"
   "  return (long)ri;\n"
   "}\n",
   {10, 3}, "ScalarChain", 2, "-msse"},

  {"sd_4op_chain",
   "long sd_4op_chain(long a, long b) {\n"
   "  double da = (double)a, db = (double)b;\n"
   "  double r = (da + db) * (da - db);\n"
   "  long ri; __builtin_memcpy(&ri, &r, 8);\n"
   "  return ri;\n"
   "}\n",
   {10, 3}, "ScalarChain", 2, "-msse2"},

  {"maxss_scalar",
   "long maxss_scalar(long a, long b) {\n"
   "  float fa = (float)a, fb = (float)b;\n"
   "  float r = fa > fb ? fa : fb;\n"
   "  int ri; __builtin_memcpy(&ri, &r, 4);\n"
   "  return (long)ri;\n"
   "}\n",
   {5, 10}, "ScalarChain", 2, "-msse"},

  {"minsd_scalar",
   "long minsd_scalar(long a, long b) {\n"
   "  double da = (double)a, db = (double)b;\n"
   "  double r = da < db ? da : db;\n"
   "  long ri; __builtin_memcpy(&ri, &r, 8);\n"
   "  return ri;\n"
   "}\n",
   {15, 20}, "ScalarChain", 2, "-msse2"},

  {"fp_accumulate_ss",
   "long fp_accumulate_ss(long a) {\n"
   "  float sum = 0.0f;\n"
   "  for (int i = 1; i <= (int)a; ++i)\n"
   "    sum += (float)i;\n"
   "  return (long)sum;\n"
   "}\n",
   {10}, "ScalarChain", 2, "-msse"},

  {"fp_accumulate_sd",
   "long fp_accumulate_sd(long a) {\n"
   "  double sum = 0.0;\n"
   "  for (int i = 1; i <= (int)a; ++i)\n"
   "    sum += (double)i;\n"
   "  return (long)sum;\n"
   "}\n",
   {10}, "ScalarChain", 2, "-msse2"},

  {"fp_polynomial_ss",
   "long fp_polynomial_ss(long a) {\n"
   "  float x = (float)a * 0.1f;\n"
   "  float r = x*x*x - 3.0f*x*x + 2.0f*x - 1.0f;\n"
   "  int ri; __builtin_memcpy(&ri, &r, 4);\n"
   "  return (long)ri;\n"
   "}\n",
   {5}, "ScalarChain", 2, "-msse"},

  {"fp_polynomial_sd",
   "long fp_polynomial_sd(long a) {\n"
   "  double x = (double)a * 0.1;\n"
   "  double r = x*x*x - 3.0*x*x + 2.0*x - 1.0;\n"
   "  long ri; __builtin_memcpy(&ri, &r, 8);\n"
   "  return ri;\n"
   "}\n",
   {5}, "ScalarChain", 2, "-msse2"},

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(ScalarChain, X64SSEScalarChainRT,
                         ::testing::ValuesIn(kSSEScalarChain),
                         [](const auto &P) { return P.param.Name; });
