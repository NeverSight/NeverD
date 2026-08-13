//===- X64_SSESatPackRTTests.cpp - x86 SSE saturating / pack roundtrip -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip tests for x86 SSE/SSE2/SSSE3/SSE4.1 saturating arithmetic, pack
// (saturating narrow) and saturating horizontal instructions, exercised with
// edge values that force saturation (overflow / negative) on every lane:
//   PACKSSWB/PACKSSDW/PACKUSWB/PACKUSDW, PADDS{B,W}/PADDUS{B,W},
//   PSUBS{B,W}/PSUBUS{B,W}, PHADDSW/PHSUBSW.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SSESatPackRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SSESatPackRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kSatPack = {
  {"packsswb",
   "#include <immintrin.h>\n"
   "long packsswb(long a, long b) {\n"
   "  __m128i va = _mm_setr_epi16((short)a, 300, -300, 32767, -32768, 100, -100, 0);\n"
   "  __m128i vb = _mm_setr_epi16((short)b, 200, -200, 1000, -1000, 5, -5, 127);\n"
   "  __m128i r = _mm_packs_epi16(va, vb);\n"
   "  signed char o[16]; _mm_storeu_si128((__m128i*)o, r);\n"
   "  int s = 0; for (int i = 0; i < 16; i++) s = s * 31 + o[i];\n"
   "  return (long)(unsigned)s;\n"
   "}\n",
   {500, (uint64_t)-500}, "SatPack", 1, "-msse4.1 -ffreestanding"},

  {"packssdw",
   "#include <immintrin.h>\n"
   "long packssdw(long a, long b) {\n"
   "  __m128i va = _mm_setr_epi32((int)a, 100000, -100000, 32767);\n"
   "  __m128i vb = _mm_setr_epi32((int)b, -40000, 40000, -32768);\n"
   "  __m128i r = _mm_packs_epi32(va, vb);\n"
   "  short o[8]; _mm_storeu_si128((__m128i*)o, r);\n"
   "  int s = 0; for (int i = 0; i < 8; i++) s = s * 31 + o[i];\n"
   "  return (long)(unsigned)s;\n"
   "}\n",
   {70000, (uint64_t)-70000}, "SatPack", 1, "-msse4.1 -ffreestanding"},

  {"packuswb",
   "#include <immintrin.h>\n"
   "long packuswb(long a, long b) {\n"
   "  __m128i va = _mm_setr_epi16((short)a, 300, -300, 255, -1, 100, 256, 0);\n"
   "  __m128i vb = _mm_setr_epi16((short)b, 200, -200, 1000, -1000, 5, -5, 254);\n"
   "  __m128i r = _mm_packus_epi16(va, vb);\n"
   "  unsigned char o[16]; _mm_storeu_si128((__m128i*)o, r);\n"
   "  int s = 0; for (int i = 0; i < 16; i++) s = s * 31 + o[i];\n"
   "  return (long)(unsigned)s;\n"
   "}\n",
   {500, (uint64_t)-7}, "SatPack", 1, "-msse4.1 -ffreestanding"},

  {"packusdw",
   "#include <immintrin.h>\n"
   "long packusdw(long a, long b) {\n"
   "  __m128i va = _mm_setr_epi32((int)a, 100000, -5, 65535);\n"
   "  __m128i vb = _mm_setr_epi32((int)b, -40000, 70000, 65536);\n"
   "  __m128i r = _mm_packus_epi32(va, vb);\n"
   "  unsigned short o[8]; _mm_storeu_si128((__m128i*)o, r);\n"
   "  int s = 0; for (int i = 0; i < 8; i++) s = s * 31 + o[i];\n"
   "  return (long)(unsigned)s;\n"
   "}\n",
   {90000, (uint64_t)-3}, "SatPack", 1, "-msse4.1 -ffreestanding"},

  {"paddsb",
   "#include <immintrin.h>\n"
   "long paddsb(long a, long b) {\n"
   "  __m128i va = _mm_setr_epi8((char)a,100,-100,127,-128,1,2,3,4,5,6,7,8,9,10,11);\n"
   "  __m128i vb = _mm_setr_epi8((char)b,100,-100,1,-1,5,6,7,8,9,10,11,12,13,14,15);\n"
   "  __m128i r = _mm_adds_epi8(va, vb);\n"
   "  signed char o[16]; _mm_storeu_si128((__m128i*)o, r);\n"
   "  int s = 0; for (int i = 0; i < 16; i++) s = s * 31 + o[i];\n"
   "  return (long)(unsigned)s;\n"
   "}\n",
   {120, 120}, "SatPack", 1, "-msse4.1 -ffreestanding"},

  {"paddsw",
   "#include <immintrin.h>\n"
   "long paddsw(long a, long b) {\n"
   "  __m128i va = _mm_setr_epi16((short)a, 30000, -30000, 32767, -32768, 1, 2, 3);\n"
   "  __m128i vb = _mm_setr_epi16((short)b, 30000, -30000, 1, -1, 5, 6, 7);\n"
   "  __m128i r = _mm_adds_epi16(va, vb);\n"
   "  short o[8]; _mm_storeu_si128((__m128i*)o, r);\n"
   "  int s = 0; for (int i = 0; i < 8; i++) s = s * 31 + o[i];\n"
   "  return (long)(unsigned)s;\n"
   "}\n",
   {20000, 20000}, "SatPack", 1, "-msse4.1 -ffreestanding"},

  {"paddusb",
   "#include <immintrin.h>\n"
   "long paddusb(long a, long b) {\n"
   "  __m128i va = _mm_setr_epi8((char)a,200u-256,255u-256,1,2,3,4,5,6,7,8,9,10,11,12,13);\n"
   "  __m128i vb = _mm_setr_epi8((char)b,100u-256,255u-256,1,2,3,4,5,6,7,8,9,10,11,12,13);\n"
   "  __m128i r = _mm_adds_epu8(va, vb);\n"
   "  unsigned char o[16]; _mm_storeu_si128((__m128i*)o, r);\n"
   "  int s = 0; for (int i = 0; i < 16; i++) s = s * 31 + o[i];\n"
   "  return (long)(unsigned)s;\n"
   "}\n",
   {200, 100}, "SatPack", 1, "-msse4.1 -ffreestanding"},

  {"paddusw",
   "#include <immintrin.h>\n"
   "long paddusw(long a, long b) {\n"
   "  __m128i va = _mm_setr_epi16((short)a, -1, 60000-65536, 1, 2, 3, 4, 5);\n"
   "  __m128i vb = _mm_setr_epi16((short)b, -1, 10000, 1, 2, 3, 4, 5);\n"
   "  __m128i r = _mm_adds_epu16(va, vb);\n"
   "  unsigned short o[8]; _mm_storeu_si128((__m128i*)o, r);\n"
   "  int s = 0; for (int i = 0; i < 8; i++) s = s * 31 + o[i];\n"
   "  return (long)(unsigned)s;\n"
   "}\n",
   {50000, 50000}, "SatPack", 1, "-msse4.1 -ffreestanding"},

  {"psubsb",
   "#include <immintrin.h>\n"
   "long psubsb(long a, long b) {\n"
   "  __m128i va = _mm_setr_epi8((char)a,-128,127,0,1,2,3,4,5,6,7,8,9,10,11,12);\n"
   "  __m128i vb = _mm_setr_epi8((char)b,100,-100,1,2,3,4,5,6,7,8,9,10,11,12,13);\n"
   "  __m128i r = _mm_subs_epi8(va, vb);\n"
   "  signed char o[16]; _mm_storeu_si128((__m128i*)o, r);\n"
   "  int s = 0; for (int i = 0; i < 16; i++) s = s * 31 + o[i];\n"
   "  return (long)(unsigned)s;\n"
   "}\n",
   {(uint64_t)-120, 50}, "SatPack", 1, "-msse4.1 -ffreestanding"},

  {"psubsw",
   "#include <immintrin.h>\n"
   "long psubsw(long a, long b) {\n"
   "  __m128i va = _mm_setr_epi16((short)a, -32768, 32767, 0, 1, 2, 3, 4);\n"
   "  __m128i vb = _mm_setr_epi16((short)b, 30000, -30000, 1, 2, 3, 4, 5);\n"
   "  __m128i r = _mm_subs_epi16(va, vb);\n"
   "  short o[8]; _mm_storeu_si128((__m128i*)o, r);\n"
   "  int s = 0; for (int i = 0; i < 8; i++) s = s * 31 + o[i];\n"
   "  return (long)(unsigned)s;\n"
   "}\n",
   {(uint64_t)-20000, 20000}, "SatPack", 1, "-msse4.1 -ffreestanding"},

  {"psubusb",
   "#include <immintrin.h>\n"
   "long psubusb(long a, long b) {\n"
   "  __m128i va = _mm_setr_epi8((char)a,10,255u-256,0,1,2,3,4,5,6,7,8,9,10,11,12);\n"
   "  __m128i vb = _mm_setr_epi8((char)b,100,1,5,2,3,4,5,6,7,8,9,10,11,12,13);\n"
   "  __m128i r = _mm_subs_epu8(va, vb);\n"
   "  unsigned char o[16]; _mm_storeu_si128((__m128i*)o, r);\n"
   "  int s = 0; for (int i = 0; i < 16; i++) s = s * 31 + o[i];\n"
   "  return (long)(unsigned)s;\n"
   "}\n",
   {5, 50}, "SatPack", 1, "-msse4.1 -ffreestanding"},

  {"psubusw",
   "#include <immintrin.h>\n"
   "long psubusw(long a, long b) {\n"
   "  __m128i va = _mm_setr_epi16((short)a, 100, 0, 65535-65536, 1, 2, 3, 4);\n"
   "  __m128i vb = _mm_setr_epi16((short)b, 200, 5, 1, 2, 3, 4, 5);\n"
   "  __m128i r = _mm_subs_epu16(va, vb);\n"
   "  unsigned short o[8]; _mm_storeu_si128((__m128i*)o, r);\n"
   "  int s = 0; for (int i = 0; i < 8; i++) s = s * 31 + o[i];\n"
   "  return (long)(unsigned)s;\n"
   "}\n",
   {50, 100}, "SatPack", 1, "-msse4.1 -ffreestanding"},

  {"phaddsw",
   "#include <immintrin.h>\n"
   "long phaddsw(long a, long b) {\n"
   "  __m128i va = _mm_setr_epi16((short)a, 30000, 30000, 5000, -32768, -1000, 7, 8);\n"
   "  __m128i vb = _mm_setr_epi16((short)b, 20000, -32768, -3000, 1, 2, 3, 4);\n"
   "  __m128i r = _mm_hadds_epi16(va, vb);\n"
   "  short o[8]; _mm_storeu_si128((__m128i*)o, r);\n"
   "  int s = 0; for (int i = 0; i < 8; i++) s = s * 31 + o[i];\n"
   "  return (long)(unsigned)s;\n"
   "}\n",
   {30000, 25000}, "SatPack", 1, "-msse4.1 -ffreestanding"},

  {"phsubsw",
   "#include <immintrin.h>\n"
   "long phsubsw(long a, long b) {\n"
   "  __m128i va = _mm_setr_epi16((short)a, -32768, 32767, -30000, 1000, -2000, 7, 8);\n"
   "  __m128i vb = _mm_setr_epi16((short)b, 30000, -30000, 1, 2, 3, 4, 5);\n"
   "  __m128i r = _mm_hsubs_epi16(va, vb);\n"
   "  short o[8]; _mm_storeu_si128((__m128i*)o, r);\n"
   "  int s = 0; for (int i = 0; i < 8; i++) s = s * 31 + o[i];\n"
   "  return (long)(unsigned)s;\n"
   "}\n",
   {32000, (uint64_t)-100}, "SatPack", 1, "-msse4.1 -ffreestanding"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(SatPack, X64SSESatPackRT,
                         ::testing::ValuesIn(kSatPack), rtTCName);
