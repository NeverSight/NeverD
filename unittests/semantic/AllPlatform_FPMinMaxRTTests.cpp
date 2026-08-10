//===- AllPlatform_FPMinMaxRTTests.cpp - FP min/max NaN semantics -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for floating-point min/max, which the lifter had implemented
// as a naive (a<b)?a:b select.  That gets two IEEE behaviours wrong:
//
//   * NaN handling.  ARMv8 FMIN/FMAX (NEON VMIN/VMAX.f) propagate NaN; FMINNM/
//     FMAXNM (VMINNM/VMAXNM) suppress it and return the numeric operand.  The
//     select returns the wrong operand whenever the NaN is on the "losing" side.
//   * Signed zeros.  FMIN(-0, +0) is -0 regardless of operand order, but the
//     select returns whichever operand the (false) comparison did not pick.
//
// The AArch64 vector FMIN/FMAX were additionally lifted with NO per-lane
// handling at all (a single whole-register compare-and-select), so any
// multi-lane vminq/vmaxq result above the low lane was wrong.
//
// The existing FP min/max tests all use ordinary, distinct, non-tie values, so
// none of this was observable — a classic weak-test blind spot.  Each probe
// below feeds a NaN, a signed zero, or distinct per-lane values so the broken
// and correct lowerings diverge.  All inputs are passed at runtime to keep the
// compiler from folding the operation away.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64MinMaxRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64MinMaxRT, Verify) { roundTripX64(GetParam()); }

class A64MinMaxRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64MinMaxRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32MinMaxRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32MinMaxRT, Verify) { roundTripARM32(GetParam()); }

// Float bit patterns passed in the low 32 bits of an integer argument.
#define F_NAN 0x7FC00000ULL  // quiet NaN
#define F_5   0x40A00000ULL  //  5.0f
#define F_3   0x40400000ULL  //  3.0f
#define F_P0  0x00000000ULL  // +0.0f
#define F_N0  0x80000000ULL  // -0.0f

// ARM32 VMINNM/VMAXNM + scalar fminf/fmaxf need an ARMv8 FP baseline.
#define A32V8 "MinMax", 1, \
  "-march=armv8-a -mfpu=neon-fp-armv8 -fno-math-errno", false, \
  "armv8-linux-gnueabihf", UC_CPU_ARM_MAX

// clang-format off

static const std::vector<RoundTripTC> kX64 = {
  // x86 has no FMINNM; __builtin_fminf lowers to a minss-based NaN-aware
  // sequence.  These are controls: the x86 path is unchanged, so they must keep
  // passing (and prove the harness handles NaN/-0 returns).
  {"x64_fminf_nan",
   "unsigned x64_fminf_nan(unsigned a, unsigned b){ float x,y; __builtin_memcpy(&x,&a,4);"
   " __builtin_memcpy(&y,&b,4); float r=__builtin_fminf(x,y); unsigned o;"
   " __builtin_memcpy(&o,&r,4); return o; }\n",
   {F_5, F_NAN}, "MinMax", 1, "-fno-math-errno"},
  {"x64_fmaxf_nan",
   "unsigned x64_fmaxf_nan(unsigned a, unsigned b){ float x,y; __builtin_memcpy(&x,&a,4);"
   " __builtin_memcpy(&y,&b,4); float r=__builtin_fmaxf(x,y); unsigned o;"
   " __builtin_memcpy(&o,&r,4); return o; }\n",
   {F_NAN, F_5}, "MinMax", 1, "-fno-math-errno"},
  // minss/maxss proper: (a<b)?a:b — x86 returns the second operand on NaN/equal.
  {"x64_minss_nan",
   "unsigned x64_minss_nan(unsigned a, unsigned b){ float x,y; __builtin_memcpy(&x,&a,4);"
   " __builtin_memcpy(&y,&b,4); float r=(x<y)?x:y; unsigned o;"
   " __builtin_memcpy(&o,&r,4); return o; }\n",
   {F_NAN, F_5}, "MinMax", 1, "-fno-math-errno"},
};

