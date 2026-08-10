//===- X64_LoopPartialRegRTTests.cpp - loop-carried partial regs -*- C++ -*-==//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A sub-register write (8/16-bit) inside a loop is a loop-carried PARTIAL value:
// the parent register's upper bits must survive every iteration (the SSA phi
// merges the partial write with the preserved high bits).  These probes keep an
// 8/16-bit accumulator/counter in a loop and read the parent at full width, so
// any mishandling of partial-write merge across a phi corrupts the upper bits
// and shows as a return-value mismatch.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64LoopPartialRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64LoopPartialRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // 16-bit accumulator in a loop, upper 16 bits of the parent preserved.
  {"loop_addw_read32",
   "long f(long n){unsigned acc=0x12340000u;"
   "for(int i=0;i<(int)n;i++)"
   "__asm__ volatile(\"addw $1,%w0\":\"+r\"(acc)::\"cc\");"
   "return acc;}\n",
   {5}, "LoopPartial"},
  // 8-bit accumulator in a loop, upper 24 bits preserved.
  {"loop_addb_read32",
   "long f(long n){unsigned acc=0x12345600u;"
   "for(int i=0;i<(int)n;i++)"
   "__asm__ volatile(\"addb $1,%b0\":\"+r\"(acc)::\"cc\");"
   "return acc;}\n",
   {7}, "LoopPartial"},
  // 16-bit accumulator read at 64-bit width.
  {"loop_addw_read64",
   "long f(long n){unsigned long acc=0xAAAABBBB00000000ULL|0x1234u;"
   "for(long i=0;i<n;i++)"
   "__asm__ volatile(\"addw $2,%w0\":\"+r\"(acc)::\"cc\");"
   "return acc;}\n",
   {3}, "LoopPartial"},
  // 8-bit subtract accumulator (borrow within byte), parent preserved.
  {"loop_subb_read32",
   "long f(long n){unsigned acc=0xDEAD00FFu;"
   "for(int i=0;i<(int)n;i++)"
   "__asm__ volatile(\"subb $1,%b0\":\"+r\"(acc)::\"cc\");"
   "return acc;}\n",
   {4}, "LoopPartial"},
  // 16-bit xor accumulator.
  {"loop_xorw_read32",
   "long f(long n){unsigned acc=0xCAFE0000u;unsigned short k=0x0101;"
   "for(int i=0;i<(int)n;i++)"
   "__asm__ volatile(\"xorw %w1,%w0\":\"+r\"(acc):\"r\"(k):\"cc\");"
   "return acc;}\n",
   {3}, "LoopPartial"},
  // Loop counter kept in 16 bits, wrapping at 0x10000, read as 32-bit.
  {"loop_cnt16_wrap",
   "long f(long n){unsigned c=0xFFFE0000u;"  // high 16 = 0xFFFE marker
   "for(int i=0;i<(int)n;i++)"
   "__asm__ volatile(\"incw %w0\":\"+r\"(c)::\"cc\");"
   "return c;}\n",
   {5}, "LoopPartial"},
  // C-level: short accumulator (compiler-managed partial register).
  {"loop_short_acc",
   "long f(long n){unsigned base=0x77770000u;short acc=0;"
   "for(int i=0;i<(int)n;i++)acc=(short)(acc+3);"
   "return base|(unsigned short)acc;}\n",
   {10}, "LoopPartial"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(LoopPartial, X64LoopPartialRT,
                         ::testing::ValuesIn(kX64), rtTCName);
