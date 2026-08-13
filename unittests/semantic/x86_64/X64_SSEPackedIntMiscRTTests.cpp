//===- X64_SSEPackedIntMiscRTTests.cpp - SSE packed int misc roundtrip -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "SemanticRoundTripFixture.h"

class X64SSEPackedIntMiscRT
    : public SemanticRoundTripFixture,
      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SSEPackedIntMiscRT, Verify) {
  roundTripX64(GetParam());
}

// clang-format off
static const std::vector<RoundTripTC> kX64SSEPackedIntMisc = {

  {"maddubs_roundtrip",
   "#include <immintrin.h>\n"
   "long maddubs_roundtrip(long a, long b) {\n"
   "  __m128i va = _mm_set_epi8(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,(char)a);\n"
   "  __m128i vb = _mm_set_epi8(2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,(char)b);\n"
   "  __m128i r = _mm_maddubs_epi16(va, vb);\n"
   "  return (long)_mm_extract_epi16(r, 0);\n"
   "}\n",
   {3, 4}, "SSEPackedInt", 2, "-mssse3 -ffreestanding"},

  {"hadd_epi16_roundtrip",
   "#include <immintrin.h>\n"
   "long hadd_epi16_roundtrip(long a) {\n"
   "  __m128i va = _mm_set_epi16(8,7,6,5,4,3,2,(short)a);\n"
   "  __m128i vb = _mm_set_epi16(16,15,14,13,12,11,10,9);\n"
   "  __m128i r = _mm_hadd_epi16(va, vb);\n"
   "  return (long)_mm_extract_epi16(r, 0);\n"
   "}\n",
   {1}, "SSEPackedInt", 2, "-mssse3 -ffreestanding"},

  {"hsub_epi16_roundtrip",
   "#include <immintrin.h>\n"
   "long hsub_epi16_roundtrip(long a) {\n"
   "  __m128i va = _mm_set_epi16(8,7,6,5,4,3,2,(short)a);\n"
   "  __m128i vb = _mm_set_epi16(16,15,14,13,12,11,10,9);\n"
   "  __m128i r = _mm_hsub_epi16(va, vb);\n"
   "  return (long)_mm_extract_epi16(r, 0);\n"
   "}\n",
   {1}, "SSEPackedInt", 2, "-mssse3 -ffreestanding"},

  {"sad_epu8_roundtrip",
   "#include <immintrin.h>\n"
   "long sad_epu8_roundtrip(long a) {\n"
   "  __m128i va = _mm_set_epi8(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,(char)a);\n"
   "  __m128i vb = _mm_setzero_si128();\n"
   "  __m128i r = _mm_sad_epu8(va, vb);\n"
   "  return (long)_mm_extract_epi16(r, 0);\n"
   "}\n",
   {16}, "SSEPackedInt", 2, "-msse2 -ffreestanding"},

  {"abs_epi32_roundtrip",
   "#include <immintrin.h>\n"
   "long abs_epi32_roundtrip(long a) {\n"
   "  __m128i va = _mm_set_epi32(-(int)a, (int)a, -(int)(a+1), (int)(a+2));\n"
   "  __m128i r = _mm_abs_epi32(va);\n"
   "  return (long)_mm_extract_epi32(r, 0);\n"
   "}\n",
   {7}, "SSEPackedInt", 2, "-mssse3 -ffreestanding"},

  {"sign_epi32_roundtrip",
   "#include <immintrin.h>\n"
   "long sign_epi32_roundtrip(long a) {\n"
   "  __m128i va = _mm_set_epi32(-(int)a, (int)a, 0, (int)(a+1));\n"
   "  __m128i vb = _mm_set_epi32(1, -1, 1, 1);\n"
   "  __m128i r = _mm_sign_epi32(va, vb);\n"
   "  return (long)_mm_extract_epi32(r, 0);\n"
   "}\n",
   {5}, "SSEPackedInt", 2, "-mssse3 -ffreestanding"},

  {"mulhrs_epi16_roundtrip",
   "#include <immintrin.h>\n"
   "long mulhrs_epi16_roundtrip(long a) {\n"
   "  __m128i va = _mm_set_epi16(100,200,300,400,500,600,700,(short)a);\n"
   "  __m128i vb = _mm_set_epi16(100,100,100,100,100,100,100,100);\n"
   "  __m128i r = _mm_mulhrs_epi16(va, vb);\n"
   "  return (long)_mm_extract_epi16(r, 0);\n"
   "}\n",
   {1000}, "SSEPackedInt", 2, "-mssse3 -ffreestanding"},

  {"movmskps_roundtrip",
   "#include <immintrin.h>\n"
   "long movmskps_roundtrip(long a) {\n"
   "  float fa = -(float)a;\n"
   "  __m128 v = _mm_set_ps(fa, (float)a, fa, (float)a);\n"
   "  return (long)_mm_movemask_ps(v);\n"
   "}\n",
   {5}, "SSEPackedInt", 2, "-msse -ffreestanding"},

  {"movmskpd_roundtrip",
   "#include <immintrin.h>\n"
   "long movmskpd_roundtrip(long a) {\n"
   "  double da = -(double)a;\n"
   "  __m128d v = _mm_set_pd(da, (double)a);\n"
   "  return (long)_mm_movemask_pd(v);\n"
   "}\n",
   {5}, "SSEPackedInt", 2, "-msse2 -ffreestanding"},

  {"cmplt_ps_roundtrip",
   "#include <immintrin.h>\n"
   "long cmplt_ps_roundtrip(long a) {\n"
   "  __m128 va = _mm_set_ps(1.0f, 2.0f, 3.0f, (float)a);\n"
   "  __m128 vb = _mm_set_ps(4.0f, 1.0f, 5.0f, 2.0f);\n"
   "  __m128 cmp = _mm_cmplt_ps(va, vb);\n"
   "  return (long)_mm_movemask_ps(cmp);\n"
   "}\n",
   {1}, "SSEPackedInt", 2, "-msse -ffreestanding"},

  {"roundps_roundtrip",
   "#include <immintrin.h>\n"
   "long roundps_roundtrip(long a) {\n"
   "  __m128 v = _mm_set_ps(1.7f, 2.3f, 3.5f, (float)a + 0.6f);\n"
   "  __m128 r = _mm_round_ps(v, _MM_FROUND_TO_NEAREST_INT);\n"
   "  return (long)_mm_cvtss_f32(r);\n"
   "}\n",
   {3}, "SSEPackedInt", 2, "-msse4.1 -ffreestanding"},

  {"blendv_epi8_roundtrip",
   "#include <immintrin.h>\n"
   "long blendv_epi8_roundtrip(long a) {\n"
   "  __m128i va = _mm_set_epi32((int)a, (int)(a+1), (int)(a+2), (int)(a+3));\n"
   "  __m128i vb = _mm_set_epi32(100, 200, 300, 400);\n"
   "  __m128i mask = _mm_set_epi32(-1, 0, -1, 0);\n"
   "  __m128i r = _mm_blendv_epi8(va, vb, mask);\n"
   "  return (long)_mm_extract_epi32(r, 0);\n"
   "}\n",
   {10}, "SSEPackedInt", 2, "-msse4.1 -ffreestanding"},

};
// clang-format on

INSTANTIATE_TEST_SUITE_P(SSEPackedInt, X64SSEPackedIntMiscRT,
                         ::testing::ValuesIn(kX64SSEPackedIntMisc),
                         [](const auto &P) { return P.param.Name; });
