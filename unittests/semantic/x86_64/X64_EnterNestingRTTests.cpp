//===- X64_EnterNestingRTTests.cpp - ENTER nesting-level frame ---*- C++ -*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// x86 `ENTER imm16, imm8` builds a stack frame; the second operand is the
// lexical NESTING LEVEL.  Intel SDM:
//
//   Push(RBP); FrameTemp = RSP
//   IF NestingLevel > 0
//     FOR i = 1 TO NestingLevel-1:  RBP -= 8; Push([RBP])   // copy display
//     Push(FrameTemp)
//   RBP = FrameTemp;  RSP -= imm16
//
// At nesting level 0 this is just `push rbp; mov rbp,rsp; sub rsp,imm16`, which
// the lifter handled.  But the handler IGNORED the nesting level entirely, so
// level >= 1 dropped the extra `Push(FrameTemp)` (and, for level >= 2, the
// display copies) — leaving RSP one+ slots too high and RBP/the frame contents
// wrong.  Compilers never emit nested ENTER, so it had no coverage and the gap
// went unnoticed.  These probes capture RSP-RBP (and the freshly pushed
// FrameTemp slot) right after ENTER and fold the relative delta into the return
// value, so the missing pushes show up as a mismatch vs the native run.
//
// Before the fix the nesting>=1 probes are RED (delta 0 instead of the true
// negative offset); the nesting==0 probes are guardrails that must stay GREEN.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64EnterNestingRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64EnterNestingRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {

  // nesting 1, size 0: push rbp; frameTemp=rsp; push frameTemp; rbp=frameTemp.
  // -> rsp = rbp - 8, so (rsp-rbp) = -8.  Buggy (no nesting) gives 0.
  {"enter_nest1_spbp",
   "unsigned long f(void){\n"
   "  unsigned long d;\n"
   "  __asm__ volatile(\"enter $0,$1\\n\\tmovq %%rsp,%0\\n\\tsubq %%rbp,%0\\n\\tleave\\n\\t\"\n"
   "    :\"=r\"(d)::\"cc\",\"memory\");\n"
   "  return d;}\n",
   {}, "EnterNesting"},

  // nesting 1, size 16: extra frameTemp push (-8) THEN sub rsp,16 -> (rsp-rbp) = -24.
  {"enter_nest1_sz16",
   "unsigned long f(void){\n"
   "  unsigned long d;\n"
   "  __asm__ volatile(\"enter $16,$1\\n\\tmovq %%rsp,%0\\n\\tsubq %%rbp,%0\\n\\tleave\\n\\t\"\n"
   "    :\"=r\"(d)::\"cc\",\"memory\");\n"
   "  return d;}\n",
   {}, "EnterNesting"},

  // nesting 1: the top of stack right after ENTER holds FrameTemp == new RBP, so
  // ([rsp] - rbp) == 0.  Buggy handler never pushes FrameTemp -> nonzero.
  {"enter_nest1_topval",
   "unsigned long f(void){\n"
   "  unsigned long d;\n"
   "  __asm__ volatile(\"enter $0,$1\\n\\tmovq (%%rsp),%0\\n\\tsubq %%rbp,%0\\n\\tleave\\n\\t\"\n"
   "    :\"=r\"(d)::\"cc\",\"memory\");\n"
   "  return d;}\n",
   {}, "EnterNesting"},

  // nesting 2, size 0: one display copy (push [rbp-8]) + push frameTemp.
  // -> rsp = rbp - 16, so (rsp-rbp) = -16.  Buggy gives 0.
  {"enter_nest2_spbp",
   "unsigned long f(void){\n"
   "  unsigned long d;\n"
   "  __asm__ volatile(\"enter $0,$2\\n\\tmovq %%rsp,%0\\n\\tsubq %%rbp,%0\\n\\tleave\\n\\t\"\n"
   "    :\"=r\"(d)::\"cc\",\"memory\");\n"
   "  return d;}\n",
   {}, "EnterNesting"},

  // ===== nesting 0 controls (unchanged behavior; must stay GREEN). =====
  // push rbp; rbp=rsp; size 0 -> rsp==rbp -> (rsp-rbp) == 0.
  {"enter_nest0_spbp",
   "unsigned long f(void){\n"
   "  unsigned long d;\n"
   "  __asm__ volatile(\"enter $0,$0\\n\\tmovq %%rsp,%0\\n\\tsubq %%rbp,%0\\n\\tleave\\n\\t\"\n"
   "    :\"=r\"(d)::\"cc\",\"memory\");\n"
   "  return d;}\n",
   {}, "EnterNesting"},

  // nesting 0, size 16: rsp = rbp - 16 -> (rsp-rbp) = -16.
  {"enter_nest0_sz16",
   "unsigned long f(void){\n"
   "  unsigned long d;\n"
   "  __asm__ volatile(\"enter $16,$0\\n\\tmovq %%rsp,%0\\n\\tsubq %%rbp,%0\\n\\tleave\\n\\t\"\n"
   "    :\"=r\"(d)::\"cc\",\"memory\");\n"
   "  return d;}\n",
   {}, "EnterNesting"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(EnterNesting, X64EnterNestingRT,
                         ::testing::ValuesIn(kX64), rtTCName);
