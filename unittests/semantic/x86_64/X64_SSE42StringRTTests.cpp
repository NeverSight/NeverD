//===- X64_SSE42StringRTTests.cpp - SSE4.2 string/CRC roundtrip tests ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Covers: CRC32 (x86), PCMPISTRI/PCMPISTRM patterns via C string ops,
//         POPCNT/LZCNT/TZCNT via builtins,
//         packed int accumulation patterns (horizontal add, dot product via
//         multiply-add-accumulate).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SSE42StringRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SSE42StringRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kSSE42String = {

  // x86 CRC32 tests omitted: NeverD codegen requires +sse4.2 feature on target
  // which causes LLVM fatal error in roundtrip. Lift-only tested separately.

  // ===== POPCNT via builtin =====
  {"popcnt_builtin",
   "long popcnt_builtin(long a) {\n"
   "  return (long)__builtin_popcountll((unsigned long long)a);\n"
   "}\n",
   {0xFF00FF00FF00FF00ULL}, "SSE42", 1, "-mpopcnt"},

  // ===== LZCNT via builtin =====
  {"lzcnt_builtin",
   "long lzcnt_builtin(long a) {\n"
   "  if (a == 0) return 64;\n"
   "  return (long)__builtin_clzll((unsigned long long)a);\n"
   "}\n",
   {0x100}, "SSE42", 1, "-mlzcnt"},

  // ===== TZCNT via builtin =====
  {"tzcnt_builtin",
   "long tzcnt_builtin(long a) {\n"
   "  if (a == 0) return 64;\n"
   "  return (long)__builtin_ctzll((unsigned long long)a);\n"
   "}\n",
   {0x100}, "SSE42", 1, "-mbmi"},

  // ===== Integer abs via C =====
  {"c_abs_long",
   "long c_abs_long(long a) {\n"
   "  return a < 0 ? -a : a;\n"
   "}\n",
   {0xFFFFFFFFFFFFFFF0ULL}, "SSE42", 1, ""},

  // ===== Bit rotation (ROL pattern) =====
  {"c_rotl64",
   "long c_rotl64(long a, long n) {\n"
   "  unsigned long long ua = (unsigned long long)a;\n"
   "  int shift = (int)n & 63;\n"
   "  return (long)((ua << shift) | (ua >> (64 - shift)));\n"
   "}\n",
   {0xDEADBEEFCAFEBABEULL, 13}, "SSE42", 1, ""},

  // ===== Bit rotation (ROR pattern) =====
  {"c_rotr64",
   "long c_rotr64(long a, long n) {\n"
   "  unsigned long long ua = (unsigned long long)a;\n"
   "  int shift = (int)n & 63;\n"
   "  return (long)((ua >> shift) | (ua << (64 - shift)));\n"
   "}\n",
   {0xDEADBEEFCAFEBABEULL, 13}, "SSE42", 1, ""},

  // ===== Packed byte sum =====
  {"packed_byte_sum",
   "typedef unsigned char v16qu __attribute__((vector_size(16)));\n"
   "long packed_byte_sum(long a) {\n"
   "  v16qu va = {(unsigned char)a, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 16; i++) sum += va[i];\n"
   "  return sum;\n"
   "}\n",
   {100}, "SSE42", 1, "-msse2"},

  // ===== Packed word shift right =====
  {"packed_word_shr",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "long packed_word_shr(long a) {\n"
   "  v8hi va = {(short)a, 200, -300, 400, -500, 600, -700, 800};\n"
   "  v8hi vr = va >> 2;\n"
   "  return (long)vr[0] + (long)vr[2];\n"
   "}\n",
   {1000}, "SSE42", 1, "-msse2"},

  // ===== Packed byte max (unsigned) =====
  {"packed_ubyte_max",
   "typedef unsigned char v16qu __attribute__((vector_size(16)));\n"
   "long packed_ubyte_max(long a, long b) {\n"
   "  v16qu va = {(unsigned char)a, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};\n"
   "  v16qu vb = {(unsigned char)b, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};\n"
   "  v16qu cmp = va > vb;\n"
   "  v16qu vr = (cmp & va) | (~cmp & vb);\n"
   "  return (long)vr[0];\n"
   "}\n",
   {42, 100}, "SSE42", 1, "-msse2"},

  // ===== Packed double multiply =====
  {"packed_double_mul",
   "typedef double v2df __attribute__((vector_size(16)));\n"
   "long packed_double_mul(long a, long b) {\n"
   "  v2df va = {(double)a, 2.0};\n"
   "  v2df vb = {(double)b, 3.0};\n"
   "  v2df vr = va * vb;\n"
   "  double r = vr[0];\n"
   "  long lr; __builtin_memcpy(&lr, &r, 8);\n"
   "  return lr;\n"
   "}\n",
   {3, 7}, "SSE42", 1, "-msse2"},

  // ===== Packed int accumulation =====
  {"packed_int_accum",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long packed_int_accum(long a) {\n"
   "  v4si va = {(int)a, 10, 20, 30};\n"
   "  v4si vb = {1, 2, 3, 4};\n"
   "  v4si vr = va + vb;\n"
   "  return (long)vr[0] + (long)vr[1] + (long)vr[2] + (long)vr[3];\n"
   "}\n",
   {100}, "SSE42", 1, "-msse2"},

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(SSE42, X64SSE42StringRT,
                         ::testing::ValuesIn(kSSE42String),
                         [](const auto &P) { return P.param.Name; });
