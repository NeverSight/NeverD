//===- AArch64_BitfieldClearRTTests.cpp - BFC roundtrip ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// AArch64 BFC Xd,#lsb,#width clears `width` bits starting at bit `lsb`:
//   Xd<lsb+width-1 : lsb> = 0   (all other bits unchanged)
//
// BFC is the source-less alias of BFM (the implicit operand is the zero
// register), so Capstone surfaces it with only THREE operands
// (Xd,#lsb,#width) and no Rn.  The BFM handler only recognised the BFI and
// BFXIL aliases (both 4-operand) and `break`ed out of the >=4-operand path for
// BFC, leaving Rd COMPLETELY UNCHANGED — a silent no-op.  clang emits BFC for
// `x &= ~(mask << lsb)` idioms, so this had real-world reach yet ZERO prior
// roundtrip coverage.  Probes cover low/middle/high fields, both register
// widths, single-bit and full-width clears.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64BitfieldClearRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64BitfieldClearRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kA64BFC = {

  // ===== 64-bit: clear 8 bits at lsb 4 of all-ones -> 0xFFFF_FFFF_FFFF_F00F.
  // (RED before fix: lifter left Rd = 0xFFFFFFFFFFFFFFFF unchanged.)
  {"bfc_x_mid",
   "unsigned long f(unsigned long a){unsigned long r=a;"
   "__asm__ volatile(\"bfc %0,#4,#8\":\"+r\"(r));return r;}\n",
   {0xFFFFFFFFFFFFFFFFULL}, "BFClear"},

  // 64-bit: clear low 16 bits.
  {"bfc_x_low16",
   "unsigned long f(unsigned long a){unsigned long r=a;"
   "__asm__ volatile(\"bfc %0,#0,#16\":\"+r\"(r));return r;}\n",
   {0xDEADBEEFCAFEBABEULL}, "BFClear"},

  // 64-bit: clear a high field [47:40] (lsb 40, width 8).
  {"bfc_x_high",
   "unsigned long f(unsigned long a){unsigned long r=a;"
   "__asm__ volatile(\"bfc %0,#40,#8\":\"+r\"(r));return r;}\n",
   {0xFFFFFFFFFFFFFFFFULL}, "BFClear"},

  // 64-bit: single-bit clear at bit 63 (sign bit).
  {"bfc_x_bit63",
   "unsigned long f(unsigned long a){unsigned long r=a;"
   "__asm__ volatile(\"bfc %0,#63,#1\":\"+r\"(r));return r;}\n",
   {0xFFFFFFFFFFFFFFFFULL}, "BFClear"},

  // 32-bit (Wd): clear 12 bits at lsb 8 -> upper 12 bits of result come from src.
  {"bfc_w_mid",
   "unsigned f(unsigned a){unsigned r=a;"
   "__asm__ volatile(\"bfc %w0,#8,#12\":\"+r\"(r));return r;}\n",
   {0xFFFFFFFFu}, "BFClear"},

  // 32-bit: clear the whole register (lsb 0, width 32).
  {"bfc_w_all",
   "unsigned f(unsigned a){unsigned r=a;"
   "__asm__ volatile(\"bfc %w0,#0,#32\":\"+r\"(r));return r;}\n",
   {0xDEADBEEFu}, "BFClear"},

  // 32-bit: clear top byte [31:24].
  {"bfc_w_topbyte",
   "unsigned f(unsigned a){unsigned r=a;"
   "__asm__ volatile(\"bfc %w0,#24,#8\":\"+r\"(r));return r;}\n",
   {0xAABBCCDDu}, "BFClear"},

  // Mixed pattern: clear [19:12] of a structured value, keep the rest.
  {"bfc_x_struct",
   "unsigned long f(unsigned long a){unsigned long r=a;"
   "__asm__ volatile(\"bfc %0,#12,#8\":\"+r\"(r));return r;}\n",
   {0x1122334455667788ULL}, "BFClear"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(BFClear, A64BitfieldClearRT,
                         ::testing::ValuesIn(kA64BFC), rtTCName);
