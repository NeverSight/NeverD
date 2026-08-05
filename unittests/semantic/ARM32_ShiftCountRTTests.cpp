//===- ARM32_ShiftCountRTTests.cpp - ARM32 register-shift count -*- C++ -*-===//
//
// ARM32 register-specified shifts use the FULL low byte Rs[7:0] (0-255) as the
// amount, NOT a 5-bit-masked value:
//   * LSL/LSR by >= 32  -> 0
//   * ASR  by >= 32     -> sign-replicated (0 or -1)
//   * ROR               -> rotate by amount mod 32 (the only mod-32 case)
// and an amount whose low byte is 0 (e.g. Rs=256) is a no-op, not a saturated 0.
//
// A naive `& 31` mask is wrong for LSL/LSR/ASR (it turns 40 into 8); the correct
// model masks the low byte (& 0xFF) and relies on the saturating shift ops for
// amounts >= 32.  These probes drive the shifts with amounts >= 32 via inline asm
// so the difference is observable.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32ShiftCountRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32ShiftCountRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kARM32 = {
  // LSL by exactly 32 -> 0 (a `& 31` mask would leave the value unshifted).
  {"lsl_by_32",
   "long f(long a){unsigned int x=(unsigned int)a,n=32,r;"
   "__asm__ volatile(\"lsl %0,%1,%2\":\"=r\"(r):\"r\"(x),\"r\"(n):);return r;}\n",
   {0xFF}, "ShiftCount", 0},

  // LSL by 40 -> 0 (a `& 31` mask would shift by 8).
  {"lsl_by_40",
   "long f(long a){unsigned int x=(unsigned int)a,n=40,r;"
   "__asm__ volatile(\"lsl %0,%1,%2\":\"=r\"(r):\"r\"(x),\"r\"(n):);return r;}\n",
   {0xFF}, "ShiftCount", 0},

  // LSR by 33 -> 0 (a `& 31` mask would shift by 1).
  {"lsr_by_33",
   "long f(long a){unsigned int x=(unsigned int)a,n=33,r;"
   "__asm__ volatile(\"lsr %0,%1,%2\":\"=r\"(r):\"r\"(x),\"r\"(n):);return r;}\n",
   {0x80000000u}, "ShiftCount", 0},

  // ASR by 40 of a negative value -> all sign bits (0xFFFFFFFF).
  {"asr_by_40_neg",
   "long f(long a){unsigned int x=(unsigned int)a,n=40,r;"
   "__asm__ volatile(\"asr %0,%1,%2\":\"=r\"(r):\"r\"(x),\"r\"(n):);return r;}\n",
   {0x80000000u}, "ShiftCount", 0},

  // ASR by 40 of a positive value -> 0.
  {"asr_by_40_pos",
   "long f(long a){unsigned int x=(unsigned int)a,n=40,r;"
   "__asm__ volatile(\"asr %0,%1,%2\":\"=r\"(r):\"r\"(x),\"r\"(n):);return r;}\n",
   {0x7FFFFFFFu}, "ShiftCount", 0},

  // Amount whose low byte is 0 (256) -> no shift (value unchanged).  This guards
  // that the fix masks the low byte (not "don't mask + saturate", which would 0).
  {"lsl_by_256",
   "long f(long a){unsigned int x=(unsigned int)a,n=256,r;"
   "__asm__ volatile(\"lsl %0,%1,%2\":\"=r\"(r):\"r\"(x),\"r\"(n):);return r;}\n",
   {0xABCD}, "ShiftCount", 0},

  // ROR by 40 == ROR by 8 (mod-32 control; should already be correct).
  {"ror_by_40",
   "long f(long a){unsigned int x=(unsigned int)a,n=40,r;"
   "__asm__ volatile(\"ror %0,%1,%2\":\"=r\"(r):\"r\"(x),\"r\"(n):);return r;}\n",
   {0x12345678u}, "ShiftCount", 0},

  // Barrel-shifter operand2 form: add r0, r0, r1 lsl r2 with r2=40 -> +0.
  {"add_lsl_reg_40",
   "long f(long a,long b){unsigned int base=(unsigned int)a,x=(unsigned int)b,n=40,r;"
   "__asm__ volatile(\"add %0,%1,%2,lsl %3\":\"=r\"(r):\"r\"(base),\"r\"(x),\"r\"(n):);"
   "return r;}\n",
   {0x1000, 0xFF}, "ShiftCount", 0},

  // Control: small amounts (< 32) must stay correct.
  {"lsl_by_5",
   "long f(long a){unsigned int x=(unsigned int)a,n=5,r;"
   "__asm__ volatile(\"lsl %0,%1,%2\":\"=r\"(r):\"r\"(x),\"r\"(n):);return r;}\n",
   {0xABCD}, "ShiftCount", 0},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(ShiftCount, ARM32ShiftCountRT,
                         ::testing::ValuesIn(kARM32), rtTCName);
