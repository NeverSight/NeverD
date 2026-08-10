//===- AllPlatform_AlgorithmTests.cpp - Algorithm roundtrip tests -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests common algorithm patterns: hashing, encoding, searching, and
// mathematical computations through roundtrip verification.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64AlgoRT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64AlgoRT, Verify) { roundTripX64(GetParam()); }

class A64AlgoRT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64AlgoRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32AlgoRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32AlgoRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kX64Algo = {
  {"algo_sdbm_hash",
   "long algo_sdbm_hash(long a, long b) {\n"
   "  unsigned long h = 0;\n"
   "  unsigned char buf[8];\n"
   "  *(long*)buf = a;\n"
   "  for (int i = 0; i < 8; ++i)\n"
   "    h = buf[i] + (h << 6) + (h << 16) - h;\n"
   "  return (long)h;\n"
   "}\n",
   {0x48656C6C6FULL, 0}, "AlgoRT"},

  {"algo_fnv1a",
   "long algo_fnv1a(long data) {\n"
   "  unsigned long h = 0xcbf29ce484222325ULL;\n"
   "  unsigned char *p = (unsigned char*)&data;\n"
   "  for (int i = 0; i < 8; ++i) {\n"
   "    h ^= p[i];\n"
   "    h *= 0x100000001b3ULL;\n"
   "  }\n"
   "  return (long)h;\n"
   "}\n",
   {0x48656C6C6FULL}, "AlgoRT"},

  {"algo_prime_check",
   "long algo_prime_check(long n) {\n"
   "  if (n < 2) return 0;\n"
   "  if (n < 4) return 1;\n"
   "  if (n % 2 == 0 || n % 3 == 0) return 0;\n"
   "  for (long i = 5; i * i <= n; i += 6)\n"
   "    if (n % i == 0 || n % (i+2) == 0) return 0;\n"
   "  return 1;\n"
   "}\n",
   {97}, "AlgoRT"},

  {"algo_count_primes",
   "long algo_count_primes(long n) {\n"
   "  long c = 0;\n"
   "  for (long i = 2; i <= n; ++i) {\n"
   "    int prime = 1;\n"
   "    for (long j = 2; j * j <= i; ++j)\n"
   "      if (i % j == 0) { prime = 0; break; }\n"
   "    c += prime;\n"
   "  }\n"
   "  return c;\n"
   "}\n",
   {30}, "AlgoRT"},

  {"algo_lcm",
   "long algo_lcm(long a, long b) {\n"
   "  long g = a, h = b;\n"
   "  while (h) { long t = h; h = g % h; g = t; }\n"
   "  return a / g * b;\n"
   "}\n",
   {12, 18}, "AlgoRT"},

  {"algo_modpow",
   "long algo_modpow(long base, long exp, long mod) {\n"
   "  unsigned long r = 1, b = (unsigned long)base % (unsigned long)mod;\n"
   "  unsigned long e = (unsigned long)exp;\n"
   "  while (e > 0) {\n"
   "    if (e & 1) r = r * b % (unsigned long)mod;\n"
   "    b = b * b % (unsigned long)mod;\n"
   "    e >>= 1;\n"
   "  }\n"
   "  return (long)r;\n"
   "}\n",
   {3, 13, 1000000007}, "AlgoRT"},

  {"algo_dec_to_bin_count",
   "long algo_dec_to_bin_count(long n) {\n"
   "  unsigned long u = (unsigned long)n;\n"
   "  long bits = 0;\n"
   "  while (u > 0) { ++bits; u >>= 1; }\n"
   "  return bits;\n"
   "}\n",
   {255}, "AlgoRT"},

  {"algo_gray_code",
   "long algo_gray_code(long n) {\n"
   "  return n ^ ((unsigned long)n >> 1);\n"
   "}\n",
   {42}, "AlgoRT"},

  {"algo_from_gray",
   "long algo_from_gray(long g) {\n"
   "  unsigned long n = (unsigned long)g;\n"
   "  unsigned long mask = n >> 1;\n"
   "  while (mask) { n ^= mask; mask >>= 1; }\n"
   "  return (long)n;\n"
   "}\n",
   {63}, "AlgoRT"},

