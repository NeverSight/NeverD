//===- X64_LoopJcxzRTTests.cpp - LOOP/LOOPcc + JRCXZ/JECXZ -------*- C++ -*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// x86 has a small family of RCX-driven control-flow ops that no other handler
// covers and that had ZERO prior roundtrip coverage:
//
//   LOOP   rel8 : RCX -= 1; branch if RCX != 0           (flags UNTOUCHED)
//   LOOPE  rel8 : RCX -= 1; branch if RCX != 0 AND ZF==1
//   LOOPNE rel8 : RCX -= 1; branch if RCX != 0 AND ZF==0
//   JRCXZ  rel8 : branch if RCX == 0   (64-bit count)    (no decrement, no flags)
//   JECXZ  rel8 : branch if ECX == 0   (32-bit count)
//
// Subtleties the lifter must get right: the RCX decrement is a plain value op
// that must NOT emit flag side effects (LOOP* preserve EFLAGS); LOOPE/LOOPNE AND
// the RCX!=0 test with the *incoming* ZF; and JECXZ tests the 32-bit ECX while
// JRCXZ tests the full 64-bit RCX (so a value with a nonzero high half but zero
// low half branches under JECXZ but not JRCXZ).  The handlers form
// `RCX = RCX-1` (no flags), `cond = (RCX!=0) [&& (~)ZF]`, and size the JxCXZ
// compare by address size — all correct, so these probes are guardrails over a
// real coverage hole.  Each builds a genuine loop / branch and folds the trip
// count or taken-arm into the return value.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64LoopJcxzRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64LoopJcxzRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {

  // ===== LOOP: body runs exactly RCX times (RCX>=1; RCX==0 would wrap to 2^64). =====
  {"loop_count5",
   "unsigned long f(unsigned long n){\n"
   "  unsigned long acc=0, cnt=n;\n"
   "  __asm__ volatile(\"1:\\n\\tincq %0\\n\\tloop 1b\\n\\t\"\n"
   "    :\"+r\"(acc),\"+c\"(cnt)::\"cc\");\n"
   "  return acc;}\n",
   {5}, "LoopJcxz"},

  {"loop_count1",
   "unsigned long f(unsigned long n){\n"
   "  unsigned long acc=0, cnt=n;\n"
   "  __asm__ volatile(\"1:\\n\\tincq %0\\n\\tloop 1b\\n\\t\"\n"
   "    :\"+r\"(acc),\"+c\"(cnt)::\"cc\");\n"
   "  return acc;}\n",
   {1}, "LoopJcxz"},

  {"loop_count100",
   "unsigned long f(unsigned long n){\n"
   "  unsigned long acc=0, cnt=n;\n"
   "  __asm__ volatile(\"1:\\n\\tincq %0\\n\\tloop 1b\\n\\t\"\n"
   "    :\"+r\"(acc),\"+c\"(cnt)::\"cc\");\n"
   "  return acc;}\n",
   {100}, "LoopJcxz"},

  // LOOP must NOT clobber flags: set CF with stc, run the loop, read CF back.
  {"loop_keeps_cf",
   "unsigned long f(unsigned long n){\n"
   "  unsigned long acc=0, cnt=n, cf;\n"
   "  __asm__ volatile(\"stc\\n\\t1:\\n\\taddq $1,%0\\n\\tloop 1b\\n\\tsetc %b2\\n\\t\"\n"
   "    :\"+r\"(acc),\"+c\"(cnt),\"=q\"(cf)::\"cc\");\n"
   "  return acc*2+(cf&1);}\n",
   {4}, "LoopJcxz"},

  // ===== LOOPNE: continue while (i != target) AND rcx!=0; stop on match. =====
  // target reachable within the count -> stops at i==target.
  {"loopne_found",
   "unsigned long f(unsigned long target){\n"
   "  unsigned long i=0, cnt=8;\n"
   "  __asm__ volatile(\"1:\\n\\tincq %0\\n\\tcmpq %2,%0\\n\\tloopne 1b\\n\\t\"\n"
   "    :\"+r\"(i),\"+c\"(cnt):\"r\"(target):\"cc\");\n"
   "  return i;}\n",
   {3}, "LoopJcxz"},

  // target out of reach -> rcx exhausts first, i == initial count (8).
  {"loopne_exhaust",
   "unsigned long f(unsigned long target){\n"
   "  unsigned long i=0, cnt=8;\n"
   "  __asm__ volatile(\"1:\\n\\tincq %0\\n\\tcmpq %2,%0\\n\\tloopne 1b\\n\\t\"\n"
   "    :\"+r\"(i),\"+c\"(cnt):\"r\"(target):\"cc\");\n"
   "  return i;}\n",
   {20}, "LoopJcxz"},

  // ===== LOOPE: continue while ZF==1 AND rcx!=0. =====
  // ZF forced 1 every iteration (cmp x,x) -> rcx is the limiter -> runs n times.
  {"loope_full",
   "unsigned long f(unsigned long n){\n"
   "  unsigned long i=0, cnt=n;\n"
   "  __asm__ volatile(\"1:\\n\\tincq %0\\n\\tcmpq %0,%0\\n\\tloope 1b\\n\\t\"\n"
   "    :\"+r\"(i),\"+c\"(cnt)::\"cc\");\n"
   "  return i;}\n",
   {6}, "LoopJcxz"},

  // ZF==0 on the first check (i=1 != 99) -> LOOPE stops despite rcx!=0 -> i==1.
  {"loope_zf0_stop",
   "unsigned long f(unsigned long other){\n"
   "  unsigned long i=0, cnt=9;\n"
   "  __asm__ volatile(\"1:\\n\\tincq %0\\n\\tcmpq %2,%0\\n\\tloope 1b\\n\\t\"\n"
   "    :\"+r\"(i),\"+c\"(cnt):\"r\"(other):\"cc\");\n"
   "  return i;}\n",
   {99}, "LoopJcxz"},

  // ===== JRCXZ: branch on the full 64-bit RCX == 0. =====
  {"jrcxz_zero",
   "unsigned long f(unsigned long n){\n"
   "  unsigned long r;\n"
   "  __asm__ volatile(\"jrcxz 2f\\n\\tmovq $111,%0\\n\\tjmp 3f\\n\\t2:\\n\\tmovq $222,%0\\n\\t3:\\n\\t\"\n"
   "    :\"=r\"(r):\"c\"(n):\"cc\");\n"
   "  return r;}\n",
   {0}, "LoopJcxz"},

  {"jrcxz_nonzero",
   "unsigned long f(unsigned long n){\n"
   "  unsigned long r;\n"
   "  __asm__ volatile(\"jrcxz 2f\\n\\tmovq $111,%0\\n\\tjmp 3f\\n\\t2:\\n\\tmovq $222,%0\\n\\t3:\\n\\t\"\n"
   "    :\"=r\"(r):\"c\"(n):\"cc\");\n"
   "  return r;}\n",
   {5}, "LoopJcxz"},

  // JRCXZ with a nonzero HIGH half but zero low 32 bits: RCX != 0 -> NOT taken
  // (distinguishes the 64-bit count from ECX).
  {"jrcxz_highbits",
   "unsigned long f(unsigned long n){\n"
   "  unsigned long r;\n"
   "  __asm__ volatile(\"jrcxz 2f\\n\\tmovq $111,%0\\n\\tjmp 3f\\n\\t2:\\n\\tmovq $222,%0\\n\\t3:\\n\\t\"\n"
   "    :\"=r\"(r):\"c\"(n):\"cc\");\n"
   "  return r;}\n",
   {0x100000000ULL}, "LoopJcxz"},

  // ===== JECXZ: branch on the 32-bit ECX == 0 (ignores RCX high half). =====
  // Same high-half value: ECX==0 -> taken (222), unlike JRCXZ above.
  {"jecxz_highbits",
   "unsigned long f(unsigned long n){\n"
   "  unsigned long r;\n"
   "  __asm__ volatile(\"jecxz 2f\\n\\tmovq $111,%0\\n\\tjmp 3f\\n\\t2:\\n\\tmovq $222,%0\\n\\t3:\\n\\t\"\n"
   "    :\"=r\"(r):\"c\"(n):\"cc\");\n"
   "  return r;}\n",
   {0x100000000ULL}, "LoopJcxz"},

  {"jecxz_nonzero",
   "unsigned long f(unsigned long n){\n"
   "  unsigned long r;\n"
   "  __asm__ volatile(\"jecxz 2f\\n\\tmovq $111,%0\\n\\tjmp 3f\\n\\t2:\\n\\tmovq $222,%0\\n\\t3:\\n\\t\"\n"
   "    :\"=r\"(r):\"c\"(n):\"cc\");\n"
   "  return r;}\n",
   {5}, "LoopJcxz"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(LoopJcxz, X64LoopJcxzRT,
                         ::testing::ValuesIn(kX64), rtTCName);
