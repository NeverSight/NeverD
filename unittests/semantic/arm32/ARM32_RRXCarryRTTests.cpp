//===- ARM32_RRXCarryRTTests.cpp - RRX operand + shifter carry ---*-C++-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// RRX (rotate right through carry by one) is a barrel-shifter mode with two
// distinct semantic effects the harness must reproduce:
//
//   value :  Rm' = (Rm >> 1) | (Cin << 31)        -- consumes the carry-IN
//   carry :  for the FLAG-SETTING logical forms (ANDS/ORRS/EORS/BICS/TST/TEQ/
//            MOVS/MVNS) C is set to the shifter carry-OUT = Rm[0]
//
// Capstone reports RRX with a zero shift amount, so older code paths that keyed
// off "amount != 0" dropped it entirely.  `ARM32_ShiftedIndexRTTests` locked in
// the *value* path for `mov Rd,Rm,rrx` and `ldr [Rn,Rm,rrx]`, and
// `ARM32_LogicShiftCarryRTTests` locked in the shifter *carry-out* for every
// shift type EXCEPT rrx (only LSL/LSR/ASR/ROR #imm, LSL reg, rot-imm).  This
// file fills both remaining gaps:
//
//   A. RRX used as the shifted second operand of the general binary data-
//      processing ops (AND/ORR/EOR/BIC/ADD/SUB/RSB).  The result is folded into
//      the return value; carry-IN is pinned with a preceding `cmp` so the bit
//      rotated into Rm'[31] is deterministic.
//   B. RRX shifter carry-OUT of the flag-setting logical forms, folded with a
//      trailing `adc Rd,Rd,#0` (Rd preset 0) exactly like the sibling file.
//      The carry-out is Rm[0] regardless of carry-in, so both Rm[0]=0 and
//      Rm[0]=1 operands are driven.
//
// The oracle compares original-Unicorn vs lifted-Unicorn (no hand-computed
// expected); a dropped rotate, a dropped/zero-width carry-out, or a missed
// emitLogicalOpCarry RRX arm all diverge.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32RRXCarryRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32RRXCarryRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
// Pin carry-IN = 1: cmp r12,r12 (0-0, no borrow -> C=1).  The "\\n\\t" survive
// into the generated C source as escape sequences inside the asm string.
#define CIN1 "mov r12,#0\\n\\tcmp r12,r12\\n\\t"
// Pin carry-IN = 0: cmp r12,#1 with r12=0 (0-1 borrows -> C=0).
#define CIN0 "mov r12,#0\\n\\tcmp r12,#1\\n\\t"

