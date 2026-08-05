//===- X64_AuxCarryRTTests.cpp - x86 AF (auxiliary carry) flag --*- C++ -*-===//
//
// Roundtrip probes for the x86 AF (auxiliary carry) flag — the carry/borrow
// out of bit 3, used by BCD adjust idioms and observable via LAHF / PUSHF.
//
// NeverD never modelled AF: the shared `emitFlagsArith` helper (ADD/SUB/CMP/
// ADC/SBC/CMPXCHG/XADD/SCAS/CMPS) and the INC/DEC/NEG handlers set ZF/SF/PF/
// CF/OF but left AF as a stale 0, and LAHF/SAHF ignored AH bit 4 entirely.  So
// any `<arith>; lahf` or `sahf; lahf` that observes AF read 0 instead of the
// real carry-out of bit 3.  The fix computes AF = bit4(A ^ B ^ Result) in one
// shared `emitAF` helper (reused by emitFlagsArith + INC/DEC/NEG) and routes
// AH bit 4 through SAHF (set) and LAHF (read).
//
// Each probe performs an arithmetic op (or SAHF), runs LAHF, and returns AH;
// AF is bit 4 (0x10).  AF is a Unicorn-native x86 flag, so the roundtrip is
// bit-exact — this is a pure lift modelling gap (weak-test-masked: no prior
// LAHF/AF coverage).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64AuxCarryRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64AuxCarryRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // add 0x08+0x08 = 0x10: carry out of bit 3 -> AF=1.  AH = 0x12 (AF<<4 | the
  // reserved bit1).  (old: AF lost -> AH=0x02)
  {"af_add_set",
   "long f(long a,long b){unsigned long o;"
   "__asm__ volatile(\"movb %b1,%%al\\n\\taddb %b2,%%al\\n\\tlahf\\n\\t\""
   "\"movzbl %%ah,%k0\":\"=&r\"(o):\"r\"(a),\"r\"(b):\"ax\",\"cc\");"
   "return (long)o;}\n",
   {0x08ULL, 0x08ULL}, "AuxCarry", 0},

  // Control: add 0x01+0x01 = 0x02, no carry from bit 3 -> AF=0.  AH=0x02 with
  // and without the fix (AF was already 0), so it passes RED and GREEN.
  {"af_add_clear",
   "long f(long a,long b){unsigned long o;"
   "__asm__ volatile(\"movb %b1,%%al\\n\\taddb %b2,%%al\\n\\tlahf\\n\\t\""
   "\"movzbl %%ah,%k0\":\"=&r\"(o):\"r\"(a),\"r\"(b):\"ax\",\"cc\");"
   "return (long)o;}\n",
   {0x01ULL, 0x01ULL}, "AuxCarry", 0},

  // sub 0x10-0x01 = 0x0F: borrow out of bit 3 -> AF=1.  PF=1 (0x0F even
  // parity) so AH = 0x10|0x04|0x02 = 0x16.  (old: AH=0x06)
  {"af_sub_set",
   "long f(long a,long b){unsigned long o;"
   "__asm__ volatile(\"movb %b1,%%al\\n\\tsubb %b2,%%al\\n\\tlahf\\n\\t\""
   "\"movzbl %%ah,%k0\":\"=&r\"(o):\"r\"(a),\"r\"(b):\"ax\",\"cc\");"
   "return (long)o;}\n",
   {0x10ULL, 0x01ULL}, "AuxCarry", 0},

  // inc 0x0F -> 0x10: AF=1.  `clc` first pins CF=0 (INC preserves CF, which is
  // otherwise the uncontrolled entry value).  AH = 0x12.  (old: AH=0x02)
  {"af_inc_set",
   "long f(long a){unsigned long o;"
   "__asm__ volatile(\"clc\\n\\tmovb %b1,%%al\\n\\tincb %%al\\n\\tlahf\\n\\t\""
   "\"movzbl %%ah,%k0\":\"=&r\"(o):\"r\"(a):\"ax\",\"cc\");"
   "return (long)o;}\n",
   {0x0FULL}, "AuxCarry", 0},

  // sahf <- AH(0x10) sets AF=1; lahf reads it back.  Self-contained AF
  // roundtrip through the flag register (no arithmetic).  AH=0x12.
  // (old: SAHF ignores bit4, LAHF emits no AF -> AH=0x02)
  {"af_sahf_lahf",
   "long f(long a){unsigned long o;"
   "__asm__ volatile(\"movb %b1,%%ah\\n\\tsahf\\n\\tlahf\\n\\t\""
   "\"movzbl %%ah,%k0\":\"=&r\"(o):\"r\"(a):\"ax\",\"cc\");"
   "return (long)o;}\n",
   {0x10ULL}, "AuxCarry", 0},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(AuxCarry, X64AuxCarryRT, ::testing::ValuesIn(kX64),
                         rtTCName);
