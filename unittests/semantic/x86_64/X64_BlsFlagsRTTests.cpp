//===- X64_BlsFlagsRTTests.cpp - BLSI/BLSR/BLSMSK flag semantics -*- C++ -*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// x86 BMI1 `BLSI`/`BLSR`/`BLSMSK` isolate/reset/mask the lowest set bit of the
// source.  Their VALUE is exercised by existing tests, but their EFLAGS output
// — which is subtle and DIFFERS across the three — had no coverage:
//
//   BLSI   dst = src & -src        : CF = (src != 0)   ZF = (dst == 0)  SF=dst<0
//   BLSR   dst = src & (src - 1)   : CF = (src == 0)   ZF = (dst == 0)  SF=dst<0
//   BLSMSK dst = src ^ (src - 1)   : CF = (src == 0)   ZF = 0 (always)  SF=dst<0
//
// (OF is cleared by all three.)  The CF polarity is the trap: BLSI sets CF when
// the source is NON-zero, while BLSR/BLSMSK set CF when the source IS zero — a
// lift that reuses one CF formula for all three is wrong for two of them.  A
// second trap: BLSMSK's ZF is *always* cleared (its result is never zero — for
// src==0 it is all-ones), unlike BLSI/BLSR whose ZF tracks the result.
//
// The lifter computes each result with INT_NEG2/INT_SUB + AND/XOR and emits the
// flags explicitly (CF via INT_NOTEQUAL for BLSI vs INT_EQUAL for BLSR/BLSMSK;
// ZF from the result for BLSI/BLSR but a hard 0 for BLSMSK; SF from the result;
// OF=0).  These probes drive the zero / non-zero source boundary, materialize
// one flag per probe with setc/setz/sets, and fold it together with the result
// so any CF-polarity, always-clear-ZF, or SF error diverges from the native run.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64BlsFlagsRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64BlsFlagsRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {

  // ===== BLSI: CF = (src != 0). =====
  // Non-zero source -> CF=1, dst=lowest set bit (0x10).
  {"blsi_cf_set",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long src=a, dst, cf;\n"
   "  __asm__ volatile(\"blsiq %2,%0\\n\\tsetc %b1\\n\\t\"\n"
   "    :\"=r\"(dst),\"=q\"(cf):\"r\"(src):\"cc\");\n"
   "  return dst*4+(cf&1);}\n",
   {0x30}, "BlsFlags", 1, "-mbmi"},

  // Zero source -> CF=0, dst=0 (BLSI CF is the OPPOSITE polarity of BLSR/BLSMSK).
  {"blsi_cf_clr",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long src=a, dst, cf;\n"
   "  __asm__ volatile(\"blsiq %2,%0\\n\\tsetc %b1\\n\\t\"\n"
   "    :\"=r\"(dst),\"=q\"(cf):\"r\"(src):\"cc\");\n"
   "  return dst*4+(cf&1);}\n",
   {0}, "BlsFlags", 1, "-mbmi"},

  // BLSI ZF = (dst == 0); zero source -> dst=0 -> ZF=1.
  {"blsi_zf_set",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long src=a, dst, zf;\n"
   "  __asm__ volatile(\"blsiq %2,%0\\n\\tsetz %b1\\n\\t\"\n"
   "    :\"=r\"(dst),\"=q\"(zf):\"r\"(src):\"cc\");\n"
   "  return dst*4+(zf&1);}\n",
   {0}, "BlsFlags", 1, "-mbmi"},

  // BLSI SF: isolated bit is bit63 (src = 1<<63) -> dst<0 -> SF=1.
  {"blsi_sf_set",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long src=a, dst, sf;\n"
   "  __asm__ volatile(\"blsiq %2,%0\\n\\tsets %b1\\n\\t\"\n"
   "    :\"=r\"(dst),\"=q\"(sf):\"r\"(src):\"cc\");\n"
   "  return dst*4+(sf&1);}\n",
   {0x8000000000000000ULL}, "BlsFlags", 1, "-mbmi"},

  // ===== BLSR: CF = (src == 0). =====
  // Zero source -> CF=1, dst=0.
  {"blsr_cf_set",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long src=a, dst, cf;\n"
   "  __asm__ volatile(\"blsrq %2,%0\\n\\tsetc %b1\\n\\t\"\n"
   "    :\"=r\"(dst),\"=q\"(cf):\"r\"(src):\"cc\");\n"
   "  return dst*4+(cf&1);}\n",
   {0}, "BlsFlags", 1, "-mbmi"},

  // Non-zero source -> CF=0, dst clears lowest set bit (0x30 -> 0x20).
  {"blsr_cf_clr",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long src=a, dst, cf;\n"
   "  __asm__ volatile(\"blsrq %2,%0\\n\\tsetc %b1\\n\\t\"\n"
   "    :\"=r\"(dst),\"=q\"(cf):\"r\"(src):\"cc\");\n"
   "  return dst*4+(cf&1);}\n",
   {0x30}, "BlsFlags", 1, "-mbmi"},

  // BLSR ZF = (dst == 0); single-bit source -> dst=0 -> ZF=1 (and CF=0 here, so
  // this also pins ZF != CF for BLSR).
  {"blsr_zf_onebit",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long src=a, dst, zf;\n"
   "  __asm__ volatile(\"blsrq %2,%0\\n\\tsetz %b1\\n\\t\"\n"
   "    :\"=r\"(dst),\"=q\"(zf):\"r\"(src):\"cc\");\n"
   "  return dst*4+(zf&1);}\n",
   {1}, "BlsFlags", 1, "-mbmi"},

  // BLSR SF: src=0x8000...0001 -> dst=0x8000...0000 -> SF=1.
  {"blsr_sf_set",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long src=a, dst, sf;\n"
   "  __asm__ volatile(\"blsrq %2,%0\\n\\tsets %b1\\n\\t\"\n"
   "    :\"=r\"(dst),\"=q\"(sf):\"r\"(src):\"cc\");\n"
   "  return dst*4+(sf&1);}\n",
   {0x8000000000000001ULL}, "BlsFlags", 1, "-mbmi"},

  // ===== BLSMSK: CF = (src == 0), ZF always 0. =====
  // Zero source -> CF=1, dst=all-ones.
  {"blsmsk_cf_set",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long src=a, dst, cf;\n"
   "  __asm__ volatile(\"blsmskq %2,%0\\n\\tsetc %b1\\n\\t\"\n"
   "    :\"=r\"(dst),\"=q\"(cf):\"r\"(src):\"cc\");\n"
   "  return dst*4+(cf&1);}\n",
   {0}, "BlsFlags", 1, "-mbmi"},

  // Non-zero source -> CF=0, dst=mask up to lowest set bit (0x30 -> 0x1F).
  {"blsmsk_cf_clr",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long src=a, dst, cf;\n"
   "  __asm__ volatile(\"blsmskq %2,%0\\n\\tsetc %b1\\n\\t\"\n"
   "    :\"=r\"(dst),\"=q\"(cf):\"r\"(src):\"cc\");\n"
   "  return dst*4+(cf&1);}\n",
   {0x30}, "BlsFlags", 1, "-mbmi"},

  // BLSMSK ZF is ALWAYS 0 — even for src==0 (result is all-ones, not zero).
  // setz must read 0.  A "ZF from result" mis-lift would also give 0 here, so
  // pair it with blsmsk_cf_set (same src) which pins CF; together they confirm
  // the result/flags for the zero source.
  {"blsmsk_zf_zerosrc",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long src=a, dst, zf;\n"
   "  __asm__ volatile(\"blsmskq %2,%0\\n\\tsetz %b1\\n\\t\"\n"
   "    :\"=r\"(dst),\"=q\"(zf):\"r\"(src):\"cc\");\n"
   "  return dst*4+(zf&1);}\n",
   {0}, "BlsFlags", 1, "-mbmi"},

  // BLSMSK SF: src=1<<63 -> dst = (1<<63) ^ (0x7FFF...F) = all-ones -> SF=1.
  {"blsmsk_sf_set",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long src=a, dst, sf;\n"
   "  __asm__ volatile(\"blsmskq %2,%0\\n\\tsets %b1\\n\\t\"\n"
   "    :\"=r\"(dst),\"=q\"(sf):\"r\"(src):\"cc\");\n"
   "  return dst*4+(sf&1);}\n",
   {0x8000000000000000ULL}, "BlsFlags", 1, "-mbmi"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(BlsFlags, X64BlsFlagsRT,
                         ::testing::ValuesIn(kX64), rtTCName);
