//===- X64_Word32Tests.cpp - 32-bit operation tests ----------*- C++ -*-===//
//
// Tests x86_64 32-bit operation patterns. On x86-64, 32-bit operations
// implicitly zero-extend to 64 bits, generating different encodings than
// 64-bit ops. Covers: add/sub/mul/div/shift/compare in 32-bit mode.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64W32RT : public SemanticRoundTripFixture,
                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64W32RT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kX64W32 = {
  {"w32_add", "long w32_add(long a, long b) { return (long)((unsigned int)a + (unsigned int)b); }\n", {100, 42}, "W32RT"},
  {"w32_sub", "long w32_sub(long a, long b) { return (long)((unsigned int)a - (unsigned int)b); }\n", {100, 42}, "W32RT"},
  {"w32_mul", "long w32_mul(long a, long b) { return (long)((unsigned int)a * (unsigned int)b); }\n", {1000, 42}, "W32RT"},
  {"w32_div", "long w32_div(long a, long b) { return (long)((unsigned int)a / (unsigned int)b); }\n", {1000, 7}, "W32RT"},
  {"w32_mod", "long w32_mod(long a, long b) { return (long)((unsigned int)a % (unsigned int)b); }\n", {1000, 7}, "W32RT"},
  {"w32_sdiv", "long w32_sdiv(long a, long b) { return (long)((int)a / (int)b); }\n", {1000, 7}, "W32RT"},
  {"w32_smod", "long w32_smod(long a, long b) { return (long)((int)a % (int)b); }\n", {1000, 7}, "W32RT"},
  {"w32_and", "long w32_and(long a, long b) { return (long)((unsigned int)a & (unsigned int)b); }\n", {0xFF00, 0x0FF0}, "W32RT"},
  {"w32_or", "long w32_or(long a, long b) { return (long)((unsigned int)a | (unsigned int)b); }\n", {0xF0, 0x0F}, "W32RT"},
  {"w32_xor", "long w32_xor(long a, long b) { return (long)((unsigned int)a ^ (unsigned int)b); }\n", {0xFF, 0x55}, "W32RT"},
  {"w32_not", "long w32_not(long a) { return (long)(~(unsigned int)a); }\n", {0xFF}, "W32RT"},
  {"w32_neg", "long w32_neg(long a) { return (long)(-(int)a); }\n", {42}, "W32RT"},
  {"w32_shl", "long w32_shl(long a, long b) { return (long)((unsigned int)a << ((int)b & 31)); }\n", {1, 20}, "W32RT"},
  {"w32_shr", "long w32_shr(long a, long b) { return (long)((unsigned int)a >> ((int)b & 31)); }\n", {0x80000000ULL, 4}, "W32RT"},
  {"w32_sar", "long w32_sar(long a, long b) { return (long)((int)a >> ((int)b & 31)); }\n", {0x80000000ULL, 4}, "W32RT"},
  {"w32_eq", "long w32_eq(long a, long b) { return (unsigned int)a == (unsigned int)b; }\n", {42, 42}, "W32RT"},
  {"w32_lt", "long w32_lt(long a, long b) { return (int)a < (int)b; }\n", {0x80000000ULL, 1}, "W32RT"},
  {"w32_ult", "long w32_ult(long a, long b) { return (unsigned int)a < (unsigned int)b; }\n", {5, 100}, "W32RT"},
  {"w32_min", "long w32_min(long a, long b) { int x=(int)a, y=(int)b; return (long)(x<y?x:y); }\n", {3, 7}, "W32RT"},
  {"w32_max", "long w32_max(long a, long b) { int x=(int)a, y=(int)b; return (long)(x>y?x:y); }\n", {3, 7}, "W32RT"},
  {"w32_abs", "long w32_abs(long a) { int x=(int)a; return (long)(x<0?-x:x); }\n", {0xFFFFFFFD}, "W32RT"},
  {"w32_clamp",
   "long w32_clamp(long v, long lo, long hi) {\n"
   "  int x=(int)v, l=(int)lo, h=(int)hi;\n"
   "  return (long)(x<l?l:x>h?h:x);\n"
   "}\n",
   {50, 10, 100}, "W32RT"},
  {"w32_rotl",
   "long w32_rotl(long a, long b) {\n"
   "  unsigned int x=(unsigned int)a, n=(unsigned int)b&31;\n"
   "  return (long)((x<<n)|(x>>(32-n)));\n"
   "}\n",
   {0xDEADBEEF, 12}, "W32RT"},
  {"w32_rotr",
   "long w32_rotr(long a, long b) {\n"
   "  unsigned int x=(unsigned int)a, n=(unsigned int)b&31;\n"
   "  return (long)((x>>n)|(x<<(32-n)));\n"
   "}\n",
   {0xDEADBEEF, 12}, "W32RT"},
  {"w32_popcount",
   "long w32_popcount(long a) {\n"
   "  unsigned int x = (unsigned int)a;\n"
   "  int c = 0;\n"
   "  while (x) { x &= x-1; ++c; }\n"
   "  return c;\n"
   "}\n",
   {0xDEADBEEF}, "W32RT"},
  {"w32_mix32",
   "long w32_mix32(long a) {\n"
   "  unsigned int x = (unsigned int)a;\n"
   "  x ^= x >> 16;\n"
   "  x *= 0x85ebca6bU;\n"
   "  x ^= x >> 13;\n"
   "  return (long)x;\n"
   "}\n",
   {42}, "W32RT"},
  {"w32_crc_step",
   "long w32_crc_step(long crc, long data) {\n"
   "  unsigned int c = (unsigned int)crc ^ (unsigned int)data;\n"
   "  for (int i = 0; i < 8; ++i)\n"
   "    c = (c >> 1) ^ (c & 1 ? 0xEDB88320U : 0);\n"
   "  return (long)c;\n"
   "}\n",
   {0xFFFFFFFF, 0x41}, "W32RT"},
  {"w32_widening",
   "long w32_widening(long a, long b) {\n"
   "  unsigned int x = (unsigned int)a;\n"
   "  unsigned int y = (unsigned int)b;\n"
   "  return (long)x * y;\n"
   "}\n",
   {0x10000, 0x10000}, "W32RT"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(W32RT, X64W32RT, ::testing::ValuesIn(kX64W32), rtTCName);
