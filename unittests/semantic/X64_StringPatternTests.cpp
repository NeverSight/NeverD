//===- X64_StringPatternTests.cpp - String/memory pattern tests --*- C++ -*-===//
//
// Tests x86_64 patterns that exercise string operations, memcpy/memset-like
// patterns, and complex memory access modes through C expressions.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64StringRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64StringRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kX64String = {
  // --- memset-like patterns ---
  {"c_fill_array",
   "long c_fill_array(long val) {\n"
   "  long arr[8];\n"
   "  for (int i = 0; i < 8; ++i) arr[i] = val;\n"
   "  return arr[0] + arr[7];\n"
   "}\n",
   {42}, "StrRT"},

  // --- memcpy-like with stride ---
  {"c_stride_copy",
   "long c_stride_copy(long a, long b, long c, long d) {\n"
   "  long src[4] = {a, b, c, d};\n"
   "  long dst[4];\n"
   "  for (int i = 0; i < 4; ++i) dst[i] = src[3 - i];\n"
   "  return dst[0]*1000 + dst[1]*100 + dst[2]*10 + dst[3];\n"
   "}\n",
   {1, 2, 3, 4}, "StrRT"},

  // --- String length (null-terminated) ---
  {"c_strlen_like",
   "long c_strlen_like(long packed) {\n"
   "  unsigned char buf[8];\n"
   "  for (int i = 0; i < 8; ++i) buf[i] = (packed >> (i*8)) & 0xFF;\n"
   "  long len = 0;\n"
   "  while (len < 8 && buf[len]) ++len;\n"
   "  return len;\n"
   "}\n",
   {0x0000006F6C6C6548ULL}, "StrRT"},

  // --- XOR cipher ---
  {"c_xor_cipher",
   "long c_xor_cipher(long data, long key) {\n"
   "  unsigned char buf[8], k[8];\n"
   "  for (int i = 0; i < 8; ++i) {\n"
   "    buf[i] = (data >> (i*8)) & 0xFF;\n"
   "    k[i] = (key >> (i*8)) & 0xFF;\n"
   "    buf[i] ^= k[i];\n"
   "  }\n"
   "  long result = 0;\n"
   "  for (int i = 7; i >= 0; --i) result = (result << 8) | buf[i];\n"
   "  return result;\n"
   "}\n",
   {0x48656C6C6F000000ULL, 0xFF00FF00FF00FF00ULL}, "StrRT"},

  // --- Lookup table pattern ---
  {"c_nibble_to_hex",
   "long c_nibble_to_hex(long a) {\n"
   "  int n = (int)(a & 0xF);\n"
   "  if (n < 10) return '0' + n;\n"
   "  return 'A' + n - 10;\n"
   "}\n",
   {0xC}, "StrRT"},

  // --- Matrix-like 2D access ---
  {"c_matrix_trace",
   "long c_matrix_trace(long a, long b, long c, long d) {\n"
   "  long m[4] = {a, b, c, d};\n"
   "  return m[0] + m[3];\n"
   "}\n",
   {1, 2, 3, 4}, "StrRT"},

  // --- Reduction patterns ---
  {"c_xor_reduce",
   "long c_xor_reduce(long a, long b, long c, long d) {\n"
   "  return a ^ b ^ c ^ d;\n"
   "}\n",
   {0x12, 0x34, 0x56, 0x78}, "StrRT"},

  {"c_and_reduce",
   "long c_and_reduce(long a, long b, long c) {\n"
   "  return a & b & c;\n"
   "}\n",
   {0xFF0F, 0xF0FF, 0xFFF0}, "StrRT"},

  // --- Pack/unpack bytes ---
  {"c_pack_bytes",
   "long c_pack_bytes(long a, long b, long c, long d) {\n"
   "  return ((unsigned long)(a & 0xFF)) |\n"
   "         ((unsigned long)(b & 0xFF) << 8) |\n"
   "         ((unsigned long)(c & 0xFF) << 16) |\n"
   "         ((unsigned long)(d & 0xFF) << 24);\n"
   "}\n",
   {0x41, 0x42, 0x43, 0x44}, "StrRT"},

  {"c_unpack_byte3",
   "long c_unpack_byte3(long packed) {\n"
   "  return ((unsigned long)packed >> 24) & 0xFF;\n"
   "}\n",
   {0x44434241ULL}, "StrRT"},

  // --- Endian conversion ---
  {"c_endian16",
   "long c_endian16(long a) {\n"
   "  unsigned short x = (unsigned short)a;\n"
   "  return (unsigned short)((x >> 8) | (x << 8));\n"
   "}\n",
   {0x1234}, "StrRT"},

  {"c_endian32",
   "long c_endian32(long a) {\n"
   "  unsigned int x = (unsigned int)a;\n"
   "  x = ((x >> 8) & 0x00FF00FFU) | ((x << 8) & 0xFF00FF00U);\n"
   "  x = (x >> 16) | (x << 16);\n"
   "  return x;\n"
   "}\n",
   {0x01020304}, "StrRT"},

  // --- Accumulate with different strides ---
  {"c_dot_product",
   "long c_dot_product(long a0, long a1, long b0, long b1) {\n"
   "  return a0*b0 + a1*b1;\n"
   "}\n",
   {3, 4, 5, 6}, "StrRT"},

  // --- Ring buffer simulation (avoids .rodata array initializer) ---
  {"c_ring_buf",
   "long c_ring_buf(long val, long idx) {\n"
   "  long buf[4];\n"
   "  buf[0] = 10; buf[1] = 20; buf[2] = 30; buf[3] = 40;\n"
   "  buf[(int)(idx & 3)] = val;\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 4; ++i) sum += buf[i];\n"
   "  return sum;\n"
   "}\n",
   {99, 2}, "StrRT"},

  // --- Hamming distance ---
  {"c_hamming",
   "long c_hamming(long a, long b) {\n"
   "  unsigned long x = (unsigned long)(a ^ b);\n"
   "  long d = 0;\n"
   "  while (x) { d += x & 1; x >>= 1; }\n"
   "  return d;\n"
   "}\n",
   {0xFF00FF00ULL, 0x00FF00FFULL}, "StrRT"},

  // --- Zigzag encode/decode ---
  {"c_zigzag_encode",
   "long c_zigzag_encode(long n) {\n"
   "  return (n << 1) ^ (n >> 63);\n"
   "}\n",
   {(uint64_t)-100}, "StrRT"},

  {"c_zigzag_decode",
   "long c_zigzag_decode(long n) {\n"
   "  unsigned long u = (unsigned long)n;\n"
   "  return (long)((u >> 1) ^ -(u & 1));\n"
   "}\n",
   {199}, "StrRT"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(StrRT, X64StringRT,
                         ::testing::ValuesIn(kX64String), rtTCName);
