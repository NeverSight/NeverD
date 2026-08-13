//===- ARM32_LogicShiftCarryRTTests.cpp - logical-op shifter carry -*-C++-*-==//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// ARM ANDS/ORRS/EORS/BICS/TST/TEQ set C to the barrel-shifter carry-out when
// the second operand is shifted (and to bit 31 of a rotated modified immediate).
// The roundtrip harness only compares return values, so these probes fold C in
// with a following `adc Rd,Rd,#0` (Rd preset to 0) to expose any missing carry.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32LogicShiftCarryRT : public SemanticRoundTripFixture,
                               public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32LogicShiftCarryRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kARM = {
  // ANDS with LSL #imm operand: C = Rm[32-n].
  {"ands_lsl_c",
   "long f(long a,long b){unsigned long c=0;"
   "__asm__ volatile(\"ands r3,%1,%2,lsl #4\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a),\"r\"(b):\"r3\",\"cc\");return c;}\n",
   {0xFFFFFFFFULL, 0x10000000ULL}, "LogicShiftCarry"},
  // ORRS with LSR #imm: C = Rm[n-1].
  {"orrs_lsr_c",
   "long f(long a,long b){unsigned long c=0;"
   "__asm__ volatile(\"orrs r3,%1,%2,lsr #4\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a),\"r\"(b):\"r3\",\"cc\");return c;}\n",
   {0x00000000ULL, 0x00000008ULL}, "LogicShiftCarry"},
  // EORS with ASR #imm: C = Rm[n-1].
  {"eors_asr_c",
   "long f(long a,long b){unsigned long c=0;"
   "__asm__ volatile(\"eors r3,%1,%2,asr #4\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a),\"r\"(b):\"r3\",\"cc\");return c;}\n",
   {0x00000000ULL, 0x00000008ULL}, "LogicShiftCarry"},
  // BICS with ROR #imm: C = Rm[(n-1) mod 32].
  {"bics_ror_c",
   "long f(long a,long b){unsigned long c=0;"
   "__asm__ volatile(\"bics r3,%1,%2,ror #4\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a),\"r\"(b):\"r3\",\"cc\");return c;}\n",
   {0x00000000ULL, 0x00000008ULL}, "LogicShiftCarry"},
  // TST with LSL #imm: C = Rm[32-n].
  {"tst_lsl_c",
   "long f(long a,long b){unsigned long c=0;"
   "__asm__ volatile(\"tst %1,%2,lsl #4\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a),\"r\"(b):\"cc\");return c;}\n",
   {0xFFFFFFFFULL, 0x10000000ULL}, "LogicShiftCarry"},
  // TEQ with LSL #imm.
  {"teq_lsl_c",
   "long f(long a,long b){unsigned long c=0;"
   "__asm__ volatile(\"teq %1,%2,lsl #4\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a),\"r\"(b):\"cc\");return c;}\n",
   {0xFFFFFFFFULL, 0x10000000ULL}, "LogicShiftCarry"},
  // ANDS with a register shift amount.
  {"ands_lslreg_c",
   "long f(long a,long b){unsigned long c=0;"
   "__asm__ volatile(\"mov r4,#16\\n\\tands r3,%1,%2,lsl r4\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a),\"r\"(b):\"r3\",\"r4\",\"cc\");return c;}\n",
   {0xFFFFFFFFULL, 0x00010000ULL}, "LogicShiftCarry"},
  // MOVS with a rotated modified immediate: C = bit31 of the constant.
  {"movs_rotimm_c",
   "long f(){unsigned long c=0;"
   "__asm__ volatile(\"movs r3,#0xFF000000\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c)::\"r3\",\"cc\");return c;}\n",
   {}, "LogicShiftCarry"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(LogicShiftCarry, ARM32LogicShiftCarryRT,
                         ::testing::ValuesIn(kARM), rtTCName);
