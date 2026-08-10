//===- ARM32_BitManipTests.cpp - ARM32 bit manipulation tests --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests ARM32 CLZ, RBIT, REV, BFC, BFI patterns and complex bit operations.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32BitRT : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32BitRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kARM32Bit = {
  // --- CLZ ---
  {"arm_clz32",
   "int arm_clz32(int a) {\n"
   "  unsigned int x = (unsigned int)a;\n"
   "  return x ? __builtin_clz(x) : 32;\n"
   "}\n",
   {0x100}, "ARM32Bit"},

  // --- CTZ ---
  {"arm_ctz32",
   "int arm_ctz32(int a) {\n"
   "  unsigned int x = (unsigned int)a;\n"
   "  return x ? __builtin_ctz(x) : 32;\n"
   "}\n",
   {0x100}, "ARM32Bit"},

  // --- BSWAP ---
  {"arm_bswap16",
   "int arm_bswap16(int a) {\n"
   "  unsigned short x = (unsigned short)a;\n"
   "  return (unsigned short)((x >> 8) | (x << 8));\n"
   "}\n",
   {0x1234}, "ARM32Bit"},

  // --- Bit isolation ---
  {"arm_blsi", "int arm_blsi(int a) { return a & (-a); }\n", {0xABCD0000}, "ARM32Bit"},
  {"arm_blsr", "int arm_blsr(int a) { return a & (a-1); }\n", {0xABCD0000}, "ARM32Bit"},
  {"arm_blsmsk", "int arm_blsmsk(int a) { return a ^ (a-1); }\n", {0xABCD0000}, "ARM32Bit"},

  // --- BT/BTS/BTR ---
  {"arm_bt", "int arm_bt(int a, int b) { return ((unsigned int)a >> (b & 31)) & 1; }\n", {0xFF00, 8}, "ARM32Bit"},
  {"arm_bts", "int arm_bts(int a, int b) { return a | (1U << (b & 31)); }\n", {0, 15}, "ARM32Bit"},
  {"arm_btr", "int arm_btr(int a, int b) { return a & ~(1U << (b & 31)); }\n", {0xFFFFFFFF, 15}, "ARM32Bit"},

  // --- Byte pack/unpack ---
  {"arm_pack4",
   "int arm_pack4(int a, int b, int c) {\n"
   "  return (a & 0xFF) | ((b & 0xFF) << 8) | ((c & 0xFF) << 16);\n"
   "}\n",
   {0x41, 0x42, 0x43}, "ARM32Bit"},

  {"arm_unpack_b1", "int arm_unpack_b1(int a) { return ((unsigned int)a >> 8) & 0xFF; }\n", {0x44434241}, "ARM32Bit"},

  // --- Zigzag ---
  {"arm_zigzag", "int arm_zigzag(int n) { return (n << 1) ^ (n >> 31); }\n", {(uint64_t)(uint32_t)-100}, "ARM32Bit"},

  // --- Endian swap ---
  {"arm_endian32",
   "int arm_endian32(int a) {\n"
   "  unsigned int x = (unsigned int)a;\n"
   "  return (int)(((x >> 24) & 0xFF) | ((x >> 8) & 0xFF00) |\n"
   "              ((x << 8) & 0xFF0000) | ((x << 24) & 0xFF000000U));\n"
   "}\n",
   {0x01020304}, "ARM32Bit"},

  // --- Popcount ---
  {"arm_popcount_bk",
   "int arm_popcount_bk(int x) {\n"
   "  unsigned int u = (unsigned int)x;\n"
   "  int c = 0;\n"
   "  while (u) { u &= u-1; ++c; }\n"
   "  return c;\n"
   "}\n",
   {0xDEADBEEF}, "ARM32Bit"},

  // --- Hamming ---
  {"arm_hamming",
   "int arm_hamming(int a, int b) {\n"
   "  unsigned int x = (unsigned int)(a ^ b);\n"
   "  int d = 0;\n"
   "  while (x) { d += x & 1; x >>= 1; }\n"
   "  return d;\n"
   "}\n",
   {0xFF00FF00, 0x00FF00FF}, "ARM32Bit"},

  // --- Power of 2 ---
  {"arm_is_pow2", "int arm_is_pow2(int a) { return a > 0 && (a & (a-1)) == 0; }\n", {256}, "ARM32Bit"},
  {"arm_next_pow2",
   "int arm_next_pow2(int a) {\n"
   "  unsigned int v = (unsigned int)(a - 1);\n"
   "  v |= v >> 1; v |= v >> 2; v |= v >> 4;\n"
   "  v |= v >> 8; v |= v >> 16;\n"
   "  return (int)(v + 1);\n"
   "}\n",
   {100}, "ARM32Bit"},

  // --- Rotate ---
  {"arm_ror",
   "int arm_ror(int a, int n) {\n"
   "  unsigned int x = (unsigned int)a;\n"
   "  unsigned int s = (unsigned int)n & 31;\n"
   "  return (int)((x >> s) | (x << (32 - s)));\n"
   "}\n",
   {0xDEADBEEF, 12}, "ARM32Bit"},

  // --- Parity ---
  {"arm_parity",
   "int arm_parity(int a) {\n"
   "  unsigned int x = (unsigned int)a;\n"
   "  x ^= x >> 16; x ^= x >> 8;\n"
   "  x ^= x >> 4; x ^= x >> 2; x ^= x >> 1;\n"
   "  return x & 1;\n"
   "}\n",
   {0x0F0F0F0F}, "ARM32Bit"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(ARM32Bit, ARM32BitRT,
                         ::testing::ValuesIn(kARM32Bit), rtTCName);
