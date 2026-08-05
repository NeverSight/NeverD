//===- ARM32_NEONExtOpsRTTests.cpp - ARM32 NEON extended ops roundtrip ----===//
//
// Covers: VABA, VABAL, VADDW, VSUBW, VBIC, VBIF, VBIT, VCGE, VCLE,
//         VMLA (int), VMLS (int), VMLAL, VMLSL, VQDMULH, VQRDMULH,
//         VQSHL, VRSHR, VSRA, VRSRA, VHSUB, VRHADD, VPADDL,
//         VSUBL, VMULL (poly), VQDMLAL, VCNT (int), VCLS
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32NEONExtOpsRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32NEONExtOpsRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kNEONExtOps = {

  {"vaba_abs_diff_acc",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long vaba_abs_diff_acc(long a) {\n"
   "  v4si acc = {100, 200, 300, 400};\n"
   "  v4si va = {(int)a, 50, 80, 10};\n"
   "  v4si vb = {10, (int)a, 20, 90};\n"
   "  v4si vr;\n"
   "  for (int i = 0; i < 4; i++) {\n"
   "    int d = va[i] - vb[i];\n"
   "    vr[i] = acc[i] + (d < 0 ? -d : d);\n"
   "  }\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 4; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "NEONExtOps", 2, "-mfpu=neon"},

  {"vaddw_widening",
   "typedef short v4hi __attribute__((vector_size(8)));\n"
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long vaddw_widening(long a) {\n"
   "  v4si va = {(int)a, 100, 200, 300};\n"
   "  v4hi vb = {5, 10, 15, 20};\n"
   "  v4si vr;\n"
   "  for (int i = 0; i < 4; i++) vr[i] = va[i] + (int)vb[i];\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 4; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {50}, "NEONExtOps", 2, "-mfpu=neon"},

  {"vsubw_widening",
   "typedef short v4hi __attribute__((vector_size(8)));\n"
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long vsubw_widening(long a) {\n"
   "  v4si va = {(int)a + 100, 200, 300, 400};\n"
   "  v4hi vb = {5, 10, 15, 20};\n"
   "  v4si vr;\n"
   "  for (int i = 0; i < 4; i++) vr[i] = va[i] - (int)vb[i];\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 4; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {50}, "NEONExtOps", 2, "-mfpu=neon"},

  {"vbic_bitclear",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long vbic_bitclear(long a) {\n"
   "  v4si va = {(int)a, -1, 0xFF00FF, 0x12345678};\n"
   "  v4si vb = {0x0F, 0xFF, 0xFF00, 0xF0F0F0F0};\n"
   "  v4si vr;\n"
   "  for (int i = 0; i < 4; i++) vr[i] = va[i] & ~vb[i];\n"
   "  return (long)(unsigned)vr[0] + (long)(unsigned)vr[3];\n"
   "}\n",
   {0xFF}, "NEONExtOps", 1, "-mfpu=neon"},

  {"vcge_compare",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long vcge_compare(long a) {\n"
   "  v4si va = {(int)a, 50, 100, 0};\n"
   "  v4si vb = {50, (int)a, 0, 100};\n"
   "  v4si vr;\n"
   "  for (int i = 0; i < 4; i++) vr[i] = va[i] >= vb[i] ? -1 : 0;\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 4; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {50}, "NEONExtOps", 2, "-mfpu=neon"},

  {"vmla_int_mul_acc",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long vmla_int_mul_acc(long a) {\n"
   "  v4si acc = {10, 20, 30, 40};\n"
   "  v4si va = {(int)a, 3, 5, 7};\n"
   "  v4si vb = {2, 4, 6, 8};\n"
   "  v4si vr;\n"
   "  for (int i = 0; i < 4; i++) vr[i] = acc[i] + va[i] * vb[i];\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 4; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {5}, "NEONExtOps", 2, "-mfpu=neon"},

  {"vmls_int_mul_sub",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long vmls_int_mul_sub(long a) {\n"
   "  v4si acc = {100, 200, 300, 400};\n"
   "  v4si va = {(int)a, 3, 5, 7};\n"
   "  v4si vb = {2, 4, 6, 8};\n"
   "  v4si vr;\n"
   "  for (int i = 0; i < 4; i++) vr[i] = acc[i] - va[i] * vb[i];\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 4; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {5}, "NEONExtOps", 2, "-mfpu=neon"},

  {"vmlal_widening_mul_add",
   "typedef short v4hi __attribute__((vector_size(8)));\n"
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long vmlal_widening_mul_add(long a) {\n"
   "  v4si acc = {100, 200, 300, 400};\n"
   "  v4hi va = {(short)a, 3, 5, 7};\n"
   "  v4hi vb = {2, 4, 6, 8};\n"
   "  v4si vr;\n"
   "  for (int i = 0; i < 4; i++) vr[i] = acc[i] + (int)va[i] * (int)vb[i];\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 4; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {10}, "NEONExtOps", 2, "-mfpu=neon"},

  {"vmlsl_widening_mul_sub",
   "typedef short v4hi __attribute__((vector_size(8)));\n"
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long vmlsl_widening_mul_sub(long a) {\n"
   "  v4si acc = {1000, 2000, 3000, 4000};\n"
   "  v4hi va = {(short)a, 3, 5, 7};\n"
   "  v4hi vb = {2, 4, 6, 8};\n"
   "  v4si vr;\n"
   "  for (int i = 0; i < 4; i++) vr[i] = acc[i] - (int)va[i] * (int)vb[i];\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 4; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {10}, "NEONExtOps", 2, "-mfpu=neon"},

  {"vsubl_widening_sub",
   "typedef short v4hi __attribute__((vector_size(8)));\n"
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long vsubl_widening_sub(long a) {\n"
   "  v4hi va = {(short)a, 100, -50, 200};\n"
   "  v4hi vb = {10, -20, 30, -40};\n"
   "  v4si vr;\n"
   "  for (int i = 0; i < 4; i++) vr[i] = (int)va[i] - (int)vb[i];\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 4; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {50}, "NEONExtOps", 2, "-mfpu=neon"},

  {"vrshr_rounding_shift",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long vrshr_rounding_shift(long a) {\n"
   "  v4si va = {(int)a, 100, -50, 200};\n"
   "  v4si vr;\n"
   "  for (int i = 0; i < 4; i++) vr[i] = va[i] >> 2;\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 4; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "NEONExtOps", 1, "-mfpu=neon"},

  {"vrhadd_rounding_halving",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long vrhadd_rounding_halving(long a) {\n"
   "  v4si va = {(int)a, 101, 50, 200};\n"
   "  v4si vb = {10, 99, 30, 100};\n"
   "  v4si vr;\n"
   "  for (int i = 0; i < 4; i++) vr[i] = (va[i] + vb[i] + 1) >> 1;\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 4; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "NEONExtOps", 2, "-mfpu=neon"},

  {"vhsub_halving_sub",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long vhsub_halving_sub(long a) {\n"
   "  v4si va = {(int)a, 100, 200, 300};\n"
   "  v4si vb = {10, 20, 30, 40};\n"
   "  v4si vr;\n"
   "  for (int i = 0; i < 4; i++) vr[i] = (va[i] - vb[i]) >> 1;\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 4; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "NEONExtOps", 2, "-mfpu=neon"},

  {"vqshl_saturating_shift",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long vqshl_saturating_shift(long a) {\n"
   "  v4si va = {(int)a, 100, -50, 200};\n"
   "  v4si vr;\n"
   "  for (int i = 0; i < 4; i++) vr[i] = va[i] << 3;\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 4; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "NEONExtOps", 1, "-mfpu=neon"},

  {"vsra_shift_accumulate",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long vsra_shift_accumulate(long a) {\n"
   "  v4si acc = {100, 200, 300, 400};\n"
   "  v4si va = {(int)a * 4, 400, -200, 800};\n"
   "  v4si vr;\n"
   "  for (int i = 0; i < 4; i++) vr[i] = acc[i] + (va[i] >> 2);\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 4; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "NEONExtOps", 2, "-mfpu=neon"},

  {"vpaddl_pairwise_add_long",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long vpaddl_pairwise_add_long(long a) {\n"
   "  v8hi va = {(short)a, 10, 20, 30, 40, 50, 60, 70};\n"
   "  v4si vr;\n"
   "  for (int i = 0; i < 4; i++) vr[i] = (int)va[2*i] + (int)va[2*i+1];\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 4; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "NEONExtOps", 2, "-mfpu=neon"},

  {"vcls_leading_sign",
   "long vcls_leading_sign(long a) {\n"
   "  int x = (int)a;\n"
   "  if (x < 0) x = ~x;\n"
   "  int r = 0;\n"
   "  if (x == 0) return 31;\n"
   "  while ((x & 0x40000000) == 0) { x <<= 1; r++; }\n"
   "  return (long)r;\n"
   "}\n",
   {0xFF}, "NEONExtOps", 1, ""},

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(NEONExtOps, ARM32NEONExtOpsRT,
                         ::testing::ValuesIn(kNEONExtOps),
                         [](const auto &P) { return P.param.Name; });
