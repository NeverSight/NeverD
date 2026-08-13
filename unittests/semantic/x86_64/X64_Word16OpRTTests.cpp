//===- X64_Word16OpRTTests.cpp - 16-bit + multi-arg RT --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests x86_64 16-bit operations (AX/BX/CX/DX), multi-argument functions,
// switch patterns, and nested loops. These exercise sub-register handling
// for the 16-bit register file (regression for #3 sub-register aliasing).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64Word16RT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64Word16RT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kX64Word16 = {
  // ========== 16-bit arithmetic ==========
  {"add16",
   "long add16(long a, long b) {\n"
   "  return (long)(unsigned short)((unsigned short)a + (unsigned short)b);\n"
   "}\n",
   {0xFFFF, 1}, "Word16"},

  {"sub16",
   "long sub16(long a, long b) {\n"
   "  return (long)(unsigned short)((unsigned short)a - (unsigned short)b);\n"
   "}\n",
   {0x1000, 1}, "Word16"},

  {"mul16",
   "long mul16(long a, long b) {\n"
   "  return (long)(unsigned short)((unsigned short)a * (unsigned short)b);\n"
   "}\n",
   {100, 200}, "Word16"},

  // 16-bit push/pop (0x66): stack delta must be 2, not 8.
  {"pushpop_ax",
   "long pushpop_ax(long x) {\n"
   "  unsigned short v = (unsigned short)x, w;\n"
   "  __asm__ volatile(\"pushw %%ax\\n\\tpopw %%bx\" : \"=b\"(w) : \"a\"(v) : \"memory\");\n"
   "  return (long)w;\n"
   "}\n",
   {0xA5A5, 0x5A5A}, "Word16"},

  // ========== 8-bit arithmetic ==========
  {"add8",
   "long add8(long a, long b) {\n"
   "  return (long)(unsigned char)((unsigned char)a + (unsigned char)b);\n"
   "}\n",
   {0xFF, 1}, "Word16"},

  {"sub8",
   "long sub8(long a, long b) {\n"
   "  return (long)(unsigned char)((unsigned char)a - (unsigned char)b);\n"
   "}\n",
   {0x10, 1}, "Word16"},

  // ========== Multi-argument (4-6 args) ==========
  {"args4",
   "long args4(long a, long b, long c, long d) {\n"
   "  return a * b + c * d;\n"
   "}\n",
   {2, 3, 4, 5}, "Word16"},

  {"args5",
   "long args5(long a, long b, long c, long d, long e) {\n"
   "  return a + b + c + d + e;\n"
   "}\n",
   {10, 20, 30, 40, 50}, "Word16"},

  {"args6",
   "long args6(long a, long b, long c, long d, long e, long f) {\n"
   "  return (a - b) * (c - d) + (e - f);\n"
   "}\n",
   {100, 10, 200, 20, 300, 30}, "Word16"},

  // ========== Switch/table ==========
  {"switch5",
   "long switch5(long a) {\n"
   "  if (a == 0) return 10;\n"
   "  if (a == 1) return 20;\n"
   "  if (a == 2) return 30;\n"
   "  if (a == 3) return 40;\n"
   "  if (a == 4) return 50;\n"
   "  return -1;\n"
   "}\n",
   {3}, "Word16"},

  {"switch5_default",
   "long switch5_default(long a) {\n"
   "  if (a == 0) return 10;\n"
   "  if (a == 1) return 20;\n"
   "  if (a == 2) return 30;\n"
   "  if (a == 3) return 40;\n"
   "  if (a == 4) return 50;\n"
   "  return -1;\n"
   "}\n",
   {99}, "Word16"},

  // ========== Nested loop ==========
  {"nested_loop",
   "long nested_loop(long n) {\n"
   "  long sum = 0;\n"
   "  for (long i = 0; i < n; ++i)\n"
   "    for (long j = 0; j <= i; ++j)\n"
   "      sum += j;\n"
   "  return sum;\n"
   "}\n",
   {5}, "Word16"},

  // ========== Multiple return paths ==========
  {"multi_ret",
   "long multi_ret(long a, long b) {\n"
   "  if (a < 0) return -1;\n"
   "  if (b < 0) return -2;\n"
   "  if (a > b) return a - b;\n"
   "  if (a < b) return b - a;\n"
   "  return 0;\n"
   "}\n",
   {42, 100}, "Word16"},

  // ========== Bitfield manipulation ==========
  {"pack_bytes",
   "long pack_bytes(long a, long b, long c, long d) {\n"
   "  return ((a & 0xFF) << 24) | ((b & 0xFF) << 16)\n"
   "       | ((c & 0xFF) << 8) | (d & 0xFF);\n"
   "}\n",
   {0xDE, 0xAD, 0xBE, 0xEF}, "Word16"},

  {"unpack_bytes",
   "long unpack_bytes(long a) {\n"
   "  long b0 = a & 0xFF;\n"
   "  long b1 = (a >> 8) & 0xFF;\n"
   "  long b2 = (a >> 16) & 0xFF;\n"
   "  long b3 = (a >> 24) & 0xFF;\n"
   "  return b0 + b1 + b2 + b3;\n"
   "}\n",
   {0x01020304}, "Word16"},

  // ========== Arithmetic with overflow detection ==========
  {"add_overflow",
   "long add_overflow(long a, long b) {\n"
   "  unsigned long r = (unsigned long)a + (unsigned long)b;\n"
   "  return r < (unsigned long)a ? -1 : (long)r;\n"
   "}\n",
   {0xFFFFFFFFFFFFFFFFULL, 1}, "Word16"},

  {"add_no_overflow",
   "long add_no_overflow(long a, long b) {\n"
   "  unsigned long r = (unsigned long)a + (unsigned long)b;\n"
   "  return r < (unsigned long)a ? -1 : (long)r;\n"
   "}\n",
   {100, 200}, "Word16"},

  // ========== XCHG pattern ==========
  {"sort2",
   "long sort2(long a, long b) {\n"
   "  if (a > b) { long t = a; a = b; b = t; }\n"
   "  return (a << 32) | (b & 0xFFFFFFFF);\n"
   "}\n",
   {100, 42}, "Word16"},

  // ========== Polynomial eval (Horner's method) ==========
  {"horner",
   "long horner(long x) {\n"
   "  return ((3 * x + 2) * x + 1) * x + 5;\n"
   "}\n",
   {10}, "Word16"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(Word16, X64Word16RT,
                         ::testing::ValuesIn(kX64Word16), rtTCName);
