//===- AllPlatform_AlgorithmRTTests.cpp - Cross-platform algo RT -*- C++ -*-===//
//
// Tests algorithmic patterns that exercise multi-BB control flow, loops,
// and complex data dependencies across all three architectures.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

// --- x64 ---
class X64AlgoRT2 : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64AlgoRT2, Verify) { roundTripX64(GetParam()); }

// --- AArch64 ---
class A64AlgoRT2 : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64AlgoRT2, Verify) { roundTripAArch64(GetParam()); }

// --- ARM32 ---
class ARM32AlgoRT2 : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32AlgoRT2, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static const char *kFactorial =
  "long factorial(long n) {\n"
  "  long r = 1;\n"
  "  for (long i = 2; i <= n; ++i) r *= i;\n"
  "  return r;\n"
  "}\n";

static const char *kFib =
  "long fib(long n) {\n"
  "  if (n <= 1) return n;\n"
  "  long a = 0, b = 1;\n"
  "  for (long i = 2; i <= n; ++i) { long t = a + b; a = b; b = t; }\n"
  "  return b;\n"
  "}\n";

static const char *kSumDigits =
  "long sum_digits(long n) {\n"
  "  long s = 0;\n"
  "  if (n < 0) n = -n;\n"
  "  while (n > 0) { s += n % 10; n /= 10; }\n"
  "  return s;\n"
  "}\n";

static const char *kIsPrime =
  "long is_prime(long n) {\n"
  "  if (n < 2) return 0;\n"
  "  for (long i = 2; i * i <= n; ++i)\n"
  "    if (n % i == 0) return 0;\n"
  "  return 1;\n"
  "}\n";

static const char *kCountBits =
  "long count_bits(long n) {\n"
  "  long c = 0;\n"
  "  unsigned long x = (unsigned long)n;\n"
  "  while (x) { c++; x &= x - 1; }\n"
  "  return c;\n"
  "}\n";

// ARM32 versions use int
static const char *kFactorial32 =
  "int factorial(int n) {\n"
  "  int r = 1;\n"
  "  for (int i = 2; i <= n; ++i) r *= i;\n"
  "  return r;\n"
  "}\n";

static const char *kFib32 =
  "int fib(int n) {\n"
  "  if (n <= 1) return n;\n"
  "  int a = 0, b = 1;\n"
  "  for (int i = 2; i <= n; ++i) { int t = a + b; a = b; b = t; }\n"
  "  return b;\n"
  "}\n";

static const char *kSumDigits32 =
  "int sum_digits(int n) {\n"
  "  int s = 0;\n"
  "  if (n < 0) n = -n;\n"
  "  while (n > 0) { s += n % 10; n /= 10; }\n"
  "  return s;\n"
  "}\n";

static const char *kIsPrime32 =
  "int is_prime(int n) {\n"
  "  if (n < 2) return 0;\n"
  "  for (int i = 2; i * i <= n; ++i)\n"
  "    if (n % i == 0) return 0;\n"
  "  return 1;\n"
  "}\n";

static const char *kCountBits32 =
  "int count_bits(int n) {\n"
  "  int c = 0;\n"
  "  unsigned x = (unsigned)n;\n"
  "  while (x) { c++; x &= x - 1; }\n"
  "  return c;\n"
  "}\n";

static const std::vector<RoundTripTC> kX64Algo = {
  {"x64_factorial", kFactorial, {10}, "X64Algo"},
  {"x64_fib", kFib, {15}, "X64Algo"},
  {"x64_sum_digits", kSumDigits, {123456}, "X64Algo"},
  {"x64_is_prime_yes", kIsPrime, {97}, "X64Algo"},
  {"x64_is_prime_no", kIsPrime, {100}, "X64Algo"},
  {"x64_count_bits", kCountBits, {0xFF00FF}, "X64Algo"},
};

static const std::vector<RoundTripTC> kA64Algo = {
  {"a64_factorial", kFactorial, {10}, "A64Algo"},
  {"a64_fib", kFib, {15}, "A64Algo"},
  {"a64_sum_digits", kSumDigits, {123456}, "A64Algo"},
  {"a64_is_prime_yes", kIsPrime, {97}, "A64Algo"},
  {"a64_is_prime_no", kIsPrime, {100}, "A64Algo"},
  {"a64_count_bits", kCountBits, {0xFF00FF}, "A64Algo"},
};

static const std::vector<RoundTripTC> kARM32Algo = {
  {"arm_factorial", kFactorial32, {10}, "ARM32Algo"},
  {"arm_fib", kFib32, {15}, "ARM32Algo"},
  {"arm_sum_digits", kSumDigits32, {123456}, "ARM32Algo"},
  {"arm_is_prime_yes", kIsPrime32, {97}, "ARM32Algo"},
  {"arm_is_prime_no", kIsPrime32, {100}, "ARM32Algo"},
  {"arm_count_bits", kCountBits32, {0xFF00FF}, "ARM32Algo"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(X64Algo2, X64AlgoRT2,
                         ::testing::ValuesIn(kX64Algo), rtTCName);
INSTANTIATE_TEST_SUITE_P(A64Algo2, A64AlgoRT2,
                         ::testing::ValuesIn(kA64Algo), rtTCName);
INSTANTIATE_TEST_SUITE_P(ARM32Algo2, ARM32AlgoRT2,
                         ::testing::ValuesIn(kARM32Algo), rtTCName);
