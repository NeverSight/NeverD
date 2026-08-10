//===- X64_StringControlRTTests.cpp - x86 string/control roundtrip -*- C++ -*-//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests x86 string operations (memset/memcpy/strcmp patterns) and miscellaneous
// control operations through the full lift pipeline.  These use C standard
// library patterns that the compiler lowers to REP STOS/MOVS/CMPS sequences.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64StringCtrlRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64StringCtrlRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kX64StringCtrl = {

  // ===== Simple accumulation loop =====
  {"sum_loop",
   "long sum_loop(long n) {\n"
   "  long sum = 0;\n"
   "  for (long i = 1; i <= n; ++i)\n"
   "    sum += i;\n"
   "  return sum;\n"
   "}\n",
   {10}, "StringCtrl", 1},

  // ===== Fibonacci =====
  {"fibonacci",
   "long fibonacci(long n) {\n"
   "  long a = 0, b = 1;\n"
   "  for (long i = 0; i < n; ++i) {\n"
   "    long t = a + b;\n"
   "    a = b;\n"
   "    b = t;\n"
   "  }\n"
   "  return a;\n"
   "}\n",
   {10}, "StringCtrl", 1},

  // ===== GCD =====
  {"gcd",
   "long gcd(long a, long b) {\n"
   "  unsigned long ua = (unsigned long)a, ub = (unsigned long)b;\n"
   "  while (ub) {\n"
   "    unsigned long t = ub;\n"
   "    ub = ua % ub;\n"
   "    ua = t;\n"
   "  }\n"
   "  return (long)ua;\n"
   "}\n",
   {48, 18}, "StringCtrl", 1},

  // ===== Popcount (Brian Kernighan) =====
  {"popcount_bk",
   "long popcount_bk(long a) {\n"
   "  unsigned long v = (unsigned long)a;\n"
   "  long count = 0;\n"
   "  while (v) {\n"
   "    v &= v - 1;\n"
   "    ++count;\n"
   "  }\n"
   "  return count;\n"
   "}\n",
   {0xFF00FF00FF00FF00ULL}, "StringCtrl", 1},

  // ===== Switch-case pattern =====
  {"switch_pattern",
   "long switch_pattern(long a) {\n"
   "  switch ((int)a) {\n"
   "    case 0: return 100;\n"
   "    case 1: return 200;\n"
   "    case 2: return 300;\n"
   "    case 3: return 400;\n"
   "    case 4: return 500;\n"
   "    default: return -1;\n"
   "  }\n"
   "}\n",
   {3}, "StringCtrl", 1},

  // ===== Unsigned division by constant (compiler uses MULHI trick) =====
  {"div_by_7",
   "long div_by_7(long a) {\n"
   "  return (unsigned long)a / 7;\n"
   "}\n",
   {1000}, "StringCtrl", 1},

  // ===== Modulo by constant =====
  {"mod_by_10",
   "long mod_by_10(long a) {\n"
   "  return (unsigned long)a % 10;\n"
   "}\n",
   {12345}, "StringCtrl", 1},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(StringCtrl, X64StringCtrlRT,
                         ::testing::ValuesIn(kX64StringCtrl), rtTCName);
