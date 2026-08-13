//===- AArch64_RoundTripSemanticTests.cpp - AArch64 lift roundtrip --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Semantic roundtrip verification for AArch64 instructions.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

TEST_P(AArch64RoundTrip, LiftVerify) { roundTripAArch64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kA64CoreRT = {
  {"add_imm",
   "long add_imm(long a) {\n"
   "  long r;\n"
   "  __asm__ volatile (\"add %0, %1, #42\" : \"=r\"(r) : \"r\"(a));\n"
   "  return r;\n"
   "}\n",
   {100}, "CoreRT"},

  {"add_reg",
   "long add_reg(long a, long b) {\n"
   "  long r;\n"
   "  __asm__ volatile (\"add %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {50, 30}, "CoreRT"},

  {"sub_imm",
   "long sub_imm(long a) {\n"
   "  long r;\n"
   "  __asm__ volatile (\"sub %0, %1, #10\" : \"=r\"(r) : \"r\"(a));\n"
   "  return r;\n"
   "}\n",
   {100}, "CoreRT"},

  {"sub_reg",
   "long sub_reg(long a, long b) {\n"
   "  long r;\n"
   "  __asm__ volatile (\"sub %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {100, 30}, "CoreRT"},

  {"and_reg",
   "long and_reg(long a, long b) {\n"
   "  long r;\n"
   "  __asm__ volatile (\"and %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0xFF00, 0x0FF0}, "CoreRT"},

  {"orr_reg",
   "long orr_reg(long a, long b) {\n"
   "  long r;\n"
   "  __asm__ volatile (\"orr %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0xF0, 0x0F}, "CoreRT"},

  {"eor_reg",
   "long eor_reg(long a, long b) {\n"
   "  long r;\n"
   "  __asm__ volatile (\"eor %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0xFF, 0x55}, "CoreRT"},

  {"neg_reg",
   "long neg_reg(long a) {\n"
   "  long r;\n"
   "  __asm__ volatile (\"neg %0, %1\" : \"=r\"(r) : \"r\"(a));\n"
   "  return r;\n"
   "}\n",
   {42}, "CoreRT"},

  {"mul_reg",
   "long mul_reg(long a, long b) {\n"
   "  long r;\n"
   "  __asm__ volatile (\"mul %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {6, 7}, "MulDivRT"},

  {"udiv_reg",
   "long udiv_reg(long a, long b) {\n"
   "  long r;\n"
   "  __asm__ volatile (\"udiv %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {100, 7}, "MulDivRT"},

  {"sdiv_reg",
   "long sdiv_reg(long a, long b) {\n"
   "  long r;\n"
   "  __asm__ volatile (\"sdiv %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {100, 7}, "MulDivRT"},

  {"lsl_imm",
   "long lsl_imm(long a) {\n"
   "  long r;\n"
   "  __asm__ volatile (\"lsl %0, %1, #4\" : \"=r\"(r) : \"r\"(a));\n"
   "  return r;\n"
   "}\n",
   {1}, "ShiftRT"},

  {"lsr_imm",
   "long lsr_imm(long a) {\n"
   "  long r;\n"
   "  __asm__ volatile (\"lsr %0, %1, #2\" : \"=r\"(r) : \"r\"(a));\n"
   "  return r;\n"
   "}\n",
   {100}, "ShiftRT"},

  {"clz_reg",
   "long clz_reg(long a) {\n"
   "  long r;\n"
   "  __asm__ volatile (\"clz %0, %1\" : \"=r\"(r) : \"r\"(a));\n"
   "  return r;\n"
   "}\n",
   {0x100}, "BitRT"},

  {"rbit_reg",
   "long rbit_reg(long a) {\n"
   "  long r;\n"
   "  __asm__ volatile (\"rbit %0, %1\" : \"=r\"(r) : \"r\"(a));\n"
   "  return r;\n"
   "}\n",
   {1}, "BitRT"},

  {"rev_reg",
   "long rev_reg(long a) {\n"
   "  long r;\n"
   "  __asm__ volatile (\"rev %0, %1\" : \"=r\"(r) : \"r\"(a));\n"
   "  return r;\n"
   "}\n",
   {0x0102030405060708ULL}, "BitRT"},

  {"madd_reg",
   "long madd_reg(long a, long b, long c) {\n"
   "  long r;\n"
   "  __asm__ volatile (\"madd %0, %1, %2, %3\" : \"=r\"(r) : \"r\"(a), \"r\"(b), \"r\"(c));\n"
   "  return r;\n"
   "}\n",
   {6, 7, 10}, "MulDivRT"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(A64CoreRT, AArch64RoundTrip, ::testing::ValuesIn(kA64CoreRT), rtTCName);
