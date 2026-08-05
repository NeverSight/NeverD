//===- AllPlatform_FPToIntSatRTTests.cpp - FP->int out-of-range -----------===//
//
// FP-to-integer conversion out-of-range / NaN behavior.  Raw LLVM fptosi/fptoui
// is UB (poison) when the value does not fit or is NaN; a compile-time-constant
// over-range operand folds to `ret poison`, which the optimizer miscompiles.
// Hardware is well-defined:
//   * AArch64 fcvtzs/fcvtzu, ARM32 vcvt: saturate (NaN -> 0).
//   * x86 cvttss2si/cvttsd2si: "integer indefinite" = INT_MIN for any
//     out-of-range / NaN / Inf.
// The conversion source is materialized as a compile-time constant (via an
// integer->FP register move) so the optimizer sees a constant feeding the
// conversion and would fold it to poison without the fix.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64FPToIntSatRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64FPToIntSatRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32FPToIntSatRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32FPToIntSatRT, Verify) { roundTripARM32(GetParam()); }

class X64FPToIntSatRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64FPToIntSatRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kA64 = {
  // FCVTZS f64->i32, +overflow (1e16) -> INT_MAX (saturate).
  {"fcvtzs_w_ovf",
   "int f(long a){ long bits=0x4341c37937e08000L; double d; int r;\n"
   "  __asm__(\"fmov %d0,%1\":\"=w\"(d):\"r\"(bits));\n"
   "  __asm__(\"fcvtzs %w0,%d1\":\"=r\"(r):\"w\"(d));\n"
   "  return r; }\n",
   {0}, "FPToIntSat", 1, "-march=armv8-a"},

  // FCVTZS f64->i32, -overflow (-1e16) -> INT_MIN.
  {"fcvtzs_w_neg",
   "int f(long a){ long bits=0xC341c37937e08000L; double d; int r;\n"
   "  __asm__(\"fmov %d0,%1\":\"=w\"(d):\"r\"(bits));\n"
   "  __asm__(\"fcvtzs %w0,%d1\":\"=r\"(r):\"w\"(d));\n"
   "  return r; }\n",
   {0}, "FPToIntSat", 1, "-march=armv8-a"},

  // FCVTZS f64->i32, NaN -> 0.
  {"fcvtzs_w_nan",
   "int f(long a){ long bits=0x7FF8000000000000L; double d; int r;\n"
   "  __asm__(\"fmov %d0,%1\":\"=w\"(d):\"r\"(bits));\n"
   "  __asm__(\"fcvtzs %w0,%d1\":\"=r\"(r):\"w\"(d));\n"
   "  return r; }\n",
   {0}, "FPToIntSat", 1, "-march=armv8-a"},

  // FCVTZU f64->i32, +overflow -> UINT_MAX.
  {"fcvtzu_w_ovf",
   "long f(long a){ long bits=0x4341c37937e08000L; double d; unsigned r;\n"
   "  __asm__(\"fmov %d0,%1\":\"=w\"(d):\"r\"(bits));\n"
   "  __asm__(\"fcvtzu %w0,%d1\":\"=r\"(r):\"w\"(d));\n"
   "  return (long)r; }\n",
   {0}, "FPToIntSat", 1, "-march=armv8-a"},

  // FCVTZU f64->i32, negative -> 0.
  {"fcvtzu_w_neg",
   "long f(long a){ long bits=0xBFF0000000000000L; double d; unsigned r;\n"
   "  __asm__(\"fmov %d0,%1\":\"=w\"(d):\"r\"(bits));\n"
   "  __asm__(\"fcvtzu %w0,%d1\":\"=r\"(r):\"w\"(d));\n"
   "  return (long)r; }\n",
   {0}, "FPToIntSat", 1, "-march=armv8-a"},

  // FCVTZS f64->i64, +overflow (1e30) -> INT64_MAX.
  {"fcvtzs_x_ovf",
   "long f(long a){ long bits=0x46293E5939A08CEAL; double d; long r;\n"
   "  __asm__(\"fmov %d0,%1\":\"=w\"(d):\"r\"(bits));\n"
   "  __asm__(\"fcvtzs %x0,%d1\":\"=r\"(r):\"w\"(d));\n"
   "  return r; }\n",
   {0}, "FPToIntSat", 1, "-march=armv8-a"},

  // Control: in-range value converts normally (no fixup change).
  {"fcvtzs_w_inrange",
   "int f(long a){ long bits=0x40C3880000000000L; double d; int r;\n"  // 10000.0
   "  __asm__(\"fmov %d0,%1\":\"=w\"(d):\"r\"(bits));\n"
   "  __asm__(\"fcvtzs %w0,%d1\":\"=r\"(r):\"w\"(d));\n"
   "  return r; }\n",
   {0}, "FPToIntSat", 1, "-march=armv8-a"},
};

