//===- ARM32_ShiftCarryRTTests.cpp - ARM32 shifter carry probes -*- C++ -*-===//
//
// The roundtrip harness compares return values only, so the C (carry) flag a
// flag-setting shift produces is invisible unless folded into the result.
// These probes drive LSLS/LSRS/ASRS/RORS and capture the shifter carry with a
// following `adc Rd,Rd,#0` (Rd preset to 0, so Rd becomes C).  ARM ARM: a
// flag-setting shift sets C to the shifter carry-out — LSL #n -> Rm[32-n],
// LSR/ASR #n -> Rm[n-1], ROR #n -> Rm[(n-1) mod 32]; a register amount of 0
// leaves C unchanged.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32ShiftCarryRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32ShiftCarryRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kARM = {
  // ===== LSLS #imm: C = Rm[32-n]. =====
  {"lsls1_c1",
   "long f(long a){unsigned long c=0;"
   "__asm__ volatile(\"lsls r3,%1,#1\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a):\"r3\",\"cc\");return c;}\n",
   {0xC0000000ULL}, "ShiftCarry"},
  {"lsls1_c0",
   "long f(long a){unsigned long c=0;"
   "__asm__ volatile(\"lsls r3,%1,#1\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a):\"r3\",\"cc\");return c;}\n",
   {0x40000000ULL}, "ShiftCarry"},
  {"lsls4_c1",
   "long f(long a){unsigned long c=0;"
   "__asm__ volatile(\"lsls r3,%1,#4\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a):\"r3\",\"cc\");return c;}\n",
   {0x10000000ULL}, "ShiftCarry"},

  // ===== LSRS #imm: C = Rm[n-1]. =====
  {"lsrs1_c1",
   "long f(long a){unsigned long c=0;"
   "__asm__ volatile(\"lsrs r3,%1,#1\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a):\"r3\",\"cc\");return c;}\n",
   {0x00000003ULL}, "ShiftCarry"},
  {"lsrs1_c0",
   "long f(long a){unsigned long c=0;"
   "__asm__ volatile(\"lsrs r3,%1,#1\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a):\"r3\",\"cc\");return c;}\n",
   {0x00000002ULL}, "ShiftCarry"},
  {"lsrs4_c1",
   "long f(long a){unsigned long c=0;"
   "__asm__ volatile(\"lsrs r3,%1,#4\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a):\"r3\",\"cc\");return c;}\n",
   {0x00000008ULL}, "ShiftCarry"},

  // ===== ASRS #imm: C = Rm[n-1]. =====
  {"asrs1_c1",
   "long f(long a){unsigned long c=0;"
   "__asm__ volatile(\"asrs r3,%1,#1\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a):\"r3\",\"cc\");return c;}\n",
   {0x00000003ULL}, "ShiftCarry"},

  // ===== RORS #imm: C = Rm[(n-1) mod 32]. =====
  {"rors1_c1",
   "long f(long a){unsigned long c=0;"
   "__asm__ volatile(\"rors r3,%1,#1\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a):\"r3\",\"cc\");return c;}\n",
   {0x00000001ULL}, "ShiftCarry"},
  {"rors8_c1",
   "long f(long a){unsigned long c=0;"
   "__asm__ volatile(\"rors r3,%1,#8\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a):\"r3\",\"cc\");return c;}\n",
   {0x00000080ULL}, "ShiftCarry"},

  // ===== Register amount: C = Rm[32-n] for LSL, n in Rs[7:0]. =====
  {"lsls_reg_c1",
   "long f(long a,long b){unsigned long c=0;"
   "__asm__ volatile(\"lsls r3,%1,%2\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a),\"r\"(b):\"r3\",\"cc\");return c;}\n",
   {0x00010000ULL, 16}, "ShiftCarry"},
  // Register amount 0: C unchanged (here entry C is 0, so result stays 0).
  {"lsls_reg0_keep",
   "long f(long a,long b){unsigned long c=0;"
   "__asm__ volatile(\"lsls r3,%1,%2\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a),\"r\"(b):\"r3\",\"cc\");return c;}\n",
   {0xFFFFFFFFULL, 0}, "ShiftCarry"},

  // ===== Register-shift result: was lifted as a plain COPY (shift dropped),
  // so these capture the shifted value itself, not the carry. =====
  {"lsls_reg_result",
   "long f(long a,long b){unsigned r;"
   "__asm__ volatile(\"lsls %0,%1,%2\":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");"
   "return r;}\n",
   {0x00000123ULL, 8}, "ShiftCarry"},
  {"lsrs_reg_result",
   "long f(long a,long b){unsigned r;"
   "__asm__ volatile(\"lsrs %0,%1,%2\":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");"
   "return r;}\n",
   {0x00120000ULL, 8}, "ShiftCarry"},
  {"asrs_reg_result",
   "long f(long a,long b){int r;"
   "__asm__ volatile(\"asrs %0,%1,%2\":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");"
   "return r;}\n",
   {(uint64_t)(int64_t)-0x10000, 4}, "ShiftCarry"},
  {"rors_reg_result",
   "long f(long a,long b){unsigned r;"
   "__asm__ volatile(\"rors %0,%1,%2\":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");"
   "return r;}\n",
   {0x000000FFULL, 4}, "ShiftCarry"},
  // Non-flag-setting register shift result (also routed via the MOV encoding).
  {"lsl_reg_result_noS",
   "long f(long a,long b){unsigned r;"
   "__asm__ volatile(\"lsl %0,%1,%2\":\"=r\"(r):\"r\"(a),\"r\"(b));"
   "return r;}\n",
   {0x00000123ULL, 12}, "ShiftCarry"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(ShiftCarry, ARM32ShiftCarryRT, ::testing::ValuesIn(kARM),
                         rtTCName);
