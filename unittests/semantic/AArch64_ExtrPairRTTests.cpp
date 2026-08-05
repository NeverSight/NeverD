//===- AArch64_ExtrPairRTTests.cpp - EXTR / ROR-imm pair RT ----*- C++ -===//
//
// AArch64 EXTR concatenates the source pair {Rn:Rm} into a double-width value
// and extracts a `datasize`-wide window starting at bit `lsb`:
//
//   Rd = (Rm >> lsb) | (Rn << (datasize - lsb))          [lsb in 1..datasize-1]
//
// ROR Xd,Xn,#sh is the architectural alias where Rn == Rm.
//
// The pre-existing coverage (AArch64_SemanticTests `extr x0,x1,x2,#4`,
// AArch64_AutoRoundTripTests `#16`, AArch64_CarryBitfieldRTTests `#20`) all use
// a STRICTLY-POSITIVE lsb, leaving two classic blind spots open:
//
//   1. lsb == 0.  EXTR with a zero shift is just `Rd = Rm` (no rotation).  A
//      naive `Rm | (Rn << (datasize - lsb))` lowers the high term to
//      `Rn << datasize` — a shift-by-bitwidth, which is UB/poison in LLVM (and,
//      if a backend instead masks the count to 0, degenerates to `Rm | Rn`).
//      EITHER way the result diverges from the correct `Rm` once Rn and Rm have
//      bits that the other lacks.  The ROR-immediate alias path already guards
//      this (it COPYs on shift==0); the 4-operand EXTR path must too.  This is
//      the same shift-by-bitwidth landmine rooted out in #309 (CSETM).
//
//   2. The 32-bit (Wd) form must zero bits 63:32 of the X register, and its own
//      `Wn << (32 - lsb)` must not bleed past bit 31.  lsb==0 here lowers to
//      `Wn << 32`, the 32-bit instance of the same UB.
//
// Rn and Rm are distinct, bit-rich values supplied as runtime arguments so the
// oracle (original-Unicorn vs lifted-Unicorn) sees a real divergence for any
// mis-extracted / clobbered bit.  All forms are base ARMv8-A, native on the
// default Unicorn arm64 CPU.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64ExtrPairRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64ExtrPairRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off
// %1 = a = Rn (the "high" source), %2 = b = Rm (the "low" source).
#define EXTR_X(LSB) \
  "unsigned long f(unsigned long a, unsigned long b){unsigned long r;\n" \
  "  __asm__ volatile(\"extr %0,%1,%2,#" #LSB "\":\"=r\"(r):\"r\"(a),\"r\"(b));\n" \
  "  return r;}\n"
#define EXTR_W(LSB) \
  "unsigned long f(unsigned long a, unsigned long b){unsigned long r;\n" \
  "  __asm__ volatile(\"extr %w0,%w1,%w2,#" #LSB "\":\"=r\"(r):\"r\"(a),\"r\"(b));\n" \
  "  return r;}\n"
// ROR-immediate alias (Rn == Rm), exercises the 2-operand alias path.
#define ROR_X(SH) \
  "unsigned long f(unsigned long a){unsigned long r;\n" \
  "  __asm__ volatile(\"ror %0,%1,#" #SH "\":\"=r\"(r):\"r\"(a));\n" \
  "  return r;}\n"
#define ROR_W(SH) \
  "unsigned long f(unsigned long a){unsigned long r;\n" \
  "  __asm__ volatile(\"ror %w0,%w1,#" #SH "\":\"=r\"(r):\"r\"(a));\n" \
  "  return r;}\n"

static const unsigned long long RN = 0xAABBCCDD11223344ULL;
static const unsigned long long RM = 0x55667788F0F0F0F0ULL;

static const std::vector<RoundTripTC> kA64 = {
  // ===== EXTR X-form: lsb==0 is the shift-by-bitwidth landmine. =====
  // Correct result = Rm (no rotation).  Rm|Rn != Rm with these inputs, so a
  // mishandled `Rn << 64` (poison OR masked-to-0) is observable.
  {"extr_x_lsb0",  EXTR_X(0),  {RN, RM}, "ExtrPair"},
  // ----- non-zero lsb guardrails (lock the believed-correct path). -----
  {"extr_x_lsb1",  EXTR_X(1),  {RN, RM}, "ExtrPair"},
  {"extr_x_lsb16", EXTR_X(16), {RN, RM}, "ExtrPair"},
  {"extr_x_lsb32", EXTR_X(32), {RN, RM}, "ExtrPair"},
  {"extr_x_lsb63", EXTR_X(63), {RN, RM}, "ExtrPair"},

  // ===== EXTR W-form: lsb==0 (Wn << 32 UB) + bits 63:32 must be zeroed. =====
  {"extr_w_lsb0",  EXTR_W(0),  {RN, RM}, "ExtrPair"},
  {"extr_w_lsb1",  EXTR_W(1),  {RN, RM}, "ExtrPair"},
  {"extr_w_lsb16", EXTR_W(16), {RN, RM}, "ExtrPair"},
  {"extr_w_lsb31", EXTR_W(31), {RN, RM}, "ExtrPair"},

  // ===== ROR immediate alias (Rn == Rm). sh==0 must COPY (no rotation). =====
  {"ror_x_0",  ROR_X(0),  {RN}, "ExtrPair"},
  {"ror_x_13", ROR_X(13), {RN}, "ExtrPair"},
  {"ror_x_63", ROR_X(63), {RN}, "ExtrPair"},
  {"ror_w_0",  ROR_W(0),  {RN}, "ExtrPair"},
  {"ror_w_17", ROR_W(17), {RN}, "ExtrPair"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(ExtrPair, A64ExtrPairRT,
                         ::testing::ValuesIn(kA64), rtTCName);
