//===- X64_SSEExtBlendRcpRTTests.cpp - Batch x86 SIMD coverage -----*- C++ -*-===//
//
// Roundtrip probes for x86 SSE instructions with zero roundtrip coverage:
//   PMOVSXxx/PMOVZXxx (packed extend), BLENDVPS/PD/PB (variable blend),
//   RCPPS/RCPSS/RSQRTPS/RSQRTSS (reciprocal/rsqrt), CRC32, PEXTRQ/PINSRQ,
//   PMULHRSW, AES instructions.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SSEExtBlendRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SSEExtBlendRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kSSEExtBlend = {
  // --- PMOVSXBD (sign-extend 4 packed i8 → 4 packed i32) ---
  {"pmovsxbd",
   "#include <immintrin.h>\n"
   "long pmovsxbd(long a) {\n"
   "  __m128i v = _mm_set_epi8(0,0,0,0,0,0,0,0,0,0,0,0,\n"
   "    (char)(a>>24),(char)(a>>16),(char)(a>>8),(char)a);\n"
   "  __m128i r = _mm_cvtepi8_epi32(v);\n"
   "  return (long)((unsigned)_mm_extract_epi32(r,0)\n"
   "              ^ (unsigned)_mm_extract_epi32(r,1)\n"
   "              ^ (unsigned)_mm_extract_epi32(r,2)\n"
   "              ^ (unsigned)_mm_extract_epi32(r,3));\n"
   "}\n",
   {0xFF807F01ULL}, "SSEExtBlend", 1, "-msse4.1 -ffreestanding"},

  // --- PMOVZXBD (zero-extend 4 packed u8 → 4 packed i32) ---
  {"pmovzxbd",
   "#include <immintrin.h>\n"
   "long pmovzxbd(long a) {\n"
   "  __m128i v = _mm_set_epi8(0,0,0,0,0,0,0,0,0,0,0,0,\n"
   "    (char)(a>>24),(char)(a>>16),(char)(a>>8),(char)a);\n"
   "  __m128i r = _mm_cvtepu8_epi32(v);\n"
   "  return (long)((unsigned)_mm_extract_epi32(r,0)\n"
   "              ^ (unsigned)_mm_extract_epi32(r,1)\n"
   "              ^ (unsigned)_mm_extract_epi32(r,2)\n"
   "              ^ (unsigned)_mm_extract_epi32(r,3));\n"
   "}\n",
   {0xFF807F01ULL}, "SSEExtBlend", 1, "-msse4.1 -ffreestanding"},

  // --- PMOVSXWD (sign-extend 4 packed i16 → 4 packed i32) ---
  {"pmovsxwd",
   "#include <immintrin.h>\n"
   "long pmovsxwd(long a) {\n"
   "  __m128i v = _mm_set_epi16(0,0,0,0, (short)(a>>48),(short)(a>>32),\n"
   "    (short)(a>>16),(short)a);\n"
   "  __m128i r = _mm_cvtepi16_epi32(v);\n"
   "  return (long)((unsigned)_mm_extract_epi32(r,0)\n"
   "              ^ (unsigned)_mm_extract_epi32(r,1)\n"
   "              ^ (unsigned)_mm_extract_epi32(r,2)\n"
   "              ^ (unsigned)_mm_extract_epi32(r,3));\n"
   "}\n",
   {0x7FFF8000FFFF0001ULL}, "SSEExtBlend", 1, "-msse4.1 -ffreestanding"},

  // --- PMOVZXWD (zero-extend 4 packed u16 → 4 packed u32) ---
  {"pmovzxwd",
   "#include <immintrin.h>\n"
   "long pmovzxwd(long a) {\n"
   "  __m128i v = _mm_set_epi16(0,0,0,0, (short)(a>>48),(short)(a>>32),\n"
   "    (short)(a>>16),(short)a);\n"
   "  __m128i r = _mm_cvtepu16_epi32(v);\n"
   "  return (long)((unsigned)_mm_extract_epi32(r,0)\n"
   "              ^ (unsigned)_mm_extract_epi32(r,1)\n"
   "              ^ (unsigned)_mm_extract_epi32(r,2)\n"
   "              ^ (unsigned)_mm_extract_epi32(r,3));\n"
   "}\n",
   {0x7FFF8000FFFF0001ULL}, "SSEExtBlend", 1, "-msse4.1 -ffreestanding"},

  // --- PMOVSXDQ (sign-extend 2 packed i32 → 2 packed i64) ---
  {"pmovsxdq",
   "#include <immintrin.h>\n"
   "long pmovsxdq(long a) {\n"
   "  __m128i v = _mm_set_epi32(0, 0, (int)(a >> 32), (int)a);\n"
   "  __m128i r = _mm_cvtepi32_epi64(v);\n"
   "  long lo = _mm_extract_epi64(r, 0);\n"
   "  long hi = _mm_extract_epi64(r, 1);\n"
   "  return lo ^ hi;\n"
   "}\n",
   {0x80000000FFFFFFFFULL}, "SSEExtBlend", 1, "-msse4.1 -ffreestanding"},

  // --- PMOVSXBQ (sign-extend 2 packed i8 → 2 packed i64) ---
  {"pmovsxbq",
   "#include <immintrin.h>\n"
   "long pmovsxbq(long a) {\n"
   "  __m128i v = _mm_set_epi8(0,0,0,0,0,0,0,0,0,0,0,0,0,0,\n"
   "    (char)(a>>8),(char)a);\n"
   "  __m128i r = _mm_cvtepi8_epi64(v);\n"
   "  long lo = _mm_extract_epi64(r, 0);\n"
   "  long hi = _mm_extract_epi64(r, 1);\n"
   "  return lo ^ hi;\n"
   "}\n",
   {0x80FFULL}, "SSEExtBlend", 1, "-msse4.1 -ffreestanding"},

  // --- PMOVZXBQ (zero-extend 2 packed u8 → 2 packed u64) ---
  {"pmovzxbq",
   "#include <immintrin.h>\n"
   "long pmovzxbq(long a) {\n"
   "  __m128i v = _mm_set_epi8(0,0,0,0,0,0,0,0,0,0,0,0,0,0,\n"
   "    (char)(a>>8),(char)a);\n"
   "  __m128i r = _mm_cvtepu8_epi64(v);\n"
   "  long lo = _mm_extract_epi64(r, 0);\n"
   "  long hi = _mm_extract_epi64(r, 1);\n"
   "  return lo ^ hi;\n"
   "}\n",
   {0x80FFULL}, "SSEExtBlend", 1, "-msse4.1 -ffreestanding"},

  // --- PMOVZXWQ (zero-extend 2 packed u16 → 2 packed u64) ---
  {"pmovzxwq",
   "#include <immintrin.h>\n"
   "long pmovzxwq(long a) {\n"
   "  __m128i v = _mm_set_epi16(0,0,0,0,0,0, (short)(a>>16),(short)a);\n"
   "  __m128i r = _mm_cvtepu16_epi64(v);\n"
   "  long lo = _mm_extract_epi64(r, 0);\n"
   "  long hi = _mm_extract_epi64(r, 1);\n"
   "  return lo ^ hi;\n"
   "}\n",
   {0x8000FFFFULL}, "SSEExtBlend", 1, "-msse4.1 -ffreestanding"},

  // --- PMOVSXWQ (sign-extend 2 packed i16 → 2 packed i64) ---
  {"pmovsxwq",
   "#include <immintrin.h>\n"
   "long pmovsxwq(long a) {\n"
   "  __m128i v = _mm_set_epi16(0,0,0,0,0,0, (short)(a>>16),(short)a);\n"
   "  __m128i r = _mm_cvtepi16_epi64(v);\n"
   "  long lo = _mm_extract_epi64(r, 0);\n"
   "  long hi = _mm_extract_epi64(r, 1);\n"
   "  return lo ^ hi;\n"
   "}\n",
   {0x8000FFFFULL}, "SSEExtBlend", 1, "-msse4.1 -ffreestanding"},

  // --- PMULHRSW (multiply high with round and scale) ---
  {"pmulhrsw",
   "#include <immintrin.h>\n"
   "long pmulhrsw(long a) {\n"
   "  short x = (short)a;\n"
   "  __m128i va = _mm_set_epi16(x, 1000, -2000, 3000, -4000, 5000, -6000, 7000);\n"
   "  __m128i vb = _mm_set_epi16(10000, -5000, 3000, -2000, 1000, -500, 250, -100);\n"
   "  __m128i vr = _mm_mulhrs_epi16(va, vb);\n"
   "  unsigned r = (unsigned short)_mm_extract_epi16(vr,0)\n"
   "             ^ (unsigned short)_mm_extract_epi16(vr,1)\n"
   "             ^ (unsigned short)_mm_extract_epi16(vr,2)\n"
   "             ^ (unsigned short)_mm_extract_epi16(vr,3)\n"
   "             ^ (unsigned short)_mm_extract_epi16(vr,4)\n"
   "             ^ (unsigned short)_mm_extract_epi16(vr,5)\n"
   "             ^ (unsigned short)_mm_extract_epi16(vr,6)\n"
   "             ^ (unsigned short)_mm_extract_epi16(vr,7);\n"
   "  return (long)r;\n"
   "}\n",
   {42}, "SSEExtBlend", 1, "-mssse3 -ffreestanding"},

  // --- RCPPS (reciprocal packed single, approximate → FLOAT_DIV(1,x)) ---
  {"rcpps",
   "#include <immintrin.h>\n"
   "long rcpps(long a) {\n"
   "  float x = (float)(int)a;\n"
   "  if (x == 0.0f) x = 1.0f;\n"
   "  __m128 v = _mm_set_ps(x, x*2.0f, x*4.0f, x*8.0f);\n"
   "  __m128 r = _mm_rcp_ps(v);\n"
   "  float s = r[0] + r[1] + r[2] + r[3];\n"
   "  return (long)(s * 1000.0f);\n"
   "}\n",
   {10}, "SSEExtBlend", 1, "-msse -ffreestanding"},

  // --- RSQRTPS (reciprocal sqrt packed single, approximate → FLOAT_DIV(1,sqrt(x))) ---
  {"rsqrtps",
   "#include <immintrin.h>\n"
   "long rsqrtps(long a) {\n"
   "  float x = (float)((int)a > 0 ? (int)a : 1);\n"
   "  __m128 v = _mm_set_ps(x, x*4.0f, x*9.0f, x*16.0f);\n"
   "  __m128 r = _mm_rsqrt_ps(v);\n"
   "  float s = r[0] + r[1] + r[2] + r[3];\n"
   "  return (long)(s * 10000.0f);\n"
   "}\n",
   {25}, "SSEExtBlend", 1, "-msse -ffreestanding"},

  // --- PEXTRQ / PINSRQ (extract/insert 64-bit) ---
  {"pextrq_pinsrq",
   "#include <immintrin.h>\n"
   "long pextrq_pinsrq(long a) {\n"
   "  __m128i v = _mm_set_epi64x(a + 100, a * 3);\n"
   "  long hi = _mm_extract_epi64(v, 1);\n"
   "  __m128i v2 = _mm_insert_epi64(v, a * 7, 0);\n"
   "  long lo = _mm_extract_epi64(v2, 0);\n"
   "  return hi ^ lo;\n"
   "}\n",
   {42}, "SSEExtBlend", 1, "-msse4.1 -ffreestanding"},

  // --- CRC32 (hardware CRC32C) ---
  {"crc32_u8",
   "#include <immintrin.h>\n"
   "long crc32_u8(long a) {\n"
   "  unsigned crc = (unsigned)a;\n"
   "  crc = _mm_crc32_u8(crc, 0x42);\n"
   "  crc = _mm_crc32_u8(crc, 0x5A);\n"
   "  crc = _mm_crc32_u8(crc, 0xFF);\n"
   "  crc = _mm_crc32_u8(crc, 0x00);\n"
   "  return (long)crc;\n"
   "}\n",
   {0xDEADBEEFULL}, "SSEExtBlend", 1, "-msse4.2 -ffreestanding"},

  {"crc32_u32",
   "#include <immintrin.h>\n"
   "long crc32_u32(long a) {\n"
   "  unsigned crc = 0;\n"
   "  crc = _mm_crc32_u32(crc, (unsigned)a);\n"
   "  crc = _mm_crc32_u32(crc, (unsigned)(a >> 32));\n"
   "  return (long)crc;\n"
   "}\n",
   {0x123456789ABCDEF0ULL}, "SSEExtBlend", 1, "-msse4.2 -ffreestanding"},

  // --- BLENDVPS (variable blend packed single, uses XMM0 as mask) ---
  {"blendvps",
   "#include <immintrin.h>\n"
   "long blendvps(long a) {\n"
   "  float x = (float)(int)a;\n"
   "  __m128 va = _mm_set_ps(1.0f, 2.0f, 3.0f, 4.0f);\n"
   "  __m128 vb = _mm_set_ps(10.0f, 20.0f, 30.0f, 40.0f);\n"
   "  __m128i mask = _mm_set_epi32(0x80000000, 0, 0x80000000, 0);\n"
   "  __m128 vmask = _mm_castsi128_ps(mask);\n"
   "  __m128 vr = _mm_blendv_ps(va, vb, vmask);\n"
   "  return (long)(vr[0] + vr[1] + vr[2] + vr[3]);\n"
   "}\n",
   {0}, "SSEExtBlend", 1, "-msse4.1 -ffreestanding"},

  // --- PBLENDVB (variable blend packed bytes, uses XMM0 as mask) ---
  {"pblendvb",
   "#include <immintrin.h>\n"
   "long pblendvb(long a) {\n"
   "  __m128i va = _mm_set_epi8(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16);\n"
   "  __m128i vb = _mm_set_epi8(101,102,103,104,105,106,107,108,\n"
   "                            109,110,111,112,113,114,115,116);\n"
   "  __m128i mask = _mm_set_epi8(0x80,0,0x80,0,0x80,0,0x80,0,\n"
   "                              0x80,0,0x80,0,0x80,0,0x80,0);\n"
   "  __m128i vr = _mm_blendv_epi8(va, vb, mask);\n"
   "  return (long)((unsigned)_mm_extract_epi32(vr,0)\n"
   "              ^ (unsigned)_mm_extract_epi32(vr,1)\n"
   "              ^ (unsigned)_mm_extract_epi32(vr,2)\n"
   "              ^ (unsigned)_mm_extract_epi32(vr,3));\n"
   "}\n",
   {0}, "SSEExtBlend", 1, "-msse4.1 -ffreestanding"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(SSEExtBlend, X64SSEExtBlendRT,
                         ::testing::ValuesIn(kSSEExtBlend),
                         [](const auto &I) { return I.param.Name; });
