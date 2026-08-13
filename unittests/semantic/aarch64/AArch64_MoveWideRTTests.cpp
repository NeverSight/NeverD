//===- AArch64_MoveWideRTTests.cpp - MOVZ/MOVN/MOVK wide immediates -*- C++ -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// The AArch64 move-wide-immediate family writes a 16-bit immediate into one of
// the four 16-bit lanes of a register, selected by an LSL #{0,16,32,48} shift:
//
//   MOVZ Xd,#imm,lsl#s  : Xd = imm << s            (other lanes ZEROED)
//   MOVN Xd,#imm,lsl#s  : Xd = ~(imm << s)         (NOT of the above)
//   MOVK Xd,#imm,lsl#s  : Xd[s+15:s] = imm         (other lanes PRESERVED)
//
// Two things make this bug-prone, and BOTH were untested by the existing
// roundtrip probe (AArch64_AutoRT `movk`, which does `movz #0x1234; movk
// #0x5678,lsl#16` — i.e. MOVK over a ZERO background, only lanes 0/1, 64-bit
// only, no runtime input):
//
//   1. MOVK must PRESERVE the other three lanes.  With a zero background a
//      clobber of lanes 2/3 (e.g. a wrong mask, or treating MOVK like MOVZ) is
//      invisible.  Here MOVK runs over a runtime-seeded background that is
//      non-zero in every lane, and the WHOLE register is folded into the return,
//      so a clobbered lane diverges.
//   2. The 32-bit (Wn) forms must ZERO the upper 32 bits of the X register (every
//      W write does).  Seeding the register non-zero first makes that observable:
//      if the lifter forgets the upper-half clear, the lifted result keeps the
//      old top half and diverges.
//
// All forms are base ARMv8-A and native on the default Unicorn arm64 CPU; this
// is a pure lift-coverage round.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64MoveWideRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64MoveWideRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off
// MOVK over a runtime background in register %0; fold the full result.
#define MOVK_X(SH) \
  "unsigned long f(unsigned long a){unsigned long r=a;\n" \
  "  __asm__ volatile(\"movk %0, #0xBEEF, lsl #" #SH "\":\"+r\"(r));\n" \
  "  return r;}\n"
// MOVK on Wn: lane preserved within W, upper 32 of X zeroed.
#define MOVK_W(SH) \
  "unsigned long f(unsigned long a){unsigned long r=a;\n" \
  "  __asm__ volatile(\"movk %w0, #0xBEEF, lsl #" #SH "\":\"+r\"(r));\n" \
  "  return r;}\n"
#define MOVZ_X(SH) \
  "unsigned long f(unsigned long a){unsigned long r=a;\n" \
  "  __asm__ volatile(\"movz %0, #0x1234, lsl #" #SH "\":\"+r\"(r));\n" \
  "  return r;}\n"
#define MOVZ_W(SH) \
  "unsigned long f(unsigned long a){unsigned long r=a;\n" \
  "  __asm__ volatile(\"movz %w0, #0x1234, lsl #" #SH "\":\"+r\"(r));\n" \
  "  return r;}\n"
#define MOVN_X(SH) \
  "unsigned long f(unsigned long a){unsigned long r=a;\n" \
  "  __asm__ volatile(\"movn %0, #0x1234, lsl #" #SH "\":\"+r\"(r));\n" \
  "  return r;}\n"
#define MOVN_W(SH) \
  "unsigned long f(unsigned long a){unsigned long r=a;\n" \
  "  __asm__ volatile(\"movn %w0, #0x1234, lsl #" #SH "\":\"+r\"(r));\n" \
  "  return r;}\n"

static const std::vector<RoundTripTC> kA64 = {
  // ===== MOVK preservation at every lane over a non-zero background. =====
  {"movk_x_lsl0",  MOVK_X(0),  {0x1111222233334444ULL}, "MoveWide"},
  {"movk_x_lsl16", MOVK_X(16), {0x1111222233334444ULL}, "MoveWide"},
  {"movk_x_lsl32", MOVK_X(32), {0x1111222233334444ULL}, "MoveWide"},
  {"movk_x_lsl48", MOVK_X(48), {0x1111222233334444ULL}, "MoveWide"},
  {"movk_x_lsl48_alt", MOVK_X(48), {0xFFFFFFFFFFFFFFFFULL}, "MoveWide"},

  // ===== MOVK on Wn: in-W lane preserved, upper 32 of X zeroed. =====
  {"movk_w_lsl0",  MOVK_W(0),  {0x1111222233334444ULL}, "MoveWide"},
  {"movk_w_lsl16", MOVK_W(16), {0x1111222233334444ULL}, "MoveWide"},

  // ===== MOVZ at shifted lanes (others zeroed); X and W (upper-zeroing). =====
  {"movz_x_lsl0",  MOVZ_X(0),  {0xAAAABBBBCCCCDDDDULL}, "MoveWide"},
  {"movz_x_lsl16", MOVZ_X(16), {0xAAAABBBBCCCCDDDDULL}, "MoveWide"},
  {"movz_x_lsl48", MOVZ_X(48), {0xAAAABBBBCCCCDDDDULL}, "MoveWide"},
  {"movz_w_lsl0",  MOVZ_W(0),  {0xAAAABBBBCCCCDDDDULL}, "MoveWide"},
  {"movz_w_lsl16", MOVZ_W(16), {0xAAAABBBBCCCCDDDDULL}, "MoveWide"},

  // ===== MOVN (bitwise NOT of the shifted immediate); X and W. =====
  {"movn_x_lsl0",  MOVN_X(0),  {0x0123456789ABCDEFULL}, "MoveWide"},
  {"movn_x_lsl16", MOVN_X(16), {0x0123456789ABCDEFULL}, "MoveWide"},
  {"movn_x_lsl32", MOVN_X(32), {0x0123456789ABCDEFULL}, "MoveWide"},
  {"movn_w_lsl0",  MOVN_W(0),  {0x0123456789ABCDEFULL}, "MoveWide"},
  {"movn_w_lsl16", MOVN_W(16), {0x0123456789ABCDEFULL}, "MoveWide"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(MoveWide, A64MoveWideRT,
                         ::testing::ValuesIn(kA64), rtTCName);
