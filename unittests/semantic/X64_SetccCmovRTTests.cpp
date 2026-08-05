//===- X64_SetccCmovRTTests.cpp - SETcc + CMOV patterns RT ----*- C++ -*-===//
//
// Tests x86_64 SETcc/CMOVcc instruction families through lift pipeline.
// These exercise flag computation + conditional selection — a rich
// source of optimizer bugs (flag folding, condition inversion, etc).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SetCmovRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SetCmovRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kX64SetCmov = {
  // ========== SETcc patterns ==========
  {"sete_true",   "long f(long a, long b) { return a == b ? 1 : 0; }\n", {42, 42}, "SetCmov"},
  {"sete_false",  "long f(long a, long b) { return a == b ? 1 : 0; }\n", {42, 43}, "SetCmov"},
  {"setne_true",  "long f(long a, long b) { return a != b ? 1 : 0; }\n", {42, 43}, "SetCmov"},
  {"setne_false", "long f(long a, long b) { return a != b ? 1 : 0; }\n", {42, 42}, "SetCmov"},
  {"setl_true",   "long f(long a, long b) { return a < b ? 1 : 0; }\n",  {(uint64_t)(int64_t)-1, 1}, "SetCmov"},
  {"setl_false",  "long f(long a, long b) { return a < b ? 1 : 0; }\n",  {1, (uint64_t)(int64_t)-1}, "SetCmov"},
  {"setg_true",   "long f(long a, long b) { return a > b ? 1 : 0; }\n",  {100, 42}, "SetCmov"},
  {"setg_false",  "long f(long a, long b) { return a > b ? 1 : 0; }\n",  {42, 100}, "SetCmov"},
  {"setle_true",  "long f(long a, long b) { return a <= b ? 1 : 0; }\n", {42, 42}, "SetCmov"},
  {"setle_false", "long f(long a, long b) { return a <= b ? 1 : 0; }\n", {43, 42}, "SetCmov"},
  {"setge_true",  "long f(long a, long b) { return a >= b ? 1 : 0; }\n", {42, 42}, "SetCmov"},
  {"setge_false", "long f(long a, long b) { return a >= b ? 1 : 0; }\n", {41, 42}, "SetCmov"},

  // unsigned
  {"setb_true",   "long f(long a, long b) { return (unsigned long)a < (unsigned long)b ? 1 : 0; }\n", {1, 0xFFFFFFFFFFFFFFFFULL}, "SetCmov"},
  {"setb_false",  "long f(long a, long b) { return (unsigned long)a < (unsigned long)b ? 1 : 0; }\n", {0xFFFFFFFFFFFFFFFFULL, 1}, "SetCmov"},
  {"seta_true",   "long f(long a, long b) { return (unsigned long)a > (unsigned long)b ? 1 : 0; }\n", {0xFFFFFFFFFFFFFFFFULL, 1}, "SetCmov"},
  {"setbe_true",  "long f(long a, long b) { return (unsigned long)a <= (unsigned long)b ? 1 : 0; }\n", {1, 1}, "SetCmov"},

  // ========== CMOV with complex conditions ==========
  {"cmov_min_signed",
   "long f(long a, long b) { return a < b ? a : b; }\n",
   {(uint64_t)(int64_t)-100, 100}, "SetCmov"},

  {"cmov_max_signed",
   "long f(long a, long b) { return a > b ? a : b; }\n",
   {(uint64_t)(int64_t)-100, 100}, "SetCmov"},

  {"cmov_min_unsigned",
   "long f(long a, long b) {\n"
   "  return (unsigned long)a < (unsigned long)b ? a : b;\n"
   "}\n",
   {1, 0xFFFFFFFFFFFFFFFFULL}, "SetCmov"},

  {"cmov_max_unsigned",
   "long f(long a, long b) {\n"
   "  return (unsigned long)a > (unsigned long)b ? a : b;\n"
   "}\n",
   {1, 0xFFFFFFFFFFFFFFFFULL}, "SetCmov"},

  // ========== CMOV chain (multi-condition) ==========
  {"cmov_clamp",
   "long f(long val, long lo, long hi) {\n"
   "  if (val < lo) val = lo;\n"
   "  if (val > hi) val = hi;\n"
   "  return val;\n"
   "}\n",
   {300, 0, 255}, "SetCmov"},

  {"cmov_clamp_in",
   "long f(long val, long lo, long hi) {\n"
   "  if (val < lo) val = lo;\n"
   "  if (val > hi) val = hi;\n"
   "  return val;\n"
   "}\n",
   {100, 0, 255}, "SetCmov"},

  // ========== TEST pattern (AND + flags) ==========
  {"test_bit0",
   "long f(long a) { return a & 1; }\n",
   {42}, "SetCmov"},

  {"test_bit7",
   "long f(long a) { return (a >> 7) & 1; }\n",
   {0x80}, "SetCmov"},

  {"test_zero",
   "long f(long a) { return a == 0 ? 42 : 0; }\n",
   {0}, "SetCmov"},

  {"test_nonzero",
   "long f(long a) { return a != 0 ? 42 : 0; }\n",
   {1}, "SetCmov"},

  // ========== CMP + branch patterns ==========
  {"cmp_branch_eq",
   "long f(long a, long b) {\n"
   "  if (a == b) return a + 1;\n"
   "  return a - 1;\n"
   "}\n",
   {42, 42}, "SetCmov"},

  {"cmp_branch_ne",
   "long f(long a, long b) {\n"
   "  if (a == b) return a + 1;\n"
   "  return a - 1;\n"
   "}\n",
   {42, 43}, "SetCmov"},

  // ========== 32-bit setcc (tests EAX zero-extend, regression for #36) ==========
  {"sete32",
   "long f(long a, long b) {\n"
   "  return (long)((unsigned)a == (unsigned)b);\n"
   "}\n",
   {42, 42}, "SetCmov"},

  {"sete32_false",
   "long f(long a, long b) {\n"
   "  return (long)((unsigned)a == (unsigned)b);\n"
   "}\n",
   {42, 43}, "SetCmov"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(SetCmov, X64SetCmovRT,
                         ::testing::ValuesIn(kX64SetCmov), rtTCName);
