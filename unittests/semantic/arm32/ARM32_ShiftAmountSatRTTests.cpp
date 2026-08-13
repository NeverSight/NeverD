//===- ARM32_ShiftAmountSatRTTests.cpp - register shift >= 32 edge -*-C++-*-==//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// ARM register-controlled shifts (`lsl/lsr/asr/ror Rd,Rm,Rs`) use only Rs[7:0]
// as the amount, and the architecture defines saturating behaviour once that
// amount reaches the operand width — a corner that LLVM's raw shl/lshr/ashr do
// NOT provide (>= bitwidth is poison):
//
//   LSL Rm, n>=32  -> 0            (C = Rm[0] at n==32, else 0)
//   LSR Rm, n>=32  -> 0            (C = Rm[31] at n==32, else 0)
//   ASR Rm, n>=32  -> Rm[31]?-1:0  (C = Rm[31])
//   ROR Rm, n      -> ROR by (n mod 32); n mod 32 == 0 (n!=0) leaves the value
//                     unchanged    (C = Rm[31])
//
// The lifter masks LSL/LSR/ASR amounts to Rs[7:0] (only ROR to mod 32) and
// relies on the IR shift ops saturating; the backend's INT_LEFT/INT_RIGHT
// `select(amt<bits, shifted, 0)` and INT_ASHR `clamp to bits-1` provide
// exactly that, and emitRegShifterCarry derives C through the same saturating
// INT_RIGHT.  Every existing register-amount probe (ARM32_ShiftCarryRTTests)
// uses amounts < 32 (0 and 16), so the >= 32 saturation arm — value AND shifter
// carry-out — was never exercised.  These probes drive 32/33/40/64/255 with the
// amount supplied in a register so the shift cannot fold to an immediate form.
//
// Oracle: original-Unicorn vs lifted-Unicorn.  A raw (non-saturating) shift
// would diverge (poison / host-modulo) from ARM's defined 0 / sign-fill.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32ShiftAmountSatRT : public SemanticRoundTripFixture,
                              public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32ShiftAmountSatRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kARM = {

  // ===== Value: LSL by a register amount >= 32 -> 0. =====
  {"lsl_reg_32",
   "long f(long a,long b){unsigned r;"
   "__asm__ volatile(\"lsl %0,%1,%2\":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");return r;}\n",
   {0x12345678ULL, 32}, "ShiftAmtSat"},
  {"lsl_reg_33",
   "long f(long a,long b){unsigned r;"
   "__asm__ volatile(\"lsl %0,%1,%2\":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");return r;}\n",
   {0xFFFFFFFFULL, 33}, "ShiftAmtSat"},
  {"lsl_reg_255",
   "long f(long a,long b){unsigned r;"
   "__asm__ volatile(\"lsl %0,%1,%2\":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");return r;}\n",
   {0xDEADBEEFULL, 255}, "ShiftAmtSat"},

  // ===== Value: LSR by a register amount >= 32 -> 0. =====
  {"lsr_reg_32",
   "long f(long a,long b){unsigned r;"
   "__asm__ volatile(\"lsr %0,%1,%2\":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");return r;}\n",
   {0x80000000ULL, 32}, "ShiftAmtSat"},
  {"lsr_reg_64",
   "long f(long a,long b){unsigned r;"
   "__asm__ volatile(\"lsr %0,%1,%2\":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");return r;}\n",
   {0xFFFFFFFFULL, 64}, "ShiftAmtSat"},

  // ===== Value: ASR by >= 32 -> sign replicated (negative source -> -1). =====
  {"asr_reg_40_neg",
   "long f(long a,long b){int r;"
   "__asm__ volatile(\"asr %0,%1,%2\":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");return r;}\n",
   {(uint64_t)(int64_t)(int32_t)0x80000000, 40}, "ShiftAmtSat"},
  // ASR by >= 32 with positive source -> 0.
  {"asr_reg_40_pos",
   "long f(long a,long b){int r;"
   "__asm__ volatile(\"asr %0,%1,%2\":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");return r;}\n",
   {0x7FFFFFFFULL, 40}, "ShiftAmtSat"},

  // ===== Value: ROR by a register amount that is a multiple of 32 -> unchanged;
  //       by 33 -> ROR by 1. =====
  {"ror_reg_32",
   "long f(long a,long b){unsigned r;"
   "__asm__ volatile(\"ror %0,%1,%2\":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");return r;}\n",
   {0x12345678ULL, 32}, "ShiftAmtSat"},
  {"ror_reg_33",
   "long f(long a,long b){unsigned r;"
   "__asm__ volatile(\"ror %0,%1,%2\":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");return r;}\n",
   {0x00000003ULL, 33}, "ShiftAmtSat"},
  {"ror_reg_64",
   "long f(long a,long b){unsigned r;"
   "__asm__ volatile(\"ror %0,%1,%2\":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");return r;}\n",
   {0xABCDEF01ULL, 64}, "ShiftAmtSat"},

  // ===== Value used inside a binary data-processing op (operandRead path):
  //       add Rd, Rn, Rm, lsl Rs with Rs >= 32 -> Rm contributes 0. =====
  {"add_lslreg_32",
   "long f(long a,long b,long c){unsigned r;"
   "__asm__ volatile(\"add %0,%1,%2,lsl %3\":\"=r\"(r):\"r\"(a),\"r\"(b),\"r\"(c):\"cc\");"
   "return r;}\n",
   {0x1000ULL, 0xFFFFFFFFULL, 32}, "ShiftAmtSat"},
  {"orr_asrreg_40",
   "long f(long a,long b,long c){unsigned r;"
   "__asm__ volatile(\"orr %0,%1,%2,asr %3\":\"=r\"(r):\"r\"(a),\"r\"(b),\"r\"(c):\"cc\");"
   "return r;}\n",
   {0x0000000FULL, (uint64_t)(int64_t)(int32_t)0x80000000, 40}, "ShiftAmtSat"},

  // ===== Carry-out: LSLS by 32 -> C = Rm[0]; by 33 -> C = 0. =====
  {"lsls_reg_32_cbit0",
   "long f(long a,long b){unsigned long c=0;"
   "__asm__ volatile(\"lsls r3,%1,%2\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a),\"r\"(b):\"r3\",\"cc\");return c;}\n",
   {0x00000001ULL, 32}, "ShiftAmtSat"},
  {"lsls_reg_33_zero",
   "long f(long a,long b){unsigned long c=0;"
   "__asm__ volatile(\"lsls r3,%1,%2\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a),\"r\"(b):\"r3\",\"cc\");return c;}\n",
   {0xFFFFFFFFULL, 33}, "ShiftAmtSat"},

  // ===== Carry-out: LSRS by 32 -> C = Rm[31]; by 64 -> C = 0. =====
  {"lsrs_reg_32_cbit31",
   "long f(long a,long b){unsigned long c=0;"
   "__asm__ volatile(\"lsrs r3,%1,%2\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a),\"r\"(b):\"r3\",\"cc\");return c;}\n",
   {0x80000000ULL, 32}, "ShiftAmtSat"},
  {"lsrs_reg_64_zero",
   "long f(long a,long b){unsigned long c=0;"
   "__asm__ volatile(\"lsrs r3,%1,%2\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a),\"r\"(b):\"r3\",\"cc\");return c;}\n",
   {0xFFFFFFFFULL, 64}, "ShiftAmtSat"},

  // ===== Carry-out: ASRS by >= 32 -> C = Rm[31]. =====
  {"asrs_reg_40_cbit31",
   "long f(long a,long b){unsigned long c=0;"
   "__asm__ volatile(\"asrs r3,%1,%2\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a),\"r\"(b):\"r3\",\"cc\");return c;}\n",
   {0x80000000ULL, 40}, "ShiftAmtSat"},

  // ===== Carry-out: RORS by 32 -> C = Rm[31]; by 33 -> C = Rm[0]. =====
  {"rors_reg_32_cbit31",
   "long f(long a,long b){unsigned long c=0;"
   "__asm__ volatile(\"rors r3,%1,%2\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a),\"r\"(b):\"r3\",\"cc\");return c;}\n",
   {0x80000000ULL, 32}, "ShiftAmtSat"},
  {"rors_reg_33_cbit0",
   "long f(long a,long b){unsigned long c=0;"
   "__asm__ volatile(\"rors r3,%1,%2\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a),\"r\"(b):\"r3\",\"cc\");return c;}\n",
   {0x00000001ULL, 33}, "ShiftAmtSat"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(ShiftAmtSat, ARM32ShiftAmountSatRT,
                         ::testing::ValuesIn(kARM), rtTCName);