static const std::vector<RoundTripTC> kX64 = {
  // CVTTSD2SI f64->i32, +overflow (1e16) -> INT_MIN (integer indefinite).
  {"cvttsd2si_ovf",
   "int f(long a){ long bits=0x4341c37937e08000L; double d; int r;\n"
   "  __asm__(\"movq %1,%0\":\"=x\"(d):\"r\"(bits));\n"
   "  __asm__(\"cvttsd2si %1,%0\":\"=r\"(r):\"x\"(d));\n"
   "  return r; }\n",
   {0}, "FPToIntSat", 1, ""},

  // CVTTSD2SI f64->i32, -overflow (-1e16) -> INT_MIN.
  {"cvttsd2si_neg",
   "int f(long a){ long bits=0xC341c37937e08000L; double d; int r;\n"
   "  __asm__(\"movq %1,%0\":\"=x\"(d):\"r\"(bits));\n"
   "  __asm__(\"cvttsd2si %1,%0\":\"=r\"(r):\"x\"(d));\n"
   "  return r; }\n",
   {0}, "FPToIntSat", 1, ""},

  // CVTTSD2SI f64->i32, NaN -> INT_MIN.
  {"cvttsd2si_nan",
   "int f(long a){ long bits=0x7FF8000000000000L; double d; int r;\n"
   "  __asm__(\"movq %1,%0\":\"=x\"(d):\"r\"(bits));\n"
   "  __asm__(\"cvttsd2si %1,%0\":\"=r\"(r):\"x\"(d));\n"
   "  return r; }\n",
   {0}, "FPToIntSat", 1, ""},

  // CVTTSD2SI f64->i64, +overflow (1e30) -> INT64_MIN.
  {"cvttsd2si_q_ovf",
   "long f(long a){ long bits=0x46293E5939A08CEAL; double d; long r;\n"
   "  __asm__(\"movq %1,%0\":\"=x\"(d):\"r\"(bits));\n"
   "  __asm__(\"cvttsd2si %1,%0\":\"=r\"(r):\"x\"(d));\n"
   "  return r; }\n",
   {0}, "FPToIntSat", 1, ""},

  // CVTTSS2SI f32->i32, +overflow -> INT_MIN.
  {"cvttss2si_ovf",
   "int f(long a){ int bits=0x58635FA9; float d; int r;\n"  // ~1e15 f32
   "  __asm__(\"movd %1,%0\":\"=x\"(d):\"r\"(bits));\n"
   "  __asm__(\"cvttss2si %1,%0\":\"=r\"(r):\"x\"(d));\n"
   "  return r; }\n",
   {0}, "FPToIntSat", 1, ""},

  // Control: in-range value.
  {"cvttsd2si_inrange",
   "int f(long a){ long bits=0x40C3880000000000L; double d; int r;\n"  // 10000.0
   "  __asm__(\"movq %1,%0\":\"=x\"(d):\"r\"(bits));\n"
   "  __asm__(\"cvttsd2si %1,%0\":\"=r\"(r):\"x\"(d));\n"
   "  return r; }\n",
   {0}, "FPToIntSat", 1, ""},
};

static const std::vector<RoundTripTC> kARM32 = {
  // VCVT.S32.F64 +overflow -> INT_MAX (saturate).
  {"vcvt_s32_ovf",
   "int f(long a){ long long bits=0x4341c37937e08000LL; double d; int r;\n"
   "  __asm__(\"vmov %P0,%Q1,%R1\":\"=w\"(d):\"r\"(bits));\n"
   "  __asm__(\"vcvt.s32.f64 %0,%P1\":\"=t\"(r):\"w\"(d));\n"
   "  return r; }\n",
   {0}, "FPToIntSat", 1, ""},

  // VCVT.U32.F64 +overflow -> UINT_MAX.
  {"vcvt_u32_ovf",
   "long f(long a){ long long bits=0x4341c37937e08000LL; double d; unsigned r;\n"
   "  __asm__(\"vmov %P0,%Q1,%R1\":\"=w\"(d):\"r\"(bits));\n"
   "  __asm__(\"vcvt.u32.f64 %0,%P1\":\"=t\"(r):\"w\"(d));\n"
   "  return (long)r; }\n",
   {0}, "FPToIntSat", 1, ""},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(FPToIntSat, A64FPToIntSatRT, ::testing::ValuesIn(kA64),
                         [](const auto &I) { return I.param.Name; });
INSTANTIATE_TEST_SUITE_P(FPToIntSat, X64FPToIntSatRT, ::testing::ValuesIn(kX64),
                         [](const auto &I) { return I.param.Name; });
INSTANTIATE_TEST_SUITE_P(FPToIntSat, ARM32FPToIntSatRT,
                         ::testing::ValuesIn(kARM32),
                         [](const auto &I) { return I.param.Name; });
