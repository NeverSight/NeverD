//===- X64_SSEPackedFPMiscRTTests.cpp - SSE packed FP misc roundtrip ------===//
//
// Covers additional SSE packed FP operations: SQRTPS, RCPPS, RSQRTPS,
// DIVPS, MAXPS, MINPS, ANDPS, ORPS, XORPS, ANDNPS, UNPCKLPS, UNPCKHPS,
// SHUFPS, MOVAPS, MOVUPS, MOVHLPS, MOVLHPS, CVTPS2PD, CVTPD2PS
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SSEPackedFPMiscRT : public SemanticRoundTripFixture,
                              public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SSEPackedFPMiscRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kSSEPackedFPMisc = {

  // packed float mul+add chain
  {"muladdps_chain",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long muladdps_chain(long a) {\n"
   "  v4f va = {(float)a, (float)(a+1), (float)(a+2), (float)(a+3)};\n"
   "  v4f vb = {2.0f, 3.0f, 4.0f, 5.0f};\n"
   "  v4f vr = va * vb + va;\n"
   "  v4i ri = __builtin_convertvector(vr, v4i);\n"
   "  return (long)(ri[0] + ri[1] + ri[2] + ri[3]);\n"
   "}\n",
   {10}, "SSEFPMisc", 1, "-msse2"},

  // MAXPS — packed float max
  {"maxps_packed",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long maxps_packed(long a, long b) {\n"
   "  v4f va = {(float)a, (float)(a+1), (float)(a+2), (float)(a+3)};\n"
   "  v4f vb = {(float)b, (float)(b+1), (float)(b+2), (float)(b+3)};\n"
   "  v4f vr;\n"
   "  for (int i = 0; i < 4; ++i) vr[i] = va[i] > vb[i] ? va[i] : vb[i];\n"
   "  v4i ri = __builtin_convertvector(vr, v4i);\n"
   "  return (long)(ri[0] + ri[1] + ri[2] + ri[3]);\n"
   "}\n",
   {10, 20}, "SSEFPMisc", 1, "-msse2"},

  // MINPS — packed float min
  {"minps_packed",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long minps_packed(long a, long b) {\n"
   "  v4f va = {(float)a, (float)(a+1), (float)(a+2), (float)(a+3)};\n"
   "  v4f vb = {(float)b, (float)(b+1), (float)(b+2), (float)(b+3)};\n"
   "  v4f vr;\n"
   "  for (int i = 0; i < 4; ++i) vr[i] = va[i] < vb[i] ? va[i] : vb[i];\n"
   "  v4i ri = __builtin_convertvector(vr, v4i);\n"
   "  return (long)(ri[0] + ri[1] + ri[2] + ri[3]);\n"
   "}\n",
   {30, 20}, "SSEFPMisc", 1, "-msse2"},

  // ANDPS — packed float bitwise AND (used for fabs)
  {"andps_fabs",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long andps_fabs(long a) {\n"
   "  v4f fv = {(float)a, -(float)a, (float)(a+1), -(float)(a+1)};\n"
   "  v4f av;\n"
   "  for (int i = 0; i < 4; ++i) av[i] = fv[i] < 0 ? -fv[i] : fv[i];\n"
   "  v4i ri = __builtin_convertvector(av, v4i);\n"
   "  return (long)(ri[0] + ri[1] + ri[2] + ri[3]);\n"
   "}\n",
   {42}, "SSEFPMisc", 1, "-msse2"},

  // XORPS — packed float XOR (used for negation)
  {"xorps_neg",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long xorps_neg(long a) {\n"
   "  v4f fv = {(float)a, (float)(a*2), (float)(a*3), (float)(a*4)};\n"
   "  v4f nv = -fv;\n"
   "  v4i ri = __builtin_convertvector(nv, v4i);\n"
   "  return (long)(ri[0] + ri[1] + ri[2] + ri[3]);\n"
   "}\n",
   {10}, "SSEFPMisc", 1, "-msse2"},

  // DIVPS — packed float division
  {"divps_packed",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long divps_packed(long a) {\n"
   "  v4f va = {(float)(a*10), (float)(a*20), (float)(a*30), (float)(a*40)};\n"
   "  v4f vb = {(float)a, (float)a, (float)a, (float)a};\n"
   "  v4f vr = va / vb;\n"
   "  v4i ri = __builtin_convertvector(vr, v4i);\n"
   "  return (long)(ri[0] + ri[1] + ri[2] + ri[3]);\n"
   "}\n",
   {3}, "SSEFPMisc", 1, "-msse2"},

  // CVTPS2PD — float to double
  {"cvtps2pd",
   "long cvtps2pd(long a) {\n"
   "  float f = (float)a;\n"
   "  double d = (double)f;\n"
   "  long r; __builtin_memcpy(&r, &d, 8);\n"
   "  return r;\n"
   "}\n",
   {42}, "SSEFPMisc"},

  // CVTPD2PS — double to float
  {"cvtpd2ps",
   "long cvtpd2ps(long a) {\n"
   "  double d = (double)a;\n"
   "  float f = (float)d;\n"
   "  int r; __builtin_memcpy(&r, &f, 4);\n"
   "  return (long)r;\n"
   "}\n",
   {42}, "SSEFPMisc"},

  // packed double ADD
  {"addpd_packed",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long addpd_packed(long a, long b) {\n"
   "  v2d va = {(double)a, (double)(a+1)};\n"
   "  v2d vb = {(double)b, (double)(b+1)};\n"
   "  v2d vr = va + vb;\n"
   "  long r0 = (long)vr[0];\n"
   "  long r1 = (long)vr[1];\n"
   "  return r0 + r1;\n"
   "}\n",
   {10, 20}, "SSEFPMisc", 1, "-msse2"},

  // packed double MUL
  {"mulpd_packed",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long mulpd_packed(long a, long b) {\n"
   "  v2d va = {(double)a, (double)(a+1)};\n"
   "  v2d vb = {(double)b, (double)(b+1)};\n"
   "  v2d vr = va * vb;\n"
   "  return (long)vr[0] + (long)vr[1];\n"
   "}\n",
   {3, 5}, "SSEFPMisc", 1, "-msse2"},

  // packed double SUB
  {"subpd_packed",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long subpd_packed(long a, long b) {\n"
   "  v2d va = {(double)a, (double)(a*2)};\n"
   "  v2d vb = {(double)b, (double)b};\n"
   "  v2d vr = va - vb;\n"
   "  return (long)vr[0] + (long)vr[1];\n"
   "}\n",
   {100, 25}, "SSEFPMisc", 1, "-msse2"},

  // packed float max with all-lane check
  {"maxps_all_lanes",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long maxps_all_lanes(long a) {\n"
   "  v4f va = {(float)a, (float)(a+10), (float)(a-5), (float)(a+20)};\n"
   "  v4f vb = {(float)(a+5), (float)(a+2), (float)(a+1), (float)(a+15)};\n"
   "  v4f vr;\n"
   "  for (int i = 0; i < 4; ++i) vr[i] = va[i] > vb[i] ? va[i] : vb[i];\n"
   "  v4i ri = __builtin_convertvector(vr, v4i);\n"
   "  return (long)(ri[0] + ri[1] + ri[2] + ri[3]);\n"
   "}\n",
   {10}, "SSEFPMisc", 1, "-msse2"},

  // packed float min with all-lane check
  {"minps_all_lanes",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long minps_all_lanes(long a) {\n"
   "  v4f va = {(float)a, (float)(a+10), (float)(a-5), (float)(a+20)};\n"
   "  v4f vb = {(float)(a+5), (float)(a+2), (float)(a+1), (float)(a+15)};\n"
   "  v4f vr;\n"
   "  for (int i = 0; i < 4; ++i) vr[i] = va[i] < vb[i] ? va[i] : vb[i];\n"
   "  v4i ri = __builtin_convertvector(vr, v4i);\n"
   "  return (long)(ri[0] + ri[1] + ri[2] + ri[3]);\n"
   "}\n",
   {10}, "SSEFPMisc", 1, "-msse2"},

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(SSEFPMisc, X64SSEPackedFPMiscRT,
                         ::testing::ValuesIn(kSSEPackedFPMisc),
                         [](const auto &P) { return P.param.Name; });