static const std::vector<RoundTripTC> kARM = {

  // ===== Group A: RRX as the shifted operand of a binary data-processing op.
  //       Result folded into the return; carry-IN pinned so Rm'[31] is fixed. =

  // AND Rd, Rn, Rm, rrx  (carry-in 1 -> Rm'[31]=1).
  {"and_rrx_c1",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"" CIN1 "and %0,%1,%2,rrx\""
   ":\"=r\"(r):\"r\"(a),\"r\"(b):\"r12\",\"cc\");return r;}\n",
   {0xFFFFFFFFULL, 0x00000003ULL}, "RRXCarry"},

  // ORR Rd, Rn, Rm, rrx  (carry-in 0 -> Rm'[31]=0).
  {"orr_rrx_c0",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"" CIN0 "orr %0,%1,%2,rrx\""
   ":\"=r\"(r):\"r\"(a),\"r\"(b):\"r12\",\"cc\");return r;}\n",
   {0x00000000ULL, 0x80000006ULL}, "RRXCarry"},

  // EOR Rd, Rn, Rm, rrx  (carry-in 1).
  {"eor_rrx_c1",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"" CIN1 "eor %0,%1,%2,rrx\""
   ":\"=r\"(r):\"r\"(a),\"r\"(b):\"r12\",\"cc\");return r;}\n",
   {0x0F0F0F0FULL, 0x00000005ULL}, "RRXCarry"},

  // BIC Rd, Rn, Rm, rrx  (carry-in 1): Rn & ~rrx(Rm).
  {"bic_rrx_c1",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"" CIN1 "bic %0,%1,%2,rrx\""
   ":\"=r\"(r):\"r\"(a),\"r\"(b):\"r12\",\"cc\");return r;}\n",
   {0xFFFFFFFFULL, 0x00000007ULL}, "RRXCarry"},

  // ADD Rd, Rn, Rm, rrx  (carry-in 1): adds the rotated operand (not a flag op,
  // so the ADD's own carry is irrelevant here).
  {"add_rrx_c1",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"" CIN1 "add %0,%1,%2,rrx\""
   ":\"=r\"(r):\"r\"(a),\"r\"(b):\"r12\",\"cc\");return r;}\n",
   {0x00000010ULL, 0x00000004ULL}, "RRXCarry"},

  // SUB Rd, Rn, Rm, rrx  (carry-in 0).
  {"sub_rrx_c0",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"" CIN0 "sub %0,%1,%2,rrx\""
   ":\"=r\"(r):\"r\"(a),\"r\"(b):\"r12\",\"cc\");return r;}\n",
   {0x00001000ULL, 0x00000008ULL}, "RRXCarry"},

  // RSB Rd, Rn, Rm, rrx  (carry-in 1): rrx(Rm) - Rn.
  {"rsb_rrx_c1",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"" CIN1 "rsb %0,%1,%2,rrx\""
   ":\"=r\"(r):\"r\"(a),\"r\"(b):\"r12\",\"cc\");return r;}\n",
   {0x00000001ULL, 0x00000002ULL}, "RRXCarry"},

  // ===== Group B: RRX shifter carry-OUT (= Rm[0]) of the flag-setting logical
  //       forms, folded via `adc Rd,Rd,#0`.  Carry-in is pinned to 1 so the
  //       captured carry can only come from Rm[0], not a leftover state. =======

  // ANDS, Rm[0]=1 -> carry-out 1.
  {"ands_rrx_cout1",
   "long f(long a,long b){unsigned long c=0;"
   "__asm__ volatile(\"" CIN1 "ands r3,%1,%2,rrx\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a),\"r\"(b):\"r3\",\"r12\",\"cc\");return c;}\n",
   {0xFFFFFFFFULL, 0x00000001ULL}, "RRXCarry"},

  // ANDS, Rm[0]=0 -> carry-out 0.
  {"ands_rrx_cout0",
   "long f(long a,long b){unsigned long c=0;"
   "__asm__ volatile(\"" CIN1 "ands r3,%1,%2,rrx\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a),\"r\"(b):\"r3\",\"r12\",\"cc\");return c;}\n",
   {0xFFFFFFFFULL, 0x00000002ULL}, "RRXCarry"},

  // ORRS, Rm[0]=1.
  {"orrs_rrx_cout1",
   "long f(long a,long b){unsigned long c=0;"
   "__asm__ volatile(\"" CIN1 "orrs r3,%1,%2,rrx\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a),\"r\"(b):\"r3\",\"r12\",\"cc\");return c;}\n",
   {0x00000000ULL, 0x12345679ULL}, "RRXCarry"},

  // EORS, Rm[0]=0.
  {"eors_rrx_cout0",
   "long f(long a,long b){unsigned long c=0;"
   "__asm__ volatile(\"" CIN1 "eors r3,%1,%2,rrx\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a),\"r\"(b):\"r3\",\"r12\",\"cc\");return c;}\n",
   {0x00000000ULL, 0x12345678ULL}, "RRXCarry"},

  // BICS, Rm[0]=1.
  {"bics_rrx_cout1",
   "long f(long a,long b){unsigned long c=0;"
   "__asm__ volatile(\"" CIN1 "bics r3,%1,%2,rrx\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a),\"r\"(b):\"r3\",\"r12\",\"cc\");return c;}\n",
   {0xFFFFFFFFULL, 0x0000000FULL}, "RRXCarry"},

  // TST, Rm[0]=1 (no destination register).
  {"tst_rrx_cout1",
   "long f(long a,long b){unsigned long c=0;"
   "__asm__ volatile(\"" CIN1 "tst %1,%2,rrx\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a),\"r\"(b):\"r12\",\"cc\");return c;}\n",
   {0xFFFFFFFFULL, 0x00000001ULL}, "RRXCarry"},

  // TEQ, Rm[0]=0.
  {"teq_rrx_cout0",
   "long f(long a,long b){unsigned long c=0;"
   "__asm__ volatile(\"" CIN1 "teq %1,%2,rrx\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a),\"r\"(b):\"r12\",\"cc\");return c;}\n",
   {0xFFFFFFFFULL, 0x00000010ULL}, "RRXCarry"},

  // MOVS Rd, Rm, rrx, Rm[0]=1.
  {"movs_rrx_cout1",
   "long f(long a){unsigned long c=0;"
   "__asm__ volatile(\"" CIN1 "movs r3,%1,rrx\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a):\"r3\",\"r12\",\"cc\");return c;}\n",
   {0x00000003ULL}, "RRXCarry"},

  // MVNS Rd, Rm, rrx, Rm[0]=0.
  {"mvns_rrx_cout0",
   "long f(long a){unsigned long c=0;"
   "__asm__ volatile(\"" CIN1 "mvns r3,%1,rrx\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c):\"r\"(a):\"r3\",\"r12\",\"cc\");return c;}\n",
   {0x00000004ULL}, "RRXCarry"},

  // ===== Combined: fold BOTH the rotated result and the carry-out so a partial
  //       implementation (value right but carry dropped, or vice versa) shows. =
  {"ands_rrx_val_and_cout",
   "long f(long a,long b){unsigned long c=0;unsigned long r;"
   "__asm__ volatile(\"" CIN1 "ands %1,%2,%3,rrx\\n\\tadc %0,%0,#0\""
   ":\"+r\"(c),\"=&r\"(r):\"r\"(a),\"r\"(b):\"r12\",\"cc\");"
   "return r*31u + c;}\n",
   {0xFFFFFFFFULL, 0x00000005ULL}, "RRXCarry"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(RRXCarry, ARM32RRXCarryRT,
                         ::testing::ValuesIn(kARM), rtTCName);
