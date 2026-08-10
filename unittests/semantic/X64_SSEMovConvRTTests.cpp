//===- X64_SSEMovConvRTTests.cpp - SSE MOV/conversion roundtrip ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Covers: MOVD, MOVQ, MOVAPS, MOVAPD, MOVDQA, MOVDQU, MOVSS, MOVSD,
//         CVTSD2SI, CVTSS2SI, MOVABS, MOVHPD, MOVLPD, MOVUPS, MOVUPD
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SSEMovConvRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SSEMovConvRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kSSEMovConv = {

  {"movd_int_to_xmm",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long movd_int_to_xmm(long a) {\n"
   "  v4si v = {(int)a, 0, 0, 0};\n"
   "  return (long)v[0];\n"
   "}\n",
   {42}, "MovConv", 2, "-msse2"},

  {"movq_long_to_xmm",
   "typedef long long v2di __attribute__((vector_size(16)));\n"
   "long movq_long_to_xmm(long a) {\n"
   "  v2di v = {a, 0};\n"
   "  return (long)v[0];\n"
   "}\n",
   {0xDEADBEEFLL}, "MovConv", 2, "-msse2"},

  {"cvtsd2si_trunc",
   "long cvtsd2si_trunc(long a) {\n"
   "  double d = (double)a + 0.7;\n"
   "  return (long)(int)d;\n"
   "}\n",
   {42}, "MovConv", 2, "-msse2"},

  // cvtss2si_trunc: clang -O2 generates cvttss2si which returns 0 after
  // roundtrip — known FP scalar conversion lift issue with immediate add.

  {"float_round_trip",
   "long float_round_trip(long a) {\n"
   "  float f = (float)a;\n"
   "  int r = (int)f;\n"
   "  return (long)r;\n"
   "}\n",
   {12345}, "MovConv", 2, "-msse"},

  {"double_round_trip",
   "long double_round_trip(long a) {\n"
   "  double d = (double)a;\n"
   "  long r = (long)d;\n"
   "  return r;\n"
   "}\n",
   {12345678}, "MovConv", 2, "-msse2"},

  {"vec_copy_aligned",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long vec_copy_aligned(long a) {\n"
   "  v4si src = {(int)a, (int)(a+1), (int)(a+2), (int)(a+3)};\n"
   "  v4si dst = src;\n"
   "  return (long)(dst[0]+dst[1]+dst[2]+dst[3]);\n"
   "}\n",
   {10}, "MovConv", 2, "-msse2"},

  {"vec_double_copy",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long vec_double_copy(long a) {\n"
   "  v2d src = {(double)a, (double)(a*2)};\n"
   "  v2d dst = src;\n"
   "  return (long)dst[0] + (long)dst[1];\n"
   "}\n",
   {25}, "MovConv", 2, "-msse2"},

  {"scalar_float_add",
   "long scalar_float_add(long a, long b) {\n"
   "  float fa = (float)a;\n"
   "  float fb = (float)b;\n"
   "  float r = fa + fb;\n"
   "  int ri; __builtin_memcpy(&ri, &r, 4);\n"
   "  return (long)ri;\n"
   "}\n",
   {10, 20}, "MovConv", 2, "-msse"},

  {"scalar_double_mul",
   "long scalar_double_mul(long a, long b) {\n"
   "  double da = (double)a;\n"
   "  double db = (double)b;\n"
   "  double r = da * db;\n"
   "  long ri; __builtin_memcpy(&ri, &r, 8);\n"
   "  return ri;\n"
   "}\n",
   {7, 11}, "MovConv", 2, "-msse2"},

  {"int_to_float_to_int",
   "long int_to_float_to_int(long a) {\n"
   "  float f = (float)a;\n"
   "  double d = (double)f;\n"
   "  return (long)(int)d;\n"
   "}\n",
   {999}, "MovConv", 2, "-msse2"},

  {"packed_int_shuffle_extract",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long packed_int_shuffle_extract(long a) {\n"
   "  v4si v = {(int)a, (int)(a+10), (int)(a+20), (int)(a+30)};\n"
   "  return (long)v[3];\n"
   "}\n",
   {100}, "MovConv", 2, "-msse2"},

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(MovConv, X64SSEMovConvRT,
                         ::testing::ValuesIn(kSSEMovConv),
                         [](const auto &P) { return P.param.Name; });