static const std::vector<RoundTripTC> kA64 = {
  // Scalar FMINNM/FMAXNM (from fminf/fmaxf) — NaN-suppressing.
  {"a64_fminnm_nan",
   "unsigned a64_fminnm_nan(unsigned a, unsigned b){ float x,y; __builtin_memcpy(&x,&a,4);"
   " __builtin_memcpy(&y,&b,4); float r=__builtin_fminf(x,y); unsigned o;"
   " __builtin_memcpy(&o,&r,4); return o; }\n",
   {F_5, F_NAN}, "MinMax", 1, "-fno-math-errno"},
  {"a64_fmaxnm_nan",
   "unsigned a64_fmaxnm_nan(unsigned a, unsigned b){ float x,y; __builtin_memcpy(&x,&a,4);"
   " __builtin_memcpy(&y,&b,4); float r=__builtin_fmaxf(x,y); unsigned o;"
   " __builtin_memcpy(&o,&r,4); return o; }\n",
   {F_NAN, F_5}, "MinMax", 1, "-fno-math-errno"},
  // Scalar FMINNM/FMAXNM signed zero: result is -0/+0 regardless of order.
  {"a64_fminnm_n0",
   "unsigned a64_fminnm_n0(unsigned a, unsigned b){ float x,y; __builtin_memcpy(&x,&a,4);"
   " __builtin_memcpy(&y,&b,4); float r=__builtin_fminf(x,y); unsigned o;"
   " __builtin_memcpy(&o,&r,4); return o; }\n",
   {F_N0, F_P0}, "MinMax", 1, "-fno-math-errno"},
  {"a64_fmaxnm_n0",
   "unsigned a64_fmaxnm_n0(unsigned a, unsigned b){ float x,y; __builtin_memcpy(&x,&a,4);"
   " __builtin_memcpy(&y,&b,4); float r=__builtin_fmaxf(x,y); unsigned o;"
   " __builtin_memcpy(&o,&r,4); return o; }\n",
   {F_N0, F_P0}, "MinMax", 1, "-fno-math-errno"},

  // Vector FMIN/FMAX (vminq_f32/vmaxq_f32) — distinct per-lane values expose the
  // missing per-lane handling (each result lane differs from all-A and all-B).
  {"a64_vminq",
   "#include <arm_neon.h>\n"
   "long a64_vminq(long a){ float va[4],vb[4];"
   " va[0]=(float)(a+1);va[1]=(float)(a+8);va[2]=(float)(a+3);va[3]=(float)(a+6);"
   " vb[0]=(float)(a+5);vb[1]=(float)(a+2);vb[2]=(float)(a+7);vb[3]=(float)(a+4);"
   " float32x4_t r=vminq_f32(vld1q_f32(va),vld1q_f32(vb)); int o[4]; vst1q_f32((float*)o,r);"
   " return (long)(unsigned)(o[0]+o[1]*7+o[2]*13+o[3]*17); }\n",
   {0}, "MinMax", 1, "-ffreestanding -fno-math-errno"},
  {"a64_vmaxq",
   "#include <arm_neon.h>\n"
   "long a64_vmaxq(long a){ float va[4],vb[4];"
   " va[0]=(float)(a+1);va[1]=(float)(a+8);va[2]=(float)(a+3);va[3]=(float)(a+6);"
   " vb[0]=(float)(a+5);vb[1]=(float)(a+2);vb[2]=(float)(a+7);vb[3]=(float)(a+4);"
   " float32x4_t r=vmaxq_f32(vld1q_f32(va),vld1q_f32(vb)); int o[4]; vst1q_f32((float*)o,r);"
   " return (long)(unsigned)(o[0]+o[1]*7+o[2]*13+o[3]*17); }\n",
   {0}, "MinMax", 1, "-ffreestanding -fno-math-errno"},
  // Vector FMIN with a runtime NaN lane — must propagate (lane0 result is NaN).
  {"a64_vminq_nan",
   "#include <arm_neon.h>\n"
   "long a64_vminq_nan(long a, long b){ float fn; unsigned bn=(unsigned)b; __builtin_memcpy(&fn,&bn,4);"
   " float va[4],vb[4]; va[0]=fn;va[1]=(float)(a+8);va[2]=(float)(a+3);va[3]=(float)(a+6);"
   " vb[0]=(float)(a+5);vb[1]=(float)(a+2);vb[2]=(float)(a+7);vb[3]=(float)(a+4);"
   " float32x4_t r=vminq_f32(vld1q_f32(va),vld1q_f32(vb)); int o[4]; vst1q_f32((float*)o,r);"
   " return (long)(unsigned)(o[0]^(o[1]*7)^(o[2]*13)^(o[3]*17)); }\n",
   {0, F_NAN}, "MinMax", 1, "-ffreestanding -fno-math-errno"},
  // Vector FMINNM with a runtime NaN lane — must suppress (lane0 = the number).
  {"a64_vminnmq_nan",
   "#include <arm_neon.h>\n"
   "long a64_vminnmq_nan(long a, long b){ float fn; unsigned bn=(unsigned)b; __builtin_memcpy(&fn,&bn,4);"
   " float va[4],vb[4]; va[0]=fn;va[1]=(float)(a+8);va[2]=(float)(a+3);va[3]=(float)(a+6);"
   " vb[0]=(float)(a+5);vb[1]=(float)(a+2);vb[2]=(float)(a+7);vb[3]=(float)(a+4);"
   " float32x4_t r=vminnmq_f32(vld1q_f32(va),vld1q_f32(vb)); int o[4]; vst1q_f32((float*)o,r);"
   " return (long)(unsigned)(o[0]^(o[1]*7)^(o[2]*13)^(o[3]*17)); }\n",
   {0, F_NAN}, "MinMax", 1, "-ffreestanding -fno-math-errno"},
  // Pairwise FMINP/FMAXP — result lanes pair adjacent elements (structure test).
  {"a64_vpminq",
   "#include <arm_neon.h>\n"
   "long a64_vpminq(long a){ float va[4],vb[4];"
   " va[0]=(float)(a+1);va[1]=(float)(a+8);va[2]=(float)(a+3);va[3]=(float)(a+6);"
   " vb[0]=(float)(a+5);vb[1]=(float)(a+2);vb[2]=(float)(a+7);vb[3]=(float)(a+4);"
   " float32x4_t r=vpminq_f32(vld1q_f32(va),vld1q_f32(vb)); int o[4]; vst1q_f32((float*)o,r);"
   " return (long)(unsigned)(o[0]+o[1]*7+o[2]*13+o[3]*17); }\n",
   {0}, "MinMax", 1, "-ffreestanding -fno-math-errno"},
  // Horizontal FMINV/FMAXV reduction across lanes to a scalar.
  {"a64_vminvq",
   "#include <arm_neon.h>\n"
   "long a64_vminvq(long a){ float va[4];"
   " va[0]=(float)(a+6);va[1]=(float)(a+2);va[2]=(float)(a+9);va[3]=(float)(a+4);"
   " float r=vminvq_f32(vld1q_f32(va)); int o; __builtin_memcpy(&o,&r,4);"
   " return (long)(unsigned)o; }\n",
   {0}, "MinMax", 1, "-ffreestanding -fno-math-errno"},
};

