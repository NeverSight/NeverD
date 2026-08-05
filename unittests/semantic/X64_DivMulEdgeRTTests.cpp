//===- X64_DivMulEdgeRTTests.cpp - Division edge cases roundtrip -*- C++ -*-===//
//
// Tests x86_64 DIV/IDIV/MUL/IMUL edge cases through lift pipeline.
// Specifically designed to catch:
//   - i128 sdiv/srem → __divti3 library call regression (#7)
//   - XOR EDX,EDX zero recognition (#8)
//   - Sign-extension patterns (CDQ/CQO/CWD/CBW)
//   - 8/16/32/64-bit operand sizes
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64DivMulRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64DivMulRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kX64DivMul = {
  // ========== 32-bit signed division (CDQ+IDIV) ==========
  {"idiv32_pos",
   "long idiv32_pos(long a, long b) {\n"
   "  int q = (int)a / (int)b;\n"
   "  return (long)q;\n"
   "}\n",
   {42, 7}, "DivMulEdge"},

  {"idiv32_neg",
   "long idiv32_neg(long a, long b) {\n"
   "  int q = (int)a / (int)b;\n"
   "  return (long)q;\n"
   "}\n",
   {(uint64_t)(int64_t)-42, 7}, "DivMulEdge"},

  {"imod32_pos",
   "long imod32_pos(long a, long b) {\n"
   "  int r = (int)a % (int)b;\n"
   "  return (long)r;\n"
   "}\n",
   {43, 7}, "DivMulEdge"},

  {"imod32_neg",
   "long imod32_neg(long a, long b) {\n"
   "  int r = (int)a % (int)b;\n"
   "  return (long)r;\n"
   "}\n",
   {(uint64_t)(int64_t)-43, 7}, "DivMulEdge"},

  // ========== 32-bit unsigned division (XOR EDX,EDX + DIV) ==========
  {"udiv32",
   "long udiv32(long a, long b) {\n"
   "  unsigned q = (unsigned)a / (unsigned)b;\n"
   "  return (long)q;\n"
   "}\n",
   {100, 7}, "DivMulEdge"},

  {"umod32",
   "long umod32(long a, long b) {\n"
   "  unsigned r = (unsigned)a % (unsigned)b;\n"
   "  return (long)r;\n"
   "}\n",
   {100, 7}, "DivMulEdge"},

  {"udiv32_large",
   "long udiv32_large(long a, long b) {\n"
   "  unsigned q = (unsigned)a / (unsigned)b;\n"
   "  return (long)q;\n"
   "}\n",
   {0xFFFFFFFF, 3}, "DivMulEdge"},

  // ========== 64-bit signed division (CQO+IDIV) ==========
  {"idiv64_pos",
   "long idiv64_pos(long a, long b) {\n"
   "  return a / b;\n"
   "}\n",
   {1000, 7}, "DivMulEdge"},

  {"idiv64_neg",
   "long idiv64_neg(long a, long b) {\n"
   "  return a / b;\n"
   "}\n",
   {(uint64_t)(int64_t)-1000, 7}, "DivMulEdge"},

  {"imod64_pos",
   "long imod64_pos(long a, long b) {\n"
   "  return a % b;\n"
   "}\n",
   {1000, 7}, "DivMulEdge"},

  {"imod64_neg",
   "long imod64_neg(long a, long b) {\n"
   "  return a % b;\n"
   "}\n",
   {(uint64_t)(int64_t)-1000, 7}, "DivMulEdge"},

  // ========== 64-bit unsigned division (XOR RDX,RDX + DIV) ==========
  {"udiv64",
   "long udiv64(long a, long b) {\n"
   "  return (long)((unsigned long)a / (unsigned long)b);\n"
   "}\n",
   {1000000, 7}, "DivMulEdge"},

  {"umod64",
   "long umod64(long a, long b) {\n"
   "  return (long)((unsigned long)a % (unsigned long)b);\n"
   "}\n",
   {1000000, 7}, "DivMulEdge"},

  // ========== divmod combined (both quotient and remainder) ==========
  {"divmod32",
   "long divmod32(long a, long b) {\n"
   "  int q = (int)a / (int)b;\n"
   "  int r = (int)a % (int)b;\n"
   "  return ((long)(unsigned)q << 32) | (unsigned)r;\n"
   "}\n",
   {123, 10}, "DivMulEdge"},

  {"divmod64",
   "long divmod64(long a, long b) {\n"
   "  long q = a / b;\n"
   "  long r = a % b;\n"
   "  return q ^ r;\n"
   "}\n",
   {123456, 1000}, "DivMulEdge"},

  // ========== 64-bit multiply full (IMUL two-operand) ==========
  {"imul64_2op",
   "long imul64_2op(long a, long b) {\n"
   "  return a * b;\n"
   "}\n",
   {12345, 6789}, "DivMulEdge"},

  {"imul64_neg",
   "long imul64_neg(long a, long b) {\n"
   "  return a * b;\n"
   "}\n",
   {(uint64_t)(int64_t)-100, 42}, "DivMulEdge"},

  // ========== 32-bit multiply (IMUL r32,r/m32) ==========
  {"imul32",
   "long imul32(long a, long b) {\n"
   "  int r = (int)a * (int)b;\n"
   "  return (long)r;\n"
   "}\n",
   {1234, 5678}, "DivMulEdge"},

  // ========== MUL (unsigned widening multiply) ==========
  {"umul_hi32",
   "long umul_hi32(long a, long b) {\n"
   "  unsigned long long r = (unsigned long long)(unsigned)a * (unsigned)b;\n"
   "  return (long)(r >> 32);\n"
   "}\n",
   {0xFFFFFFFF, 2}, "DivMulEdge"},

  // ========== Division by power of 2 (compiler may optimize) ==========
  {"div_pow2",
   "long div_pow2(long a) {\n"
   "  return a / 8;\n"
   "}\n",
   {100}, "DivMulEdge"},

  {"div_pow2_neg",
   "long div_pow2_neg(long a) {\n"
   "  return a / 8;\n"
   "}\n",
   {(uint64_t)(int64_t)-100}, "DivMulEdge"},

  // ========== Modulo power of 2 ==========
  {"mod_pow2",
   "long mod_pow2(long a) {\n"
   "  return a % 16;\n"
   "}\n",
   {100}, "DivMulEdge"},

  // ========== Division by constant (compiler mul + shift trick) ==========
  {"div_const3",
   "long div_const3(long a) {\n"
   "  return (unsigned long)a / 3;\n"
   "}\n",
   {1000000}, "DivMulEdge"},

  {"div_const7",
   "long div_const7(long a) {\n"
   "  return (unsigned long)a / 7;\n"
   "}\n",
   {1000000}, "DivMulEdge"},

  // ========== Sign extension patterns ==========
  {"sext_8_to_32",
   "long sext_8_to_32(long a) {\n"
   "  return (long)(int)(signed char)a;\n"
   "}\n",
   {0x80}, "DivMulEdge"},

  {"sext_16_to_32",
   "long sext_16_to_32(long a) {\n"
   "  return (long)(int)(short)a;\n"
   "}\n",
   {0x8000}, "DivMulEdge"},

  {"sext_32_to_64",
   "long sext_32_to_64(long a) {\n"
   "  return (long)(int)a;\n"
   "}\n",
   {0x80000000ULL}, "DivMulEdge"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(DivMulEdge, X64DivMulRT,
                         ::testing::ValuesIn(kX64DivMul), rtTCName);
