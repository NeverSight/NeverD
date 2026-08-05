//===- X64_SIMDShufflePermRTTests.cpp - SSE shuffle/permute roundtrip -----===//
//
// Roundtrip tests for x86 SIMD shuffle, permute, unpack, interleave, and
// pack instructions.  These have complex per-lane semantics and historically
// have been a major source of lift bugs.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SIMDShufflePermRT : public SemanticRoundTripFixture,
                              public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SIMDShufflePermRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

// ============================================================================
// PSHUFD — dword shuffle within XMM
// ============================================================================
static const std::vector<RoundTripTC> kPSHUFD = {
  {"pshufd_reverse",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long pshufd_reverse(long a, long b) {\n"
   "  v4i v = {(int)a, (int)b, 3, 4};\n"
   "  v4i r = __builtin_ia32_pshufd(v, 0x1B);\n"  // 0x1B = reverse
   "  return r[0];\n"
   "}\n",
   {10, 20}, "PSHUFD", 1, "-msse2 -mno-avx"},

  {"pshufd_broadcast",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long pshufd_broadcast(long a) {\n"
   "  v4i v = {(int)a, 0, 0, 0};\n"
   "  v4i r = __builtin_ia32_pshufd(v, 0x00);\n"  // broadcast lane 0
   "  return r[3];\n"
   "}\n",
   {42}, "PSHUFD", 1, "-msse2 -mno-avx"},
};

// ============================================================================
// PSHUFB — byte-level shuffle (SSSE3)
// ============================================================================
static const std::vector<RoundTripTC> kPSHUFB = {
  {"pshufb_identity",
   "typedef char v16c __attribute__((vector_size(16)));\n"
   "long pshufb_identity(long a, long b) {\n"
   "  v16c data = {(char)a, (char)b, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};\n"
   "  v16c mask = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};\n"
   "  v16c r = __builtin_ia32_pshufb128(data, mask);\n"
   "  return (unsigned char)r[0];\n"
   "}\n",
   {0x41, 0x42}, "PSHUFB", 1, "-mssse3 -mno-avx"},

  {"pshufb_zero_mask",
   "typedef char v16c __attribute__((vector_size(16)));\n"
   "long pshufb_zero_mask(long a) {\n"
   "  v16c data = {(char)a, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};\n"
   "  v16c mask = {-128, 0, -128, 0, -128, 0, -128, 0, -128, 0, -128, 0, -128, 0, -128, 0};\n"
   "  v16c r = __builtin_ia32_pshufb128(data, mask);\n"
   "  return (unsigned char)r[1];\n"
   "}\n",
   {99}, "PSHUFB", 1, "-mssse3 -mno-avx"},
};

// ============================================================================
// PUNPCKL/PUNPCKH — interleave low/high
// ============================================================================
static const std::vector<RoundTripTC> kPUNPCK = {
  {"punpcklbw_c_interleave",
   "typedef char v16c __attribute__((vector_size(16)));\n"
   "long punpcklbw_c_interleave(long a, long b) {\n"
   "  v16c va = {(char)a, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};\n"
   "  v16c vb = {(char)b, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 0, 0, 0, 0};\n"
   "  v16c r = __builtin_shufflevector(va, vb, 0,16, 1,17, 2,18, 3,19, 4,20, 5,21, 6,22, 7,23);\n"
   "  return (unsigned char)r[0] + ((unsigned char)r[1] << 8);\n"
   "}\n",
   {0x11, 0x22}, "PUNPCK", 1, "-msse2 -mno-avx"},

  {"punpckldq_c_interleave",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long punpckldq_c_interleave(long a, long b) {\n"
   "  v4i va = {(int)a, 2, 3, 4};\n"
   "  v4i vb = {(int)b, 20, 30, 40};\n"
   "  v4i r = __builtin_shufflevector(va, vb, 0, 4, 1, 5);\n"
   "  return r[0] + r[1];\n"
   "}\n",
   {100, 200}, "PUNPCK", 1, "-msse2 -mno-avx"},

  {"punpckhwd_c_interleave",
   "typedef short v8s __attribute__((vector_size(16)));\n"
   "long punpckhwd_c_interleave(long a, long b) {\n"
   "  v8s va = {1, 2, 3, (short)a, 5, 6, 7, 8};\n"
   "  v8s vb = {10, 20, 30, (short)b, 50, 60, 70, 80};\n"
   "  v8s r = __builtin_shufflevector(va, vb, 4,12, 5,13, 6,14, 7,15);\n"
   "  return r[0];\n"
   "}\n",
   {1000, 2000}, "PUNPCK", 1, "-msse2 -mno-avx"},
};

// ============================================================================
// PALIGNR — byte-granularity concatenate + shift (SSSE3)
// ============================================================================
static const std::vector<RoundTripTC> kPALIGNR = {
  {"palignr_shift4",
   "typedef char v16c __attribute__((vector_size(16)));\n"
   "long palignr_shift4(long a, long b) {\n"
   "  v16c va = {(char)a, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};\n"
   "  v16c vb = {(char)b, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 0, 0, 0, 0};\n"
   "  v16c r = __builtin_ia32_palignr128(va, vb, 4*8);\n"
   "  return (unsigned char)r[0];\n"
   "}\n",
   {0xAA, 0xBB}, "PALIGNR", 1, "-mssse3 -mno-avx"},
};

// ============================================================================
// SHUFPS / SHUFPD — float shuffle
// ============================================================================
static const std::vector<RoundTripTC> kSHUFPS = {
  {"shufps_swap_pairs",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long shufps_swap_pairs(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  v4f va = {fa, fb, 1.0f, 2.0f};\n"
   "  v4f vb = {3.0f, 4.0f, 5.0f, 6.0f};\n"
   "  v4f r = __builtin_ia32_shufps(va, vb, 0x4E);\n"
   "  float res = r[0];\n"
   "  long rv = 0; __builtin_memcpy(&rv, &res, 4); return rv;\n"
   "}\n",
   {0x40400000ULL, 0x40800000ULL}, "SHUFPS", 1, "-msse -mno-avx"},
};

// ============================================================================
// PACKSSWB / PACKUSWB — pack with saturation
// ============================================================================
static const std::vector<RoundTripTC> kPACK = {
  {"packsswb_basic",
   "typedef short v8s __attribute__((vector_size(16)));\n"
   "typedef char v16c __attribute__((vector_size(16)));\n"
   "long packsswb_basic(long a, long b) {\n"
   "  v8s va = {(short)a, 200, -200, 50, -50, 127, -128, 0};\n"
   "  v8s vb = {(short)b, 300, -300, 100, -100, 0, 0, 0};\n"
   "  v16c r = __builtin_ia32_packsswb128(va, vb);\n"
   "  return (unsigned char)r[0];\n"
   "}\n",
   {42, 99}, "PACK", 1, "-msse2 -mno-avx"},

  {"packsswb_basic_noopt",
   "typedef short v8s __attribute__((vector_size(16)));\n"
   "typedef char v16c __attribute__((vector_size(16)));\n"
   "long packsswb_basic_noopt(long a, long b) {\n"
   "  v8s va = {(short)a, 200, -200, 50, -50, 127, -128, 0};\n"
   "  v8s vb = {(short)b, 300, -300, 100, -100, 0, 0, 0};\n"
   "  v16c r = __builtin_ia32_packsswb128(va, vb);\n"
   "  return (unsigned char)r[0];\n"
   "}\n",
   {42, 99}, "PACK", 1, "-msse2 -mno-avx", /*NoOpt=*/true},

  {"packuswb_basic",
   "typedef short v8s __attribute__((vector_size(16)));\n"
   "typedef char v16c __attribute__((vector_size(16)));\n"
   "long packuswb_basic(long a, long b) {\n"
   "  v8s va = {(short)a, 200, 300, -10, 0, 127, 255, 256};\n"
   "  v8s vb = {(short)b, 0, 0, 0, 0, 0, 0, 0};\n"
   "  v16c r = __builtin_ia32_packuswb128(va, vb);\n"
   "  return (unsigned char)r[0];\n"
   "}\n",
   {150, 200}, "PACK", 1, "-msse2 -mno-avx"},

  {"packssdw_basic",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "typedef short v8s __attribute__((vector_size(16)));\n"
   "long packssdw_basic(long a, long b) {\n"
   "  v4i va = {(int)a, 50000, -50000, 0};\n"
   "  v4i vb = {(int)b, 32767, -32768, 100};\n"
   "  v8s r = __builtin_ia32_packssdw128(va, vb);\n"
   "  return (unsigned short)r[0];\n"
   "}\n",
   {1000, 2000}, "PACK", 1, "-msse2 -mno-avx"},
};

// ============================================================================
// MOVHLPS / MOVLHPS — move high/low packed single-precision
// ============================================================================
static const std::vector<RoundTripTC> kMOVHL = {
  {"movhlps_c_basic",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long movhlps_c_basic(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  v4f va = {fa, 2.0f, 3.0f, 4.0f};\n"
   "  v4f vb = {5.0f, 6.0f, fb, 8.0f};\n"
   "  v4f r = __builtin_shufflevector(vb, va, 2, 3, 6, 7);\n"
   "  float res = r[0];\n"
   "  long rv = 0; __builtin_memcpy(&rv, &res, 4); return rv;\n"
   "}\n",
   {0x3F800000ULL, 0x41000000ULL}, "MOVHLPS", 1, "-msse -mno-avx"},
};

// ============================================================================
// SSE4.1 conversion: CVTPS2DQ / CVTDQ2PS / CVTTPD2DQ
// ============================================================================
static const std::vector<RoundTripTC> kSSEConv = {
  {"cvt_float_to_int",
   "long cvt_float_to_int(long a) {\n"
   "  float fa; __builtin_memcpy(&fa, &a, 4);\n"
   "  int r = (int)fa;\n"
   "  return r;\n"
   "}\n",
   {0x41480000ULL}, "SSEConv", 1, ""},  // 12.5f → 12

  {"cvt_int_to_float",
   "long cvt_int_to_float(long a) {\n"
   "  float r = (float)(int)a;\n"
   "  long rv = 0; __builtin_memcpy(&rv, &r, 4); return rv;\n"
   "}\n",
   {25}, "SSEConv", 1, ""},
};

// ============================================================================
// PABS — packed absolute value (SSSE3)
// ============================================================================
static const std::vector<RoundTripTC> kPABS = {
  {"c_abs_int",
   "long c_abs_int(long a) {\n"
   "  int v = (int)a;\n"
   "  return v < 0 ? -v : v;\n"
   "}\n",
   {(uint64_t)(int64_t)-123}, "PABS", 1, ""},

  {"c_abs_short",
   "long c_abs_short(long a) {\n"
   "  short v = (short)a;\n"
   "  return (unsigned short)(v < 0 ? -v : v);\n"
   "}\n",
   {(uint64_t)(int64_t)-500}, "PABS", 1, ""},

  {"c_abs_byte",
   "long c_abs_byte(long a) {\n"
   "  signed char v = (signed char)a;\n"
   "  return (unsigned char)(v < 0 ? -v : v);\n"
   "}\n",
   {(uint64_t)(int64_t)-77}, "PABS", 1, ""},
};

// ============================================================================
// PSIGNB/PSIGNW/PSIGND — conditional negate (SSSE3)
// ============================================================================
static const std::vector<RoundTripTC> kPSIGN = {
  {"psignd_negate",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long psignd_negate(long a) {\n"
   "  v4i data = {(int)a, 100, -50, 0};\n"
   "  v4i sign = {-1, 1, -1, 0};\n"
   "  v4i r = __builtin_ia32_psignd128(data, sign);\n"
   "  return r[0];\n"
   "}\n",
   {42}, "PSIGN", 1, "-mssse3 -mno-avx"},
};

// ============================================================================
// PMADDWD — packed multiply and add (SSE2)
// ============================================================================
static const std::vector<RoundTripTC> kPMADDWD = {
  {"pmaddwd_basic",
   "typedef short v8s __attribute__((vector_size(16)));\n"
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long pmaddwd_basic(long a, long b) {\n"
   "  v8s va = {(short)a, (short)b, 3, 4, 5, 6, 7, 8};\n"
   "  v8s vb = {1, 1, 1, 1, 1, 1, 1, 1};\n"
   "  v4i r = __builtin_ia32_pmaddwd128(va, vb);\n"
   "  return r[0];\n"  // a*1 + b*1
   "}\n",
   {10, 20}, "PMADDWD", 1, "-msse2 -mno-avx"},
};

// ============================================================================
// PHADDW/PHADDD — horizontal add (SSSE3)
// ============================================================================
static const std::vector<RoundTripTC> kPHADD = {
  {"phaddd_basic",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long phaddd_basic(long a, long b) {\n"
   "  v4i va = {(int)a, (int)b, 100, 200};\n"
   "  v4i vb = {1, 2, 3, 4};\n"
   "  v4i r = __builtin_ia32_phaddd128(va, vb);\n"
   "  return r[0];\n"  // a + b
   "}\n",
   {15, 25}, "PHADD", 1, "-mssse3 -mno-avx"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(PSHUFD, X64SIMDShufflePermRT,
                         ::testing::ValuesIn(kPSHUFD), rtTCName);
INSTANTIATE_TEST_SUITE_P(PSHUFB, X64SIMDShufflePermRT,
                         ::testing::ValuesIn(kPSHUFB), rtTCName);
INSTANTIATE_TEST_SUITE_P(PUNPCK, X64SIMDShufflePermRT,
                         ::testing::ValuesIn(kPUNPCK), rtTCName);
INSTANTIATE_TEST_SUITE_P(PALIGNR, X64SIMDShufflePermRT,
                         ::testing::ValuesIn(kPALIGNR), rtTCName);
INSTANTIATE_TEST_SUITE_P(SHUFPS, X64SIMDShufflePermRT,
                         ::testing::ValuesIn(kSHUFPS), rtTCName);
INSTANTIATE_TEST_SUITE_P(PACK, X64SIMDShufflePermRT,
                         ::testing::ValuesIn(kPACK), rtTCName);
INSTANTIATE_TEST_SUITE_P(MOVHLPS, X64SIMDShufflePermRT,
                         ::testing::ValuesIn(kMOVHL), rtTCName);
INSTANTIATE_TEST_SUITE_P(SSEConv, X64SIMDShufflePermRT,
                         ::testing::ValuesIn(kSSEConv), rtTCName);
INSTANTIATE_TEST_SUITE_P(PABS, X64SIMDShufflePermRT,
                         ::testing::ValuesIn(kPABS), rtTCName);
INSTANTIATE_TEST_SUITE_P(PSIGN, X64SIMDShufflePermRT,
                         ::testing::ValuesIn(kPSIGN), rtTCName);
INSTANTIATE_TEST_SUITE_P(PMADDWD, X64SIMDShufflePermRT,
                         ::testing::ValuesIn(kPMADDWD), rtTCName);
INSTANTIATE_TEST_SUITE_P(PHADD, X64SIMDShufflePermRT,
                         ::testing::ValuesIn(kPHADD), rtTCName);
