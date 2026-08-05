//===- X64_AVXPerLaneRTTests.cpp - AVX per-lane roundtrip tests -----------===//
//
// Roundtrip tests for x86 AVX/VEX per-lane SIMD operations that have
// historically had full-width vs per-lane semantic bugs.  Uses C vector
// extensions and __builtin_shufflevector since immintrin.h is unavailable
// in cross-compilation on macOS.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64AVXPerLaneRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64AVXPerLaneRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

// ============================================================================
// Packed integer arithmetic with C vector types — exercises PADDD/PSUBD/PMULLD
// ============================================================================
static const std::vector<RoundTripTC> kPackedInt = {
  {"vec4i_add",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long vec4i_add(long a, long b) {\n"
   "  v4i va = {(int)a, 10, 20, 30};\n"
   "  v4i vb = {(int)b, 100, 200, 300};\n"
   "  v4i r = va + vb;\n"
   "  return r[0];\n"
   "}\n",
   {42, 58}, "PackedInt", 1, "-msse2 -mno-avx"},

  {"vec4i_sub",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long vec4i_sub(long a, long b) {\n"
   "  v4i va = {(int)a, 100, 200, 300};\n"
   "  v4i vb = {(int)b, 10, 20, 30};\n"
   "  v4i r = va - vb;\n"
   "  return r[0];\n"
   "}\n",
   {100, 30}, "PackedInt", 1, "-msse2 -mno-avx"},

  {"vec4i_mul",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long vec4i_mul(long a, long b) {\n"
   "  v4i va = {(int)a, 3, 5, 7};\n"
   "  v4i vb = {(int)b, 2, 4, 6};\n"
   "  v4i r = va * vb;\n"
   "  return r[0];\n"
   "}\n",
   {11, 13}, "PackedInt", 1, "-msse4.1 -mno-avx"},

  {"vec8h_add",
   "typedef short v8h __attribute__((vector_size(16)));\n"
   "long vec8h_add(long a, long b) {\n"
   "  v8h va = {(short)a, 2, 3, 4, 5, 6, 7, 8};\n"
   "  v8h vb = {(short)b, 20, 30, 40, 50, 60, 70, 80};\n"
   "  v8h r = va + vb;\n"
   "  return (unsigned short)r[0];\n"
   "}\n",
   {100, 200}, "PackedInt", 1, "-msse2 -mno-avx"},

  {"vec16b_add",
   "typedef char v16b __attribute__((vector_size(16)));\n"
   "long vec16b_add(long a, long b) {\n"
   "  v16b va = {(char)a, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};\n"
   "  v16b vb = {(char)b, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 0, 0, 0, 0};\n"
   "  v16b r = va + vb;\n"
   "  return (unsigned char)r[0];\n"
   "}\n",
   {0x10, 0x20}, "PackedInt", 1, "-msse2 -mno-avx"},
};

// ============================================================================
// Packed comparisons — exercises PCMPEQD/PCMPGTD etc.
// ============================================================================
static const std::vector<RoundTripTC> kPackedCmp = {
  {"vec4i_cmpeq",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long vec4i_cmpeq(long a, long b) {\n"
   "  v4i va = {(int)a, 10, 20, 30};\n"
   "  v4i vb = {(int)b, 10, 99, 30};\n"
   "  v4i r = (va == vb);\n"
   "  return r[0] & 1;\n"
   "}\n",
   {42, 42}, "PackedCmp", 1, "-msse2 -mno-avx"},

  {"vec4i_cmpgt",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long vec4i_cmpgt(long a, long b) {\n"
   "  v4i va = {(int)a, 10, 20, 30};\n"
   "  v4i vb = {(int)b, 100, 200, 300};\n"
   "  v4i r = (va > vb);\n"
   "  return r[0] & 1;\n"
   "}\n",
   {100, 50}, "PackedCmp", 1, "-msse2 -mno-avx"},
};

// ============================================================================
// Packed float arithmetic — exercises ADDPS/SUBPS/MULPS/DIVPS/MAXPS/MINPS
// ============================================================================
static const std::vector<RoundTripTC> kPackedFloat = {
  {"vec4f_add",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long vec4f_add(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  v4f va = {fa, 2.0f, 3.0f, 4.0f};\n"
   "  v4f vb = {fb, 20.0f, 30.0f, 40.0f};\n"
   "  v4f r = va + vb;\n"
   "  float res = r[0];\n"
   "  long rv = 0; __builtin_memcpy(&rv, &res, 4); return rv;\n"
   "}\n",
   {0x40400000ULL, 0x40800000ULL}, "PackedFloat", 1, "-msse -mno-avx"},

  {"vec4f_mul",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long vec4f_mul(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  v4f va = {fa, 2.0f, 3.0f, 4.0f};\n"
   "  v4f vb = {fb, 0.5f, 0.25f, 0.125f};\n"
   "  v4f r = va * vb;\n"
   "  float res = r[0];\n"
   "  long rv = 0; __builtin_memcpy(&rv, &res, 4); return rv;\n"
   "}\n",
   {0x40A00000ULL, 0x40000000ULL}, "PackedFloat", 1, "-msse -mno-avx"},

  {"vec2d_add",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long vec2d_add(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  v2d va = {da, 100.0};\n"
   "  v2d vb = {db, 200.0};\n"
   "  v2d r = va + vb;\n"
   "  double res = r[0];\n"
   "  long rv; __builtin_memcpy(&rv, &res, 8); return rv;\n"
   "}\n",
   {0x4024000000000000ULL, 0x4014000000000000ULL}, "PackedFloat", 1, "-msse2 -mno-avx"},
};

// ============================================================================
// Shuffle with __builtin_shufflevector — exercises PSHUFD/SHUFPS
// ============================================================================
static const std::vector<RoundTripTC> kShuffle = {
  {"shuffle_4i_rotate",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long shuffle_4i_rotate(long a, long b) {\n"
   "  v4i v = {(int)a, (int)b, 30, 40};\n"
   "  v4i r = __builtin_shufflevector(v, v, 1, 2, 3, 0);\n"
   "  return r[0];\n"
   "}\n",
   {10, 20}, "Shuffle", 1, "-msse2 -mno-avx"},

  {"shuffle_4i_broadcast",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long shuffle_4i_broadcast(long a) {\n"
   "  v4i v = {(int)a, 0, 0, 0};\n"
   "  v4i r = __builtin_shufflevector(v, v, 0, 0, 0, 0);\n"
   "  return r[3];\n"
   "}\n",
   {42}, "Shuffle", 1, "-msse2 -mno-avx"},

  {"shuffle_8h_reverse",
   "typedef short v8h __attribute__((vector_size(16)));\n"
   "long shuffle_8h_reverse(long a) {\n"
   "  v8h v = {(short)a, 2, 3, 4, 5, 6, 7, 8};\n"
   "  v8h r = __builtin_shufflevector(v, v, 7, 6, 5, 4, 3, 2, 1, 0);\n"
   "  return (unsigned short)r[7];\n"
   "}\n",
   {1234}, "Shuffle", 1, "-msse2 -mno-avx"},
};

// ============================================================================
// Packed shifts — exercises PSLLD/PSRLD/PSRAD/PSLLW/PSRLW
// ============================================================================
static const std::vector<RoundTripTC> kPackedShift = {
  {"vec4i_shl",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long vec4i_shl(long a) {\n"
   "  v4i v = {(int)a, 100, 200, 300};\n"
   "  v4i r = v << 4;\n"
   "  return r[0];\n"
   "}\n",
   {0x12345}, "PackedShift", 1, "-msse2 -mno-avx"},

  {"vec4i_shr",
   "typedef unsigned v4u __attribute__((vector_size(16)));\n"
   "long vec4i_shr(long a) {\n"
   "  v4u v = {(unsigned)a, 0xFFFFFFFF, 0x80000000, 0};\n"
   "  v4u r = v >> 8;\n"
   "  return r[0];\n"
   "}\n",
   {0xABCD1234ULL}, "PackedShift", 1, "-msse2 -mno-avx"},

  {"vec4i_sar",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long vec4i_sar(long a) {\n"
   "  v4i v = {(int)a, -100, -200, -300};\n"
   "  v4i r = v >> 4;\n"
   "  return r[0];\n"
   "}\n",
   {(uint64_t)(int64_t)-0x12345}, "PackedShift", 1, "-msse2 -mno-avx"},
};

// ============================================================================
// Bitwise packed — exercises PAND/POR/PXOR/PANDN
// ============================================================================
static const std::vector<RoundTripTC> kPackedBit = {
  {"vec4i_and",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long vec4i_and(long a, long b) {\n"
   "  v4i va = {(int)a, 0xFF00FF, 0xAA55AA55, 0};\n"
   "  v4i vb = {(int)b, 0x0F0F0F, 0xFFFF0000, 0};\n"
   "  v4i r = va & vb;\n"
   "  return r[0];\n"
   "}\n",
   {0xABCDEF12ULL, 0xF0F0F0F0ULL}, "PackedBit", 1, "-msse2 -mno-avx"},

  {"vec4i_xor",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long vec4i_xor(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i r = va ^ vb;\n"
   "  return r[0];\n"
   "}\n",
   {0x12345678ULL, 0x9ABCDEF0ULL}, "PackedBit", 1, "-msse2 -mno-avx"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(PackedInt, X64AVXPerLaneRT,
                         ::testing::ValuesIn(kPackedInt), rtTCName);
INSTANTIATE_TEST_SUITE_P(PackedCmp, X64AVXPerLaneRT,
                         ::testing::ValuesIn(kPackedCmp), rtTCName);
INSTANTIATE_TEST_SUITE_P(PackedFloat, X64AVXPerLaneRT,
                         ::testing::ValuesIn(kPackedFloat), rtTCName);
INSTANTIATE_TEST_SUITE_P(Shuffle, X64AVXPerLaneRT,
                         ::testing::ValuesIn(kShuffle), rtTCName);
INSTANTIATE_TEST_SUITE_P(PackedShift, X64AVXPerLaneRT,
                         ::testing::ValuesIn(kPackedShift), rtTCName);
INSTANTIATE_TEST_SUITE_P(PackedBit, X64AVXPerLaneRT,
                         ::testing::ValuesIn(kPackedBit), rtTCName);
