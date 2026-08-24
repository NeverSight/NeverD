//===- X64_RoundTripSemanticTests.cpp - x64 lift roundtrip tests --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Semantic roundtrip verification for x86_64 instructions via C wrappers.
// Each function uses inline asm to exercise one or more instructions.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

TEST_P(X64RoundTrip, LiftVerify) { roundTripX64(GetParam()); }

// Helper: generate a one-operand inline asm wrapper
#define X64_UNARY(name, insn, inval)                                         \
  {#name,                                                                    \
   "long " #name "(long a) {\n"                                              \
   "  __asm__ volatile (\"" insn " %0\" : \"+r\"(a));\n"                     \
   "  return a;\n"                                                           \
   "}\n",                                                                    \
   {inval}, "CoreRT"}

#define X64_BINARY(name, insn, a, b)                                         \
  {#name,                                                                    \
   "long " #name "(long a, long b) {\n"                                      \
   "  __asm__ volatile (\"" insn " %1, %0\" : \"+r\"(a) : \"r\"(b));\n"     \
   "  return a;\n"                                                           \
   "}\n",                                                                    \
   {a, b}, "CoreRT"}

// clang-format off

static const std::vector<RoundTripTC> kX64CoreRT = {
  // ADD
  {"add_imm",
   "long add_imm(long a) {\n"
   "  __asm__ volatile (\"addq $42, %0\" : \"+r\"(a));\n"
   "  return a;\n"
   "}\n",
   {100}, "CoreRT"},

  X64_BINARY(add_reg, "addq", 50, 30),
  X64_BINARY(sub_reg, "subq", 100, 30),
  X64_BINARY(and_reg, "andq", 0xFF00, 0x0FF0),
  X64_BINARY(or_reg,  "orq",  0xF0, 0x0F),
  X64_BINARY(xor_reg, "xorq", 0xFF, 0x55),

  // SUB imm
  {"sub_imm",
   "long sub_imm(long a) {\n"
   "  __asm__ volatile (\"subq $10, %0\" : \"+r\"(a));\n"
   "  return a;\n"
   "}\n",
   {100}, "CoreRT"},

  // NOT
  X64_UNARY(not_reg, "notq", 0),

  // NEG
  X64_UNARY(neg_reg, "negq", 42),

  // INC / DEC
  X64_UNARY(inc_reg, "incq", 99),
  X64_UNARY(dec_reg, "decq", 100),

  // SHL
  {"shl_imm",
   "long shl_imm(long a) {\n"
   "  __asm__ volatile (\"shlq $3, %0\" : \"+r\"(a));\n"
   "  return a;\n"
   "}\n",
   {5}, "ShiftRT"},

  // SHR
  {"shr_imm",
   "long shr_imm(long a) {\n"
   "  __asm__ volatile (\"shrq $2, %0\" : \"+r\"(a));\n"
   "  return a;\n"
   "}\n",
   {100}, "ShiftRT"},

  // SAR
  {"sar_imm",
   "long sar_imm(long a) {\n"
   "  __asm__ volatile (\"sarq $2, %0\" : \"+r\"(a));\n"
   "  return a;\n"
   "}\n",
   {(uint64_t)-128}, "ShiftRT"},

  // ROL
  {"rol_imm",
   "long rol_imm(long a) {\n"
   "  __asm__ volatile (\"rolq $4, %0\" : \"+r\"(a));\n"
   "  return a;\n"
   "}\n",
   {0xF}, "ShiftRT"},

  // ROR
  {"ror_imm",
   "long ror_imm(long a) {\n"
   "  __asm__ volatile (\"rorq $4, %0\" : \"+r\"(a));\n"
   "  return a;\n"
   "}\n",
   {0xF0}, "ShiftRT"},

  // IMUL
  {"imul_reg",
   "long imul_reg(long a, long b) {\n"
   "  __asm__ volatile (\"imulq %1, %0\" : \"+r\"(a) : \"r\"(b));\n"
   "  return a;\n"
   "}\n",
   {6, 7}, "MulDivRT"},

  // BSWAP
  {"bswap64",
   "long bswap64(long a) {\n"
   "  __asm__ volatile (\"bswapq %0\" : \"+r\"(a));\n"
   "  return a;\n"
   "}\n",
   {0x0102030405060708ULL}, "ExtRT"},

  // BSF
  {"bsf_reg",
   "long bsf_reg(long a) {\n"
   "  long r;\n"
   "  __asm__ volatile (\"bsfq %1, %0\" : \"=r\"(r) : \"r\"(a));\n"
   "  return r;\n"
   "}\n",
   {0x80}, "ExtRT"},

  // BSR
  {"bsr_reg",
   "long bsr_reg(long a) {\n"
   "  long r;\n"
   "  __asm__ volatile (\"bsrq %1, %0\" : \"=r\"(r) : \"r\"(a));\n"
   "  return r;\n"
   "}\n",
   {0x80}, "ExtRT"},

  // POPCNT
  {"popcnt_reg",
   "long popcnt_reg(long a) {\n"
   "  long r;\n"
   "  __asm__ volatile (\"popcntq %1, %0\" : \"=r\"(r) : \"r\"(a));\n"
   "  return r;\n"
   "}\n",
   {0xFF}, "ExtRT"},

  // LZCNT
  {"lzcnt_reg",
   "long lzcnt_reg(long a) {\n"
   "  long r;\n"
   "  __asm__ volatile (\"lzcntq %1, %0\" : \"=r\"(r) : \"r\"(a));\n"
   "  return r;\n"
   "}\n",
   {0x100}, "ExtRT"},

  // TZCNT
  {"tzcnt_reg",
   "long tzcnt_reg(long a) {\n"
   "  long r;\n"
   "  __asm__ volatile (\"tzcntq %1, %0\" : \"=r\"(r) : \"r\"(a));\n"
   "  return r;\n"
   "}\n",
   {0x100}, "ExtRT"},

  // LEA (complex)
  {"lea_sib",
   "long lea_sib(long a, long b) {\n"
   "  long r;\n"
   "  __asm__ volatile (\"leaq 8(%1,%2,4), %0\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {100, 10}, "CoreRT"},

  // Multi-instruction: CMP + SETcc
  {"cmp_setz",
   "long cmp_setz(long a, long b) {\n"
   "  long r = 0;\n"
   "  __asm__ volatile (\n"
   "    \"cmpq %2, %1\\n\\t\"\n"
   "    \"sete %b0\"\n"
   "    : \"+r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {42, 42}, "CondRT"},

  // Multi-instruction: CMP + CMOVcc
  {"cmovz",
   "long cmovz(long a, long b) {\n"
   "  long r = 0;\n"
   "  __asm__ volatile (\n"
   "    \"cmpq %1, %1\\n\\t\"\n"
   "    \"cmovzq %2, %0\"\n"
   "    : \"+r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {42, 99}, "CondRT"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(CoreRT, X64RoundTrip, ::testing::ValuesIn(kX64CoreRT), rtTCName);

class X64SysRegRT : public SemanticRoundTripFixture {};

TEST_F(X64SysRegRT, StrR32ZeroExt) {
  // A 32-bit register destination receives the 16-bit task-register selector
  // zero-extended to 32 bits.  The nonzero upper-word sentinel distinguishes
  // that architectural write from a 16-bit partial-register update.
  roundTripX64({"str_r32_zero_ext",
                "unsigned long str_r32_zero_ext(unsigned long x) {\n"
                "  __asm__ volatile (\"str %k0\" : \"+r\"(x));\n"
                "  return x;\n"
                "}\n",
                {0xA5A55A5ADEADBEEFULL}, "SysRegRT", 1});
}
