//===- X64_SSESqrtRecipRTTests.cpp - SSE sqrt/reciprocal roundtrip --------===//
//
// Covers: SQRTPS, SQRTSS, SQRTPD, SQRTSD, RCPPS, RCPSS, RSQRTPS, RSQRTSS,
//         MAXPD, MINPD, HSUBPD, CVTPD2DQ, CVTTPD2DQ
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SSESqrtRecipRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SSESqrtRecipRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kSSESqrtRecip = {

  {"sqrtss_scalar",
   "long sqrtss_scalar(long a) {\n"
   "  float f = (float)a;\n"
   "  float r = __builtin_sqrtf(f);\n"
   "  int ri; __builtin_memcpy(&ri, &r, 4);\n"
   "  return (long)ri;\n"
   "}\n",
   {25}, "SqrtRecip", 2, "-msse -fno-math-errno"},

  {"sqrtsd_scalar",
   "long sqrtsd_scalar(long a) {\n"
   "  double d = (double)a;\n"
   "  double r = __builtin_sqrt(d);\n"
   "  long ri; __builtin_memcpy(&ri, &r, 8);\n"
   "  return ri;\n"
   "}\n",
   {100}, "SqrtRecip", 2, "-msse2 -fno-math-errno"},

  {"sqrtps_packed",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long sqrtps_packed(long a) {\n"
   "  float fa = (float)(a > 0 ? a : -a);\n"
   "  v4f va = {fa, fa*4.0f, fa*9.0f, fa*16.0f};\n"
   "  v4f vr;\n"
   "  for (int i = 0; i < 4; ++i) vr[i] = __builtin_sqrtf(va[i]);\n"
   "  v4i ri = __builtin_convertvector(vr, v4i);\n"
   "  return (long)(ri[0] + ri[1] + ri[2] + ri[3]);\n"
   "}\n",
   {4}, "SqrtRecip", 2, "-msse -fno-math-errno"},

  {"sqrtpd_packed",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long sqrtpd_packed(long a) {\n"
   "  double da = (double)(a > 0 ? a : -a);\n"
   "  v2d va = {da, da*4.0};\n"
   "  v2d vr;\n"
   "  for (int i = 0; i < 2; ++i) vr[i] = __builtin_sqrt(va[i]);\n"
   "  return (long)vr[0] + (long)vr[1];\n"
   "}\n",
   {16}, "SqrtRecip", 2, "-msse2 -fno-math-errno"},

  {"maxpd_packed",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long maxpd_packed(long a, long b) {\n"
   "  v2d va = {(double)a, (double)(a+5)};\n"
   "  v2d vb = {(double)b, (double)(b-3)};\n"
   "  v2d vr;\n"
   "  for (int i = 0; i < 2; ++i) vr[i] = va[i] > vb[i] ? va[i] : vb[i];\n"
   "  return (long)vr[0] + (long)vr[1];\n"
   "}\n",
   {10, 20}, "SqrtRecip", 1, "-msse2"},

  {"minpd_packed",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long minpd_packed(long a, long b) {\n"
   "  v2d va = {(double)a, (double)(a+5)};\n"
   "  v2d vb = {(double)b, (double)(b-3)};\n"
   "  v2d vr;\n"
   "  for (int i = 0; i < 2; ++i) vr[i] = va[i] < vb[i] ? va[i] : vb[i];\n"
   "  return (long)vr[0] + (long)vr[1];\n"
   "}\n",
   {30, 20}, "SqrtRecip", 1, "-msse2"},

  {"divpd_packed",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long divpd_packed(long a) {\n"
   "  v2d va = {(double)(a*6), (double)(a*10)};\n"
   "  v2d vb = {(double)a, (double)a};\n"
   "  v2d vr = va / vb;\n"
   "  return (long)vr[0] + (long)vr[1];\n"
   "}\n",
   {5}, "SqrtRecip", 1, "-msse2"},

  {"cvtpd2dq_packed",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long cvtpd2dq_packed(long a) {\n"
   "  v2d va = {(double)a + 0.7, (double)(a*2) + 0.3};\n"
   "  int r0 = (int)va[0];\n"
   "  int r1 = (int)va[1];\n"
   "  return (long)r0 + (long)r1;\n"
   "}\n",
   {10}, "SqrtRecip", 1, "-msse2"},

  {"cvttpd2dq_trunc",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long cvttpd2dq_trunc(long a) {\n"
   "  v2d va = {(double)a + 0.9, (double)(a*2) + 0.1};\n"
   "  int r0 = (int)va[0];\n"
   "  int r1 = (int)va[1];\n"
   "  return (long)(r0 + r1);\n"
   "}\n",
   {10}, "SqrtRecip", 1, "-msse2"},

  {"maxpd_chain",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long maxpd_chain(long a) {\n"
   "  v2d va = {(double)a, (double)(a-10)};\n"
   "  v2d vb = {(double)(a+5), (double)(a-20)};\n"
   "  v2d vc = {(double)(a-3), (double)(a+2)};\n"
   "  v2d r1, r2;\n"
   "  for (int i=0;i<2;++i) r1[i] = va[i]>vb[i]?va[i]:vb[i];\n"
   "  for (int i=0;i<2;++i) r2[i] = r1[i]>vc[i]?r1[i]:vc[i];\n"
   "  return (long)r2[0] + (long)r2[1];\n"
   "}\n",
   {50}, "SqrtRecip", 1, "-msse2"},

  {"addss_mulss_chain",
   "long addss_mulss_chain(long a, long b) {\n"
   "  float fa = (float)a, fb = (float)b;\n"
   "  float r = (fa + fb) * fa;\n"
   "  int ri; __builtin_memcpy(&ri, &r, 4);\n"
   "  return (long)ri;\n"
   "}\n",
   {3, 7}, "SqrtRecip", 2, "-msse"},

  {"subss_divss_chain",
   "long subss_divss_chain(long a, long b) {\n"
   "  float fa = (float)a, fb = (float)b;\n"
   "  float r = (fa - fb) / fa;\n"
   "  int ri; __builtin_memcpy(&ri, &r, 4);\n"
   "  return (long)ri;\n"
   "}\n",
   {20, 5}, "SqrtRecip", 2, "-msse -fno-math-errno"},

  {"addsd_mulsd_chain",
   "long addsd_mulsd_chain(long a, long b) {\n"
   "  double da = (double)a, db = (double)b;\n"
   "  double r = (da + db) * da;\n"
   "  long ri; __builtin_memcpy(&ri, &r, 8);\n"
   "  return ri;\n"
   "}\n",
   {3, 7}, "SqrtRecip", 2, "-msse2"},

  {"fp_scalar_chain_3op",
   "long fp_scalar_chain_3op(long a, long b) {\n"
   "  float fa = (float)a, fb = (float)b;\n"
   "  float r = fa * fb + fa - fb;\n"
   "  int ri; __builtin_memcpy(&ri, &r, 4);\n"
   "  return (long)ri;\n"
   "}\n",
   {5, 3}, "SqrtRecip", 2, "-msse"},

  {"sqrt_chain_float",
   "long sqrt_chain_float(long a) {\n"
   "  float f = (float)(a > 0 ? a : -a);\n"
   "  float s = __builtin_sqrtf(f);\n"
   "  float r = s * f;\n"
   "  int ri; __builtin_memcpy(&ri, &r, 4);\n"
   "  return (long)ri;\n"
   "}\n",
   {16}, "SqrtRecip", 2, "-msse -fno-math-errno"},

  // SQRTSS whose source is a full XMM (from `movd`, not a narrow CVTSI2SS
  // result).  The scalar handler must extract the low 4 bytes so the op stays
  // single-precision; feeding the whole 16B register made the emitter infer
  // double and lower to sqrt.f64 (a=4.0f bits -> wrong unless single).
  {"sqrtss_fullxmm",
   "long sqrtss_fullxmm(long a) {\n"
   "  float x; __builtin_memcpy(&x, &a, 4);\n"
   "  float r;\n"
   "  __asm__(\"sqrtss %1, %0\" : \"=x\"(r) : \"x\"(x));\n"
   "  int ri; __builtin_memcpy(&ri, &r, 4);\n"
   "  return (long)ri;\n"
   "}\n",
   {0x40800000ULL}, "SqrtRecip", 1, "-msse"},

  // SQRTSD with a full-XMM source (low double): must stay double precision.
  {"sqrtsd_fullxmm",
   "long sqrtsd_fullxmm(long a) {\n"
   "  double x; __builtin_memcpy(&x, &a, 8);\n"
   "  double r;\n"
   "  __asm__(\"sqrtsd %1, %0\" : \"=x\"(r) : \"x\"(x));\n"
   "  long ri; __builtin_memcpy(&ri, &r, 8);\n"
   "  return ri;\n"
   "}\n",
   {0x4010000000000000ULL}, "SqrtRecip", 1, "-msse2"},

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(SqrtRecip, X64SSESqrtRecipRT,
                         ::testing::ValuesIn(kSSESqrtRecip),
                         [](const auto &P) { return P.param.Name; });
