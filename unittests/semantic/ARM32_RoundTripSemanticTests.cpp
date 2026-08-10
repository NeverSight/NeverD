//===- ARM32_RoundTripSemanticTests.cpp - ARM32 lift roundtrip ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Semantic roundtrip verification for ARM32 instructions.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

TEST_P(ARM32RoundTrip, LiftVerify) { roundTripARM32(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kARM32CoreRT = {
  {"add_imm",
   "int add_imm(int a) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"add %0, %1, #42\" : \"=r\"(r) : \"r\"(a));\n"
   "  return r;\n"
   "}\n",
   {100}, "CoreRT"},

  {"add_reg",
   "int add_reg(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"add %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {50, 30}, "CoreRT"},

  {"sub_imm",
   "int sub_imm(int a) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"sub %0, %1, #10\" : \"=r\"(r) : \"r\"(a));\n"
   "  return r;\n"
   "}\n",
   {100}, "CoreRT"},

  {"sub_reg",
   "int sub_reg(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"sub %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {100, 30}, "CoreRT"},

  {"and_reg",
   "int and_reg(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"and %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0xFF00, 0x0FF0}, "CoreRT"},

  {"orr_reg",
   "int orr_reg(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"orr %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0xF0, 0x0F}, "CoreRT"},

  {"eor_reg",
   "int eor_reg(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"eor %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0xFF, 0x55}, "CoreRT"},

  {"mvn_reg",
   "int mvn_reg(int a) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"mvn %0, %1\" : \"=r\"(r) : \"r\"(a));\n"
   "  return r;\n"
   "}\n",
   {0}, "CoreRT"},

  {"mul_reg",
   "int mul_reg(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"mul %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {6, 7}, "MulRT"},

  {"lsl_imm",
   "int lsl_imm(int a) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"lsl %0, %1, #3\" : \"=r\"(r) : \"r\"(a));\n"
   "  return r;\n"
   "}\n",
   {5}, "ShiftRT"},

  {"lsr_imm",
   "int lsr_imm(int a) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"lsr %0, %1, #2\" : \"=r\"(r) : \"r\"(a));\n"
   "  return r;\n"
   "}\n",
   {100}, "ShiftRT"},

  {"clz_reg",
   "int clz_reg(int a) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"clz %0, %1\" : \"=r\"(r) : \"r\"(a));\n"
   "  return r;\n"
   "}\n",
   {0x100}, "ExtRT"},

  {"rev_reg",
   "int rev_reg(int a) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"rev %0, %1\" : \"=r\"(r) : \"r\"(a));\n"
   "  return r;\n"
   "}\n",
   {0x01020304}, "ExtRT"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(ARM32CoreRT, ARM32RoundTrip, ::testing::ValuesIn(kARM32CoreRT), rtTCName);
