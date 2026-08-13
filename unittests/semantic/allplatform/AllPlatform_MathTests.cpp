//===- AllPlatform_MathTests.cpp - Mathematical roundtrip tests -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Mathematical computation patterns: integer math, bit tricks, number theory.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64MathRT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64MathRT, Verify) { roundTripX64(GetParam()); }

class A64MathRT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64MathRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32MathRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32MathRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kX64Math = {
  {"m_avg", "long m_avg(long a, long b) { return (a + b) / 2; }\n", {100, 200}, "MathRT"},
  {"m_avg_no_overflow", "long m_avg_no(long a, long b) { return a/2 + b/2 + (a%2 + b%2)/2; }\n", {100, 201}, "MathRT"},
  {"m_clamp", "long m_clamp(long v, long lo, long hi) { return v<lo?lo:v>hi?hi:v; }\n", {50, 10, 100}, "MathRT"},
  {"m_sign", "long m_sign(long a) { return a > 0 ? 1 : a < 0 ? -1 : 0; }\n", {0xFFFFFFFFFFFFFF00ULL}, "MathRT"},
  {"m_round_up_pow2",
   "long m_round_up(long a, long p) {\n"
   "  unsigned long mask = (unsigned long)p - 1;\n"
   "  return (long)(((unsigned long)a + mask) & ~mask);\n"
   "}\n",
   {100, 16}, "MathRT"},
  {"m_align_down",
   "long m_align_down(long a, long p) {\n"
   "  return a & ~((unsigned long)p - 1);\n"
   "}\n",
   {100, 16}, "MathRT"},
  {"m_is_aligned", "long m_is_aligned(long a, long p) { return ((unsigned long)a & ((unsigned long)p-1)) == 0; }\n", {64, 16}, "MathRT"},
  {"m_div_round_up", "long m_div_up(long a, long b) { return (a + b - 1) / b; }\n", {100, 7}, "MathRT"},
  {"m_cross2d",
   "long m_cross2d(long ax, long ay, long bx, long by) {\n"
   "  return ax*by - ay*bx;\n"
   "}\n",
   {3, 4, 5, 6}, "MathRT"},
  {"m_manhattan",
   "long m_manhattan(long x1, long y1, long x2, long y2) {\n"
   "  long dx=x1-x2; dx=dx<0?-dx:dx;\n"
   "  long dy=y1-y2; dy=dy<0?-dy:dy;\n"
   "  return dx+dy;\n"
   "}\n",
   {3, 7, 10, 2}, "MathRT"},
  {"m_midpoint", "long m_mid(long a, long b) { return a + (b-a)/2; }\n", {10, 100}, "MathRT"},
  {"m_wrap", "long m_wrap(long v, long max) { return ((v % max) + max) % max; }\n", {(uint64_t)(int64_t)-3, 10}, "MathRT"},
  {"m_lerp_int", "long m_lerp(long a, long b, long t) { return a + (b-a)*t/100; }\n", {0, 100, 75}, "MathRT"},
  {"m_step", "long m_step(long edge, long x) { return x >= edge ? 1 : 0; }\n", {50, 42}, "MathRT"},
  {"m_smoothstep_approx",
   "long m_smoothstep(long x) {\n"
   "  if (x <= 0) return 0;\n"
   "  if (x >= 100) return 100;\n"
   "  return x*x*(300-2*x)/10000;\n"
   "}\n",
   {50}, "MathRT"},
};

static const std::vector<RoundTripTC> kA64Math = {
  {"a64m_avg", "long a64m_avg(long a, long b) { return (a+b)/2; }\n", {100, 200}, "MathRT"},
  {"a64m_clamp", "long a64m_clamp(long v,long lo,long hi) { return v<lo?lo:v>hi?hi:v; }\n", {50, 10, 100}, "MathRT"},
  {"a64m_sign", "long a64m_sign(long a) { return a>0?1:a<0?-1:0; }\n", {0xFFFFFFFFFFFFFF00ULL}, "MathRT"},
  {"a64m_align", "long a64m_align(long a, long p) { return a & ~((unsigned long)p-1); }\n", {100, 16}, "MathRT"},
  {"a64m_divup", "long a64m_divup(long a, long b) { return (a+b-1)/b; }\n", {100, 7}, "MathRT"},
  {"a64m_cross", "long a64m_cross(long ax,long ay,long bx,long by) { return ax*by-ay*bx; }\n", {3, 4, 5, 6}, "MathRT"},
  {"a64m_manhattan",
   "long a64m_manhattan(long x1,long y1,long x2,long y2) {\n"
   "  long dx=x1-x2; dx=dx<0?-dx:dx;\n"
   "  long dy=y1-y2; dy=dy<0?-dy:dy;\n"
   "  return dx+dy;\n"
   "}\n",
   {3, 7, 10, 2}, "MathRT"},
  {"a64m_mid", "long a64m_mid(long a, long b) { return a+(b-a)/2; }\n", {10, 100}, "MathRT"},
  {"a64m_wrap", "long a64m_wrap(long v, long max) { return ((v%max)+max)%max; }\n", {0xFFFFFFFFFFFFFFFDULL, 10}, "MathRT"},
  {"a64m_lerp", "long a64m_lerp(long a, long b, long t) { return a+(b-a)*t/100; }\n", {0, 100, 75}, "MathRT"},
};

static const std::vector<RoundTripTC> kARM32Math = {
  {"armm_avg", "int armm_avg(int a, int b) { return (a+b)/2; }\n", {100, 200}, "MathRT"},
  {"armm_clamp", "int armm_clamp(int v,int lo,int hi) { return v<lo?lo:v>hi?hi:v; }\n", {50, 10, 100}, "MathRT"},
  {"armm_sign", "int armm_sign(int a) { return a>0?1:a<0?-1:0; }\n", {0xFFFFFF00}, "MathRT"},
  {"armm_align", "int armm_align(int a, int p) { return a & ~((unsigned int)p-1); }\n", {100, 16}, "MathRT"},
  {"armm_divup", "int armm_divup(int a, int b) { return (a+b-1)/b; }\n", {100, 7}, "MathRT"},
  {"armm_cross", "int armm_cross(int ax,int ay,int bx,int by) { return ax*by-ay*bx; }\n", {3, 4, 5, 6}, "MathRT"},
  {"armm_manhattan",
   "int armm_manhattan(int x1,int y1,int x2) {\n"
   "  int dx=x1-x2; dx=dx<0?-dx:dx;\n"
   "  return dx+y1;\n"
   "}\n",
   {3, 7, 10}, "MathRT"},
  {"armm_mid", "int armm_mid(int a, int b) { return a+(b-a)/2; }\n", {10, 100}, "MathRT"},
  {"armm_lerp", "int armm_lerp(int a, int b, int t) { return a+(b-a)*t/100; }\n", {0, 100, 75}, "MathRT"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(MathRT, X64MathRT, ::testing::ValuesIn(kX64Math), rtTCName);
INSTANTIATE_TEST_SUITE_P(MathRT, A64MathRT, ::testing::ValuesIn(kA64Math), rtTCName);
INSTANTIATE_TEST_SUITE_P(MathRT, ARM32MathRT, ::testing::ValuesIn(kARM32Math), rtTCName);