static const std::vector<RoundTripTC> kArm32 = {
  // Scalar VMINNM/VMAXNM (from fminf/fmaxf, ARMv8 FP) — NaN-suppressing.
  {"arm_fminnm_nan",
   "unsigned arm_fminnm_nan(unsigned a, unsigned b){ float x,y; __builtin_memcpy(&x,&a,4);"
   " __builtin_memcpy(&y,&b,4); float r=__builtin_fminf(x,y); unsigned o;"
   " __builtin_memcpy(&o,&r,4); return o; }\n",
   {F_5, F_NAN}, A32V8},
  {"arm_fmaxnm_n0",
   "unsigned arm_fmaxnm_n0(unsigned a, unsigned b){ float x,y; __builtin_memcpy(&x,&a,4);"
   " __builtin_memcpy(&y,&b,4); float r=__builtin_fmaxf(x,y); unsigned o;"
   " __builtin_memcpy(&o,&r,4); return o; }\n",
   {F_N0, F_P0}, A32V8},

  // Vector VMIN/VMAX (vminq_f32/vmaxq_f32) — NEON, available on cortex-a15.
  {"arm_vminq",
   "#include <arm_neon.h>\n"
   "long arm_vminq(long a){ float va[4],vb[4];"
   " va[0]=(float)(a+1);va[1]=(float)(a+8);va[2]=(float)(a+3);va[3]=(float)(a+6);"
   " vb[0]=(float)(a+5);vb[1]=(float)(a+2);vb[2]=(float)(a+7);vb[3]=(float)(a+4);"
   " float32x4_t r=vminq_f32(vld1q_f32(va),vld1q_f32(vb)); int o[4]; vst1q_f32((float*)o,r);"
   " return (long)(unsigned)(o[0]+o[1]*7+o[2]*13+o[3]*17); }\n",
   {0}, "MinMax", 1, "-ffreestanding -fno-math-errno"},
  {"arm_vminq_nan",
   "#include <arm_neon.h>\n"
   "long arm_vminq_nan(long a, long b){ float fn; unsigned bn=(unsigned)b; __builtin_memcpy(&fn,&bn,4);"
   " float va[4],vb[4]; va[0]=fn;va[1]=(float)(a+8);va[2]=(float)(a+3);va[3]=(float)(a+6);"
   " vb[0]=(float)(a+5);vb[1]=(float)(a+2);vb[2]=(float)(a+7);vb[3]=(float)(a+4);"
   " float32x4_t r=vminq_f32(vld1q_f32(va),vld1q_f32(vb)); int o[4]; vst1q_f32((float*)o,r);"
   " return (long)(unsigned)(o[0]^(o[1]*7)^(o[2]*13)^(o[3]*17)); }\n",
   {0, F_NAN}, "MinMax", 1, "-ffreestanding -fno-math-errno"},
  // Vector VMINNM per-lane (ARMv8 NEON) — distinct values verify the per-lane
  // minNum lowering and the f32 (not f64) lane width.  NaN-suppression for ARM32
  // vminnm is covered by arm_fminnm_nan (scalar) and a64_vminnmq_nan (vector);
  // injecting a NaN here via a GPR forces a `vmov.32 d[0]` partial-D write whose
  // whole-Q reconstruction is a separate, pre-existing lifter limitation (the
  // sibling S-lane write is dropped) tracked for a dedicated fix.
  {"arm_vminnmq",
   "#include <arm_neon.h>\n"
   "long arm_vminnmq(long a){ float va[4],vb[4];"
   " va[0]=(float)(a+1);va[1]=(float)(a+8);va[2]=(float)(a+3);va[3]=(float)(a+6);"
   " vb[0]=(float)(a+5);vb[1]=(float)(a+2);vb[2]=(float)(a+7);vb[3]=(float)(a+4);"
   " float32x4_t r=vminnmq_f32(vld1q_f32(va),vld1q_f32(vb)); int o[4]; vst1q_f32((float*)o,r);"
   " return (long)(unsigned)(o[0]+o[1]*7+o[2]*13+o[3]*17); }\n",
   {0},
   "MinMax", 1, "-march=armv8-a -mfpu=neon-fp-armv8 -ffreestanding -fno-math-errno",
   false, "armv8-linux-gnueabihf", UC_CPU_ARM_MAX},
  // Vector VMINNM with a runtime NaN lane (ARMv8 NEON) — minNum must suppress
  // the NaN.  Injecting the NaN from a GPR makes clang build the vector with
  // `vmov.32 d[0], r` (a 32-bit lane move, modeled as a whole-D CONCAT) and a
  // `vcvt` into the same D's high S lane; the whole-Q read must reconstruct from
  // that newer sibling S write rather than dropping it.
  {"arm_vminnmq_nan",
   "#include <arm_neon.h>\n"
   "long arm_vminnmq_nan(long a, long b){ float fn; unsigned bn=(unsigned)b; __builtin_memcpy(&fn,&bn,4);"
   " float va[4],vb[4]; va[0]=fn;va[1]=(float)(a+8);va[2]=(float)(a+3);va[3]=(float)(a+6);"
   " vb[0]=(float)(a+5);vb[1]=(float)(a+2);vb[2]=(float)(a+7);vb[3]=(float)(a+4);"
   " float32x4_t r=vminnmq_f32(vld1q_f32(va),vld1q_f32(vb)); int o[4]; vst1q_f32((float*)o,r);"
   " return (long)(unsigned)(o[0]^(o[1]*7)^(o[2]*13)^(o[3]*17)); }\n",
   {0, F_NAN},
   "MinMax", 1, "-march=armv8-a -mfpu=neon-fp-armv8 -ffreestanding -fno-math-errno",
   false, "armv8-linux-gnueabihf", UC_CPU_ARM_MAX},
  // Pairwise VPMIN.f32 (NEON, D-register) — structure test.
  {"arm_vpmin",
   "#include <arm_neon.h>\n"
   "long arm_vpmin(long a){ float va[2],vb[2];"
   " va[0]=(float)(a+1);va[1]=(float)(a+8); vb[0]=(float)(a+5);vb[1]=(float)(a+2);"
   " float32x2_t r=vpmin_f32(vld1_f32(va),vld1_f32(vb)); int o[2]; vst1_f32((float*)o,r);"
   " return (long)(unsigned)(o[0]+o[1]*7); }\n",
   {0}, "MinMax", 1, "-ffreestanding -fno-math-errno"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(MinMax, X64MinMaxRT, ::testing::ValuesIn(kX64),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(MinMax, A64MinMaxRT, ::testing::ValuesIn(kA64),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(MinMax, ARM32MinMaxRT, ::testing::ValuesIn(kArm32),
                         rtTCName);
