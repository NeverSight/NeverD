//===- X64_Div128RTTests.cpp - genuine 128/64 div (no lib call) -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// `mul; div` / `imul; idiv` form a genuine 128-bit-dividend / 64-bit-divisor
// division (RDX:RAX is a real 128-bit value from the multiply, not a sign- or
// zero-extension of RAX).  The lifter must keep this as the hardware division
// (the original is a real `div`/`idiv`); lowering it through an i128 udiv/sdiv
// emits a __udivti3/__divti3 library call that Unicorn cannot resolve.  These
// probes drive the idiom so any library-call lowering shows as a recompiled
// emulation failure / mismatch.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64Div128RT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64Div128RT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // Unsigned 128/64: q = (a*b) / c.  a=b=2^32 → a*b=2^64 (RDX:RAX=1:0), /4=2^62.
  {"mul_div_quot",
   "unsigned long f(unsigned long a,unsigned long b,unsigned long c){"
   "unsigned long q,r;"
   "__asm__ volatile(\"mulq %[b]\\n\\tdivq %[c]\""
   ":\"=&a\"(q),\"=&d\"(r):\"a\"(a),[b]\"r\"(b),[c]\"r\"(c):\"cc\");"
   "return q;}\n",
   {0x100000000ULL, 0x100000000ULL, 4}, "Div128"},
  // Unsigned 128/64 remainder.
  {"mul_div_rem",
   "unsigned long f(unsigned long a,unsigned long b,unsigned long c){"
   "unsigned long q,r;"
   "__asm__ volatile(\"mulq %[b]\\n\\tdivq %[c]\""
   ":\"=&a\"(q),\"=&d\"(r):\"a\"(a),[b]\"r\"(b),[c]\"r\"(c):\"cc\");"
   "return r;}\n",
   {0xDEADBEEFCAFEULL, 0x1000003ULL, 0x123457ULL}, "Div128"},
  // Unsigned 128/64 with non-trivial high part and remainder.
  {"mul_div_general",
   "unsigned long f(unsigned long a,unsigned long b,unsigned long c){"
   "unsigned long q,r;"
   "__asm__ volatile(\"mulq %[b]\\n\\tdivq %[c]\""
   ":\"=&a\"(q),\"=&d\"(r):\"a\"(a),[b]\"r\"(b),[c]\"r\"(c):\"cc\");"
   "return q*7+r;}\n",
   {0xFEDCBA9876543ULL, 0x123456789ULL, 0x9E3779B97F4A7ULL}, "Div128"},
  // Signed 128/64: q = (a*b)/c via imul/idiv.
  {"imul_idiv_quot",
   "long f(long a,long b,long c){long q,r;"
   "__asm__ volatile(\"imulq %[b]\\n\\tidivq %[c]\""
   ":\"=&a\"(q),\"=&d\"(r):\"a\"(a),[b]\"r\"(b),[c]\"r\"(c):\"cc\");"
   "return q;}\n",
   {0x100000000LL, 0x100000000LL, 4}, "Div128"},
  // Signed 128/64 with negative operand.
  {"imul_idiv_neg",
   "long f(long a,long b,long c){long q,r;"
   "__asm__ volatile(\"imulq %[b]\\n\\tidivq %[c]\""
   ":\"=&a\"(q),\"=&d\"(r):\"a\"(a),[b]\"r\"(b),[c]\"r\"(c):\"cc\");"
   "return q*100+r;}\n",
   {(uint64_t)(int64_t)-0x100000000LL, 0x100000000LL, 7}, "Div128"},
  // 32-bit genuine 64/32 division (mul/div with EDX:EAX).
  {"mul_div32",
   "unsigned f(unsigned a,unsigned b,unsigned c){unsigned q,r;"
   "__asm__ volatile(\"mull %[b]\\n\\tdivl %[c]\""
   ":\"=&a\"(q),\"=&d\"(r):\"a\"(a),[b]\"r\"(b),[c]\"r\"(c):\"cc\");"
   "return q;}\n",
   {0x10000U, 0x10000U, 5}, "Div128"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(Div128, X64Div128RT, ::testing::ValuesIn(kX64),
                         rtTCName);
