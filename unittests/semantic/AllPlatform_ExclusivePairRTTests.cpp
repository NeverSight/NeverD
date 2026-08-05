//===- AllPlatform_ExclusivePairRTTests.cpp - exclusive pair atomics -C++-===//
//
// Exclusive load/store PAIR sequences that the C path never emits: AArch64
// LDXP/STXP and acquire/release LDAXP/STLXP (128-bit exclusive pair), plus
// ARM32 LDREXD/STREXD (64-bit exclusive double) and word LDREX/STREX.  Each
// loads a pair, mutates it, stores it back exclusively, then reads memory --
// a wrong pair address, half-store, or dropped status corrupts the result.
// Single-threaded, so the monitor always succeeds (status 0).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64ExclPairRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64ExclPairRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32ExclPairRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32ExclPairRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kA64 = {
  // LDXP/STXP: load pair, add, store pair back, read memory.  The pair buffer
  // MUST be 16-byte aligned: LDXP/STXP architecturally require quadword
  // alignment and fault otherwise (QEMU enforces MO_ALIGN_16).
  {"ldxp_stxp",
   "long f(long a){ unsigned long b[2] __attribute__((aligned(16)));\n"
   "  b[0]=(unsigned long)a; b[1]=(unsigned long)(a*3+7);\n"
   "  unsigned long* p=b; unsigned long x0,x1; unsigned w;\n"
   "  __asm__ volatile(\"ldxp %1,%2,[%3]\\n\\tadd %1,%1,#5\\n\\tadd %2,%2,#9\\n\\tstxp %w0,%1,%2,[%3]\"\n"
   "    :\"=&r\"(w),\"=&r\"(x0),\"=&r\"(x1):\"r\"(p):\"memory\");\n"
   "  return (long)(b[0]*1000+b[1]+w*1000000); }\n",
   {0x123ULL}, "ExclPair"},
  // LDAXP/STLXP acquire/release variant (also quadword-aligned).
  {"ldaxp_stlxp",
   "long f(long a){ unsigned long b[2] __attribute__((aligned(16)));\n"
   "  b[0]=(unsigned long)(a^0x55); b[1]=(unsigned long)(a+0x100);\n"
   "  unsigned long* p=b; unsigned long x0,x1; unsigned w;\n"
   "  __asm__ volatile(\"ldaxp %1,%2,[%3]\\n\\teor %1,%1,#0xf\\n\\teor %2,%2,#0xf0\\n\\tstlxp %w0,%1,%2,[%3]\"\n"
   "    :\"=&r\"(w),\"=&r\"(x0),\"=&r\"(x1):\"r\"(p):\"memory\");\n"
   "  return (long)(b[0]+b[1]*10+w*1000000); }\n",
   {0x456ULL}, "ExclPair"},
  // 32-bit exclusive word LDXR/STXR.
  {"ldxr_stxr_w",
   "long f(long a){ unsigned b=(unsigned)a; unsigned* p=&b; unsigned x; unsigned w;\n"
   "  __asm__ volatile(\"ldxr %w1,[%2]\\n\\tadd %w1,%w1,#0x11\\n\\tstxr %w0,%w1,[%2]\"\n"
   "    :\"=&r\"(w),\"=&r\"(x):\"r\"(p):\"memory\");\n"
   "  return (long)(b + w*1000000u); }\n",
   {0x789ULL}, "ExclPair"},
};

static const std::vector<RoundTripTC> kARM = {
  // LDREXD/STREXD: 64-bit exclusive double (even/odd register pair).
  {"ldrexd_strexd",
   "int f(int a){ unsigned long long v=((unsigned long long)a<<32)|(unsigned)(a*3+1);\n"
   "  unsigned long long* p=&v; unsigned w;\n"
   "  register unsigned lo asm(\"r4\"); register unsigned hi asm(\"r5\");\n"
   "  __asm__ volatile(\"ldrexd r4,r5,[%1]\\n\\tadds r4,r4,#7\\n\\tadc r5,r5,#0\\n\\tstrexd %0,r4,r5,[%1]\"\n"
   "    :\"=&r\"(w):\"r\"(p):\"r4\",\"r5\",\"memory\",\"cc\");\n"
   "  (void)lo;(void)hi;\n"
   "  return (int)((unsigned)v + (unsigned)(v>>32) + w*100000u); }\n",
   {0x33ULL}, "ExclPair"},
  // LDREX/STREX: 32-bit exclusive word.
  {"ldrex_strex",
   "int f(int a){ unsigned b=(unsigned)a; unsigned* p=&b; unsigned x,w;\n"
   "  __asm__ volatile(\"ldrex %1,[%2]\\n\\tadd %1,%1,#0x21\\n\\tstrex %0,%1,[%2]\"\n"
   "    :\"=&r\"(w),\"=&r\"(x):\"r\"(p):\"memory\");\n"
   "  return (int)(b + w*100000u); }\n",
   {0x44ULL}, "ExclPair"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(ExclPair, A64ExclPairRT,
                         ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(ExclPair, ARM32ExclPairRT,
                         ::testing::ValuesIn(kARM), rtTCName);
