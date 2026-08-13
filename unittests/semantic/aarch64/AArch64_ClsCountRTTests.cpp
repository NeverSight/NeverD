//===- AArch64_ClsCountRTTests.cpp - CLS (count leading sign) RT -*- C++ -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// AArch64 CLS returns (number of consecutive bits below the sign bit that EQUAL
// the sign bit), i.e. one less than the total leading-sign run.  The lifter
// implements the standard identity CLS(x) = CLZ(x ^ (x ASR 1)) - 1, computed at
// the operand width (AArch64LiftCore.cpp).
//
// Pre-existing coverage (AArch64_SemanticTests `cls_zero`/`cls_neg`,
// AArch64_CarryBitfieldRTTests `cls_neg`, AArch64_AutoRoundTripTests `cls`) is
// all 64-bit (Xd) and only the 0 / all-ones corners.  Two blind spots remain:
//
//   1. The 32-bit (Wd) form.  CLS must count within 32 bits (result 0..31).  The
//      identity's LZCOUNT lowers to `llvm.ctlz` typed at the operand width; if a
//      regression widened the temp to 64 bits the count would be off by 32 and
//      every Wd probe here would diverge.  The Wd result must also zero X[63:32].
//
//   2. Mid-run corners.  Inputs whose leading-sign run ends at an interior bit
//      (e.g. 0xFFFF0000 -> 15, 0xC0000000 -> 1, 0x7FFFFFFF -> 0) exercise the
//      `x ^ (x asr 1)` boundary; the 0/-1 corners alone (run == width) hide an
//      off-by-one or a logical-vs-arithmetic shift in the ASR step.
//
// The oracle compares original-Unicorn vs lifted-Unicorn, so no count is
// hand-computed; the corner inputs simply guarantee a width/shift bug is
// observable.  CLS is base ARMv8-A, native on the default Unicorn arm64 CPU.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64ClsCountRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64ClsCountRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off
#define CLS_X \
  "unsigned long f(unsigned long a){unsigned long r;\n" \
  "  __asm__ volatile(\"cls %0,%1\":\"=r\"(r):\"r\"(a));\n" \
  "  return r;}\n"
#define CLS_W \
  "unsigned long f(unsigned long a){unsigned long r;\n" \
  "  __asm__ volatile(\"cls %w0,%w1\":\"=r\"(r):\"r\"(a));\n" \
  "  return r;}\n"

static const std::vector<RoundTripTC> kA64 = {
  // ===== Wd form: count within 32 bits, mid-run corners, X[63:32] cleared. =====
  {"cls_w_zero",   CLS_W, {0x0000000000000000ULL}, "ClsCount"}, // ->31
  {"cls_w_ones",   CLS_W, {0x00000000FFFFFFFFULL}, "ClsCount"}, // ->31
  {"cls_w_7fff",   CLS_W, {0x000000007FFFFFFFULL}, "ClsCount"}, // ->0
  {"cls_w_8000",   CLS_W, {0x0000000080000000ULL}, "ClsCount"}, // ->0
  {"cls_w_c000",   CLS_W, {0x00000000C0000000ULL}, "ClsCount"}, // ->1
  {"cls_w_ffff0",  CLS_W, {0x00000000FFFF0000ULL}, "ClsCount"}, // ->15
  {"cls_w_0000ffff",CLS_W,{0x0000000000007FFFULL}, "ClsCount"}, // ->16
  {"cls_w_fffe",   CLS_W, {0x00000000FFFFFFFEULL}, "ClsCount"}, // ->30
  // High 32 bits of the argument are non-zero: the Wd op must ignore them and
  // the result must zero X[63:32]; a stray 64-bit count would leak them.
  {"cls_w_dirtyhi",CLS_W, {0xDEADBEEF40000000ULL}, "ClsCount"}, // ->0

  // ===== Xd cross-check (run ending mid-register). =====
  {"cls_x_zero",   CLS_X, {0x0000000000000000ULL}, "ClsCount"}, // ->63
  {"cls_x_ones",   CLS_X, {0xFFFFFFFFFFFFFFFFULL}, "ClsCount"}, // ->63
  {"cls_x_c000",   CLS_X, {0xC000000000000000ULL}, "ClsCount"}, // ->1
  {"cls_x_ff00",   CLS_X, {0xFFFFFFFF00000000ULL}, "ClsCount"}, // ->31
  {"cls_x_0080",   CLS_X, {0x0000000080000000ULL}, "ClsCount"}, // ->31
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(ClsCount, A64ClsCountRT,
                         ::testing::ValuesIn(kA64), rtTCName);
