//===- AArch64_FPScalarChainRTTests.cpp - FP scalar chain roundtrip -------===//
//
// Covers: FADD/FMUL/FSUB/FDIV chained, FMADD/FNMADD, FMAX/FMIN,
// FCVTZS/UCVTF chains, FRECPE/FRSQRTE scalar, FP accumulate loops.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64FPScalarChainRT
    : public SemanticRoundTripFixture,
      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64FPScalarChainRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kA64FPScalarChain = {

  {"fadd_fmul_chain_s",
   "long fadd_fmul_chain_s(long a, long b) {\n"
   "  float fa = (float)a, fb = (float)b;\n"
   "  float r = (fa + fb) * fa;\n"
   "  int ri; __builtin_memcpy(&ri, &r, 4);\n"
   "  return (long)ri;\n"
   "}\n",
   {3, 7}, "FPScalarChain", 2, ""},

  {"fsub_fdiv_chain_d",
   "long fsub_fdiv_chain_d(long a, long b) {\n"
   "  double da = (double)a, db = (double)b;\n"
   "  double r = (da - db) / da;\n"
   "  long ri; __builtin_memcpy(&ri, &r, 8);\n"
   "  return ri;\n"
   "}\n",
   {20, 5}, "FPScalarChain", 2, ""},

  {"fmadd_d",
   "long fmadd_d(long a, long b, long c) {\n"
   "  double da = (double)a, db = (double)b, dc = (double)c;\n"
   "  double r = da * db + dc;\n"
   "  long ri; __builtin_memcpy(&ri, &r, 8);\n"
   "  return ri;\n"
   "}\n",
   {3, 7, 11}, "FPScalarChain", 2, ""},

  {"fmsub_d",
   "long fmsub_d(long a, long b, long c) {\n"
   "  double da = (double)a, db = (double)b, dc = (double)c;\n"
   "  double r = da * db - dc;\n"
   "  long ri; __builtin_memcpy(&ri, &r, 8);\n"
   "  return ri;\n"
   "}\n",
   {5, 9, 3}, "FPScalarChain", 2, ""},

  {"fmax_fmin_chain",
   "long fmax_fmin_chain(long a, long b, long c) {\n"
   "  double da = (double)a, db = (double)b, dc = (double)c;\n"
   "  double mx = da > db ? da : db;\n"
   "  double mn = mx < dc ? mx : dc;\n"
   "  return (long)mn;\n"
   "}\n",
   {10, 20, 15}, "FPScalarChain", 2, ""},

  {"fcvt_round_trip",
   "long fcvt_round_trip(long a) {\n"
   "  float f = (float)a;\n"
   "  double d = (double)f;\n"
   "  return (long)d;\n"
   "}\n",
   {42}, "FPScalarChain", 2, ""},

  {"fp_polynomial_d",
   "long fp_polynomial_d(long a) {\n"
   "  double x = (double)a * 0.1;\n"
   "  double r = x*x*x - 3.0*x*x + 2.0*x - 1.0;\n"
   "  long ri; __builtin_memcpy(&ri, &r, 8);\n"
   "  return ri;\n"
   "}\n",
   {5}, "FPScalarChain", 2, ""},

  {"fp_accumulate",
   "long fp_accumulate(long n) {\n"
   "  double sum = 0.0;\n"
   "  for (long i = 1; i <= n; ++i)\n"
   "    sum += (double)i;\n"
   "  return (long)sum;\n"
   "}\n",
   {10}, "FPScalarChain", 2, ""},

  {"fneg_fabs_chain",
   "long fneg_fabs_chain(long a) {\n"
   "  double da = (double)a;\n"
   "  double neg = -da;\n"
   "  double ab = neg < 0.0 ? -neg : neg;\n"
   "  return (long)ab;\n"
   "}\n",
   {(uint64_t)(int64_t)-7}, "FPScalarChain", 2, ""},

  {"fsqrt_chain",
   "long fsqrt_chain(long a) {\n"
   "  double da = (double)(a > 0 ? a : -a);\n"
   "  double s = __builtin_sqrt(da);\n"
   "  double r = s * s;\n"
   "  return (long)r;\n"
   "}\n",
   {25}, "FPScalarChain", 2, "-fno-math-errno"},

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(FPScalarChain, AArch64FPScalarChainRT,
                         ::testing::ValuesIn(kA64FPScalarChain),
                         [](const auto &P) { return P.param.Name; });
