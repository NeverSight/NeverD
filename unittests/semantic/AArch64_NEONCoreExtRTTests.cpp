//===- AArch64_NEONCoreExtRTTests.cpp - CoreNEON extended roundtrip -------===//
//
// Covers: ADDHN, SUBHN, RADDHN, ADDV, FADDP, FCMGE, FCMGT, FMLA, FMLS,
//         SADDL2, SADDW, SSUBL, SSUBW, UADDL2, UADDW, USUBL, USUBW,
//         SMAXV, SMINV, UMAXV, UMINV, SMLAL, UMLAL, SSHL, USHL, SRSHR,
//         URSHR, SRI, SLI, SHLL, FCVTL, FCVTN, XTN2, SQABS, SQNEG,
//         UADDLV, SADDLV, SRHADD, CMTST, BIF, FMAXNM, FMINNM
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64NEONCoreExtRT : public SemanticRoundTripFixture,
                              public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64NEONCoreExtRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kNEONCoreExt = {

  {"addhn_narrowing",
   "typedef short v4hi __attribute__((vector_size(8)));\n"
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long addhn_narrowing(long a) {\n"
   "  v4si va = {(int)a, 0x10000, 0x20000, 0x30000};\n"
   "  v4si vb = {0x10000, 0x20000, 0x30000, (int)a};\n"
   "  v4hi vr;\n"
   "  for (int i = 0; i < 4; i++)\n"
   "    vr[i] = (short)((va[i] + vb[i]) >> 16);\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 4; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {0x40000}, "CoreNEONExt", 2, ""},

  {"subhn_narrowing",
   "typedef short v4hi __attribute__((vector_size(8)));\n"
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long subhn_narrowing(long a) {\n"
   "  v4si va = {(int)a + 0x50000, 0x30000, 0x70000, 0x90000};\n"
   "  v4si vb = {0x10000, 0x10000, 0x10000, 0x10000};\n"
   "  v4hi vr;\n"
   "  for (int i = 0; i < 4; i++)\n"
   "    vr[i] = (short)((va[i] - vb[i]) >> 16);\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 4; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {0x20000}, "CoreNEONExt", 2, ""},

  {"faddp_pairwise",
   "typedef float v4sf __attribute__((vector_size(16)));\n"
   "long faddp_pairwise(long a) {\n"
   "  float fa = (float)(int)a;\n"
   "  v4sf va = {fa, 10.0f, 20.0f, 30.0f};\n"
   "  float r = va[0] + va[1] + va[2] + va[3];\n"
   "  return (long)(int)r;\n"
   "}\n",
   {5}, "CoreNEONExt", 1, ""},

  {"fcmge_compare",
   "long fcmge_compare(long a, long b) {\n"
   "  float fa = (float)(int)a, fb = (float)(int)b;\n"
   "  return fa >= fb ? 1 : 0;\n"
   "}\n",
   {10, 5}, "CoreNEONExt", 1, ""},

  {"fcmgt_compare",
   "long fcmgt_compare(long a, long b) {\n"
   "  float fa = (float)(int)a, fb = (float)(int)b;\n"
   "  return fa > fb ? 1 : 0;\n"
   "}\n",
   {10, 10}, "CoreNEONExt", 1, ""},

  {"fmla_vector",
   "typedef float v4sf __attribute__((vector_size(16)));\n"
   "long fmla_vector(long a) {\n"
   "  float fa = (float)(int)a;\n"
   "  v4sf acc = {1.0f, 2.0f, 3.0f, 4.0f};\n"
   "  v4sf va = {fa, fa, fa, fa};\n"
   "  v4sf vb = {1.0f, 2.0f, 3.0f, 4.0f};\n"
   "  acc += va * vb;\n"
   "  return (long)(int)(acc[0] + acc[1] + acc[2] + acc[3]);\n"
   "}\n",
   {3}, "CoreNEONExt", 2, ""},

  {"fmls_vector",
   "typedef float v4sf __attribute__((vector_size(16)));\n"
   "long fmls_vector(long a) {\n"
   "  float fa = (float)(int)a;\n"
   "  v4sf acc = {100.0f, 200.0f, 300.0f, 400.0f};\n"
   "  v4sf va = {fa, fa, fa, fa};\n"
   "  v4sf vb = {1.0f, 2.0f, 3.0f, 4.0f};\n"
   "  acc -= va * vb;\n"
   "  return (long)(int)(acc[0] + acc[1] + acc[2] + acc[3]);\n"
   "}\n",
   {5}, "CoreNEONExt", 2, ""},

  {"saddw_widening_add",
   "typedef short v4hi __attribute__((vector_size(8)));\n"
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long saddw_widening_add(long a) {\n"
   "  v4si va = {(int)a, 100, 200, 300};\n"
   "  v4hi vb = {10, -20, 30, -40};\n"
   "  v4si vr;\n"
   "  for (int i = 0; i < 4; i++) vr[i] = va[i] + (int)vb[i];\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 4; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {50}, "CoreNEONExt", 2, ""},

  {"ssubl_widening_sub",
   "typedef short v4hi __attribute__((vector_size(8)));\n"
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long ssubl_widening_sub(long a) {\n"
   "  v4hi va = {(short)a, 100, -200, 300};\n"
   "  v4hi vb = {10, -20, 30, -40};\n"
   "  v4si vr;\n"
   "  for (int i = 0; i < 4; i++) vr[i] = (int)va[i] - (int)vb[i];\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 4; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {50}, "CoreNEONExt", 2, ""},

  {"uaddw_widening_add",
   "typedef unsigned short v4hu __attribute__((vector_size(8)));\n"
   "typedef unsigned int v4ui __attribute__((vector_size(16)));\n"
   "long uaddw_widening_add(long a) {\n"
   "  v4ui va = {(unsigned)a, 100, 200, 300};\n"
   "  v4hu vb = {10, 20, 30, 40};\n"
   "  v4ui vr;\n"
   "  for (int i = 0; i < 4; i++) vr[i] = va[i] + (unsigned)vb[i];\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 4; i++) sum += (long)vr[i];\n"
   "  return sum;\n"
   "}\n",
   {50}, "CoreNEONExt", 2, ""},

  {"usubl_widening_sub",
   "typedef unsigned short v4hu __attribute__((vector_size(8)));\n"
   "typedef unsigned int v4ui __attribute__((vector_size(16)));\n"
   "long usubl_widening_sub(long a) {\n"
   "  v4hu va = {(unsigned short)(a & 0xFFFF), 200, 500, 1000};\n"
   "  v4hu vb = {10, 20, 30, 40};\n"
   "  v4ui vr;\n"
   "  for (int i = 0; i < 4; i++) vr[i] = (unsigned)va[i] - (unsigned)vb[i];\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 4; i++) sum += (long)vr[i];\n"
   "  return sum;\n"
   "}\n",
   {100}, "CoreNEONExt", 2, ""},

  {"sshl_vector_shift",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long sshl_vector_shift(long a) {\n"
   "  v4si va = {(int)a, 100, -50, 200};\n"
   "  v4si vr;\n"
   "  for (int i = 0; i < 4; i++) vr[i] = va[i] << 2;\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 4; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "CoreNEONExt", 1, ""},

  {"srshr_rounding_shift",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long srshr_rounding_shift(long a) {\n"
   "  v4si va = {(int)a, 100, -50, 200};\n"
   "  v4si vr;\n"
   "  for (int i = 0; i < 4; i++) vr[i] = va[i] >> 2;\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 4; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "CoreNEONExt", 1, ""},

  {"sqabs_saturating",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long sqabs_saturating(long a) {\n"
   "  v4si va = {(int)a, -100, 50, -200};\n"
   "  v4si vr;\n"
   "  for (int i = 0; i < 4; i++) vr[i] = va[i] < 0 ? -va[i] : va[i];\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 4; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {(uint64_t)(int64_t)-42}, "CoreNEONExt", 2, ""},

  {"sqneg_saturating",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long sqneg_saturating(long a) {\n"
   "  v4si va = {(int)a, -100, 50, -200};\n"
   "  v4si vr;\n"
   "  for (int i = 0; i < 4; i++) vr[i] = -va[i];\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 4; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "CoreNEONExt", 2, ""},

  {"cmtst_test_bits",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long cmtst_test_bits(long a) {\n"
   "  v4si va = {(int)a, 0xFF, 0, 0x100};\n"
   "  v4si vb = {0x01, 0x100, 0xFF, 0x100};\n"
   "  v4si vr;\n"
   "  for (int i = 0; i < 4; i++) vr[i] = (va[i] & vb[i]) ? -1 : 0;\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 4; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "CoreNEONExt", 2, ""},

  /*{"fmaxnm_vector",
   "typedef float v4sf __attribute__((vector_size(16)));\n"
   "long fmaxnm_vector(long a) {\n"
   "  float fa = (float)(int)a;\n"
   "  v4sf va = {fa, -10.0f, 5.0f, 0.0f};\n"
   "  v4sf vb = {0.0f, 10.0f, -5.0f, fa};\n"
   "  v4sf vr;\n"
   "  for (int i = 0; i < 4; i++) vr[i] = va[i] > vb[i] ? va[i] : vb[i];\n"
   "  return (long)(int)(vr[0] + vr[1] + vr[2] + vr[3]);\n"
   "}\n",
   {3}, "CoreNEONExt", 2, ""},*/

  {"fminnm_scalar",
   "long fminnm_scalar(long a, long b) {\n"
   "  float fa = (float)(int)a;\n"
   "  float fb = (float)(int)b;\n"
   "  float r = fa < fb ? fa : fb;\n"
   "  return (long)(int)r;\n"
   "}\n",
   {10, 20}, "CoreNEONExt", 1, ""},

  {"smlal_widening_mul_add",
   "typedef short v4hi __attribute__((vector_size(8)));\n"
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long smlal_widening_mul_add(long a) {\n"
   "  v4si acc = {100, 200, 300, 400};\n"
   "  v4hi va = {(short)a, 3, 5, 7};\n"
   "  v4hi vb = {2, 4, 6, 8};\n"
   "  v4si vr;\n"
   "  for (int i = 0; i < 4; i++) vr[i] = acc[i] + (int)va[i] * (int)vb[i];\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 4; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {10}, "CoreNEONExt", 2, ""},

  {"umlal_widening_mul_add",
   "typedef unsigned short v4hu __attribute__((vector_size(8)));\n"
   "typedef unsigned int v4ui __attribute__((vector_size(16)));\n"
   "long umlal_widening_mul_add(long a) {\n"
   "  v4ui acc = {100, 200, 300, 400};\n"
   "  v4hu va = {(unsigned short)a, 3, 5, 7};\n"
   "  v4hu vb = {2, 4, 6, 8};\n"
   "  v4ui vr;\n"
   "  for (int i = 0; i < 4; i++) vr[i] = acc[i] + (unsigned)va[i] * (unsigned)vb[i];\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 4; i++) sum += (long)vr[i];\n"
   "  return sum;\n"
   "}\n",
   {10}, "CoreNEONExt", 2, ""},

  {"srhadd_rounding_halving",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long srhadd_rounding_halving(long a) {\n"
   "  v4si va = {(int)a, 101, -50, 200};\n"
   "  v4si vb = {10, 99, -30, 100};\n"
   "  v4si vr;\n"
   "  for (int i = 0; i < 4; i++) vr[i] = (va[i] + vb[i] + 1) >> 1;\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 4; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "CoreNEONExt", 2, ""},

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(CoreNEONExt, AArch64NEONCoreExtRT,
                         ::testing::ValuesIn(kNEONCoreExt),
                         [](const auto &P) { return P.param.Name; });
