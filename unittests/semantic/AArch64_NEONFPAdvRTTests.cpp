//===- AArch64_NEONFPAdvRTTests.cpp - NEON FP advanced roundtrip ----------===//
//
// Tests AArch64 FP operations using C expressions computed from parameters
// to avoid rodata constant pool issues.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64NEONFPAdvRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64NEONFPAdvRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kNEONFPAdv = {

  {"fp_add_chain",
   "long fp_add_chain(long a, long b) {\n"
   "  float fa = (float)(int)a, fb = (float)(int)b;\n"
   "  float r = fa + fb + fa + fb;\n"
   "  return (long)(int)r;\n"
   "}\n",
   {5, 7}, "NEONFPAdv", 2, ""},

  {"fp_mul_add",
   "long fp_mul_add(long a, long b) {\n"
   "  float fa = (float)(int)a, fb = (float)(int)b;\n"
   "  float r = fa * fb + fa + fb;\n"
   "  return (long)(int)r;\n"
   "}\n",
   {6, 7}, "NEONFPAdv", 2, ""},

  {"fp_sub_chain",
   "long fp_sub_chain(long a, long b) {\n"
   "  float fa = (float)(int)a, fb = (float)(int)b;\n"
   "  float r = fa - fb + fa - fb;\n"
   "  return (long)(int)r;\n"
   "}\n",
   {20, 3}, "NEONFPAdv", 2, ""},

  {"fp_neg",
   "long fp_neg(long a) {\n"
   "  float fa = (float)(int)a;\n"
   "  float r = -fa;\n"
   "  return (long)(int)r;\n"
   "}\n",
   {42}, "NEONFPAdv", 1, ""},

  {"fp_abs",
   "long fp_abs(long a) {\n"
   "  float fa = (float)(int)a;\n"
   "  float r = fa < 0 ? -fa : fa;\n"
   "  return (long)(int)r;\n"
   "}\n",
   {(uint64_t)(int64_t)-25}, "NEONFPAdv", 1, ""},

  {"fp_max",
   "long fp_max(long a, long b) {\n"
   "  float fa = (float)(int)a, fb = (float)(int)b;\n"
   "  return (long)(int)(fa > fb ? fa : fb);\n"
   "}\n",
   {10, 20}, "NEONFPAdv", 2, ""},

  {"fp_min",
   "long fp_min(long a, long b) {\n"
   "  float fa = (float)(int)a, fb = (float)(int)b;\n"
   "  return (long)(int)(fa < fb ? fa : fb);\n"
   "}\n",
   {10, 20}, "NEONFPAdv", 2, ""},

  {"double_add",
   "long double_add(long a, long b) {\n"
   "  double da = (double)a, db = (double)b;\n"
   "  return (long)(da + db);\n"
   "}\n",
   {100, 200}, "NEONFPAdv", 2, ""},

  {"double_mul",
   "long double_mul(long a, long b) {\n"
   "  double da = (double)a, db = (double)b;\n"
   "  return (long)(da * db);\n"
   "}\n",
   {7, 11}, "NEONFPAdv", 2, ""},

  {"int_to_float_roundtrip",
   "long int_to_float_roundtrip(long a) {\n"
   "  float fa = (float)(int)a;\n"
   "  int ri = (int)(fa + fa);\n"
   "  return (long)ri;\n"
   "}\n",
   {123}, "NEONFPAdv", 1, ""},

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(NEONFPAdv, AArch64NEONFPAdvRT,
    ::testing::ValuesIn(kNEONFPAdv),
    [](const auto &I) { return I.param.Name; });