  {"algo_abs_diff_sum",
   "long algo_abs_diff_sum(long a, long b, long c) {\n"
   "  long d1 = a-b; d1 = d1 < 0 ? -d1 : d1;\n"
   "  long d2 = b-c; d2 = d2 < 0 ? -d2 : d2;\n"
   "  long d3 = a-c; d3 = d3 < 0 ? -d3 : d3;\n"
   "  return d1 + d2 + d3;\n"
   "}\n",
   {10, 30, 50}, "AlgoRT"},
};

static const std::vector<RoundTripTC> kA64Algo = {
  {"a64_prime", "long a64_prime(long n) { if(n<2)return 0; for(long i=2;i*i<=n;++i) if(n%i==0)return 0; return 1; }\n", {97}, "AlgoRT"},
  {"a64_lcm", "long a64_lcm(long a,long b) { long g=a,h=b; while(h){long t=h;h=g%h;g=t;} return a/g*b; }\n", {12, 18}, "AlgoRT"},
  {"a64_gray", "long a64_gray(long n) { return n ^ ((unsigned long)n >> 1); }\n", {42}, "AlgoRT"},
  {"a64_modpow",
   "long a64_modpow(long base, long exp, long mod) {\n"
   "  unsigned long r=1, b=(unsigned long)base%(unsigned long)mod;\n"
   "  unsigned long e=(unsigned long)exp;\n"
   "  while(e>0){if(e&1)r=r*b%(unsigned long)mod;b=b*b%(unsigned long)mod;e>>=1;}\n"
   "  return (long)r;\n"
   "}\n",
   {3, 13, 1000000007}, "AlgoRT"},
  {"a64_bit_count", "long a64_bit_count(long n) { unsigned long u=(unsigned long)n; long b=0; while(u>0){++b;u>>=1;} return b; }\n", {255}, "AlgoRT"},
  {"a64_abs_diff_sum",
   "long a64_abs_diff_sum(long a, long b, long c) {\n"
   "  long d1=a-b; d1=d1<0?-d1:d1;\n"
   "  long d2=b-c; d2=d2<0?-d2:d2;\n"
   "  return d1+d2;\n"
   "}\n",
   {10, 30, 50}, "AlgoRT"},
};

static const std::vector<RoundTripTC> kARM32Algo = {
  {"arm_prime", "int arm_prime(int n) { if(n<2)return 0; for(int i=2;i*i<=n;++i) if(n%i==0)return 0; return 1; }\n", {97}, "AlgoRT"},
  {"arm_lcm", "int arm_lcm(int a,int b) { int g=a,h=b; while(h){int t=h;h=g%h;g=t;} return a/g*b; }\n", {12, 18}, "AlgoRT"},
  {"arm_gray", "int arm_gray(int n) { return n ^ ((unsigned int)n >> 1); }\n", {42}, "AlgoRT"},
  {"arm_modpow",
   "int arm_modpow(int base, int exp, int mod) {\n"
   "  unsigned int r=1, b=(unsigned int)base%(unsigned int)mod;\n"
   "  unsigned int e=(unsigned int)exp;\n"
   "  while(e>0){if(e&1)r=r*b%(unsigned int)mod;b=b*b%(unsigned int)mod;e>>=1;}\n"
   "  return (int)r;\n"
   "}\n",
   {3, 13, 100000007}, "AlgoRT"},
  {"arm_bit_count", "int arm_bit_count(int n) { unsigned int u=(unsigned int)n; int b=0; while(u>0){++b;u>>=1;} return b; }\n", {255}, "AlgoRT"},
  {"arm_abs_diff_sum",
   "int arm_abs_diff_sum(int a, int b, int c) {\n"
   "  int d1=a-b; d1=d1<0?-d1:d1;\n"
   "  int d2=b-c; d2=d2<0?-d2:d2;\n"
   "  return d1+d2;\n"
   "}\n",
   {10, 30, 50}, "AlgoRT"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(AlgoRT, X64AlgoRT, ::testing::ValuesIn(kX64Algo), rtTCName);
INSTANTIATE_TEST_SUITE_P(AlgoRT, A64AlgoRT, ::testing::ValuesIn(kA64Algo), rtTCName);
INSTANTIATE_TEST_SUITE_P(AlgoRT, ARM32AlgoRT, ::testing::ValuesIn(kARM32Algo), rtTCName);
