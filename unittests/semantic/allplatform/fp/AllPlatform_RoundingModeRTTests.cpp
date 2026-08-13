//===- AllPlatform_RoundingModeRTTests.cpp - round-to-nearest-even -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes that exercise the IEEE default rounding mode (round to
// nearest, ties to even) on the FP→int convert and round-to-integral
// instructions.  These had been lifted with round-half-away-from-zero
// (FLOAT_ROUND / llvm.round), which only matches on non-tie inputs; the
// existing FP tests all use 7.1-style values, so the tie behaviour went
// untested.  Every probe below feeds an exact .5 tie at runtime so even-vs-away
// rounding produces different results:
//
//   value | round-to-even | round-half-away
//   ------+---------------+----------------
//    0.5  |      0        |      1
//    2.5  |      2        |      3
//    4.5  |      4        |      5
//   -2.5  |     -2        |     -3
//
// Covered: x86 CVTSD2SI/CVTSS2SI/CVTPS2DQ/ROUNDSD/ROUNDSS/ROUNDPS,
// AArch64 FRINTX/FRINTI/FCVTNS (scalar + vector), ARM32 VRINTR/VRINTX/VRINTN
// (the AArch32 forms need an ARMv8 FP baseline, which the recompiler now enables
// for the rounding intrinsics so they select vrint* instead of a library call).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64RoundModeRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64RoundModeRT, Verify) { roundTripX64(GetParam()); }

class A64RoundModeRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64RoundModeRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32RoundModeRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32RoundModeRT, Verify) { roundTripARM32(GetParam()); }

// Double bit patterns for exact .5 ties.
#define D_0_5   0x3FE0000000000000ULL   //  0.5
#define D_2_5   0x4004000000000000ULL   //  2.5
#define D_4_5   0x4012000000000000ULL   //  4.5
#define D_NEG25 0xC004000000000000ULL   // -2.5
// Float bit patterns (passed in the low 32 bits of the argument).
#define F_2_5   0x40200000ULL           //  2.5f
#define F_0_5   0x3F000000ULL           //  0.5f
#define F_NEG25 0xC0200000ULL           // -2.5f

// AArch32 VRINT* need an ARMv8 FP baseline + the MAX CPU for emulation.
#define A32RND "Rounding", 1, \
  "-march=armv8-a -mfpu=neon-fp-armv8 -fno-math-errno", false, \
  "armv8-linux-gnueabihf", UC_CPU_ARM_MAX

// clang-format off

static const std::vector<RoundTripTC> kX64Round = {
  // CVTSD2SI — double -> i64, MXCSR rounding (nearest, ties even).
  {"x64_lrint_25",
   "long x64_lrint_25(long a){ double d; __builtin_memcpy(&d,&a,8); return __builtin_lrint(d); }\n",
   {D_2_5}, "Rounding", 1, "-fno-math-errno"},
  {"x64_lrint_05",
   "long x64_lrint_05(long a){ double d; __builtin_memcpy(&d,&a,8); return __builtin_lrint(d); }\n",
   {D_0_5}, "Rounding", 1, "-fno-math-errno"},
  {"x64_lrint_neg25",
   "long x64_lrint_neg25(long a){ double d; __builtin_memcpy(&d,&a,8); return __builtin_lrint(d); }\n",
   {D_NEG25}, "Rounding", 1, "-fno-math-errno"},

  // CVTSS2SI — float -> i64, MXCSR rounding.
  {"x64_lrintf_25",
   "long x64_lrintf_25(long a){ float f; int i=(int)a; __builtin_memcpy(&f,&i,4); return __builtin_lrintf(f); }\n",
   {F_2_5}, "Rounding", 1, "-fno-math-errno"},
  {"x64_lrintf_45",
   "long x64_lrintf_45(long a){ float f; int i=(int)a; __builtin_memcpy(&f,&i,4); return __builtin_lrintf(f); }\n",
   {0x40900000ULL /*4.5f*/}, "Rounding", 1, "-fno-math-errno"},

  // ROUNDSD imm0 (use-MXCSR) — round double to integral double, ties even.
  {"x64_nearbyint_25",
   "long x64_nearbyint_25(long a){ double d; __builtin_memcpy(&d,&a,8); double r=__builtin_nearbyint(d);"
   " long o; __builtin_memcpy(&o,&r,8); return o; }\n",
   {D_2_5}, "Rounding", 1, "-msse4.1 -fno-math-errno"},
  {"x64_nearbyint_45",
   "long x64_nearbyint_45(long a){ double d; __builtin_memcpy(&d,&a,8); double r=__builtin_nearbyint(d);"
   " long o; __builtin_memcpy(&o,&r,8); return o; }\n",
   {D_4_5}, "Rounding", 1, "-msse4.1 -fno-math-errno"},

  // ROUNDSS imm0 — round float to integral float, ties even.
  {"x64_nearbyintf_25",
   "long x64_nearbyintf_25(long a){ float f; int i=(int)a; __builtin_memcpy(&f,&i,4); float r=__builtin_nearbyintf(f);"
   " int o; __builtin_memcpy(&o,&r,4); return (long)(unsigned)o; }\n",
   {F_2_5}, "Rounding", 1, "-msse4.1 -fno-math-errno"},

  // CVTPS2DQ — packed float -> i32, MXCSR rounding (ties even), per lane.
  {"x64_cvtps2dq",
   "#include <immintrin.h>\n"
   "long x64_cvtps2dq(long a){\n"
   "  float xf[4]; for(int i=0;i<4;i++) xf[i]=(float)((int)a+i)+0.5f;\n"
   "  __m128 v=_mm_loadu_ps(xf); __m128i r=_mm_cvtps_epi32(v);\n"
   "  int o[4]; _mm_storeu_si128((__m128i*)o,r);\n"
   "  return (long)(unsigned)(o[0]+o[1]*7+o[2]*13+o[3]*17); }\n",
   {2}, "Rounding", 2, "-msse2 -ffreestanding"},

  // ROUNDPS imm0 — packed float round to integral, ties even, per lane.
  {"x64_roundps",
   "#include <immintrin.h>\n"
   "long x64_roundps(long a){\n"
   "  float xf[4]; for(int i=0;i<4;i++) xf[i]=(float)((int)a+i)+0.5f;\n"
   "  __m128 v=_mm_loadu_ps(xf);\n"
   "  __m128 r=_mm_round_ps(v, _MM_FROUND_TO_NEAREST_INT|_MM_FROUND_NO_EXC);\n"
   "  int o[4]; _mm_storeu_ps((float*)o,r);\n"
   "  return (long)(unsigned)(o[0]+o[1]*7+o[2]*13+o[3]*17); }\n",
   {2}, "Rounding", 2, "-msse4.1 -ffreestanding"},

  // ROUNDSS imm3 — truncate toward zero.  The value (5e9f) is already integral
  // but exceeds 2^31, so the old float->int->float lowering overflowed i32.
  {"x64_truncf_large",
   "unsigned x64_truncf_large(long a){ float f=(float)(a*100000000); float r=__builtin_truncf(f);"
   " unsigned o; __builtin_memcpy(&o,&r,4); return o; }\n",
   {50}, "Rounding", 1, "-msse4.1 -fno-math-errno"},

  // ROUNDSD imm3 — truncate toward zero past 2^63 (old path overflowed i64).
  {"x64_trunc_large",
   "unsigned long x64_trunc_large(long a){ double d=(double)a*(double)a; double r=__builtin_trunc(d);"
   " unsigned long o; __builtin_memcpy(&o,&r,8); return o; }\n",
   {4000000000ULL}, "Rounding", 1, "-msse4.1 -fno-math-errno"},

  // ROUNDPS imm3 — packed truncate toward zero past 2^31, per lane.
  {"x64_roundps_trunc",
   "#include <immintrin.h>\n"
   "long x64_roundps_trunc(long a){\n"
   "  float xf[4]; for(int i=0;i<4;i++) xf[i]=(float)((a+i)*100000000);\n"
   "  __m128 v=_mm_loadu_ps(xf);\n"
   "  __m128 r=_mm_round_ps(v, _MM_FROUND_TO_ZERO|_MM_FROUND_NO_EXC);\n"
   "  int o[4]; _mm_storeu_ps((float*)o,r);\n"
   "  return (long)(unsigned)(o[0]^o[1]^o[2]^o[3]); }\n",
   {50}, "Rounding", 2, "-msse4.1 -ffreestanding"},
};

static const std::vector<RoundTripTC> kA64Round = {
  // FRINTX + FCVTZS — lrint: double -> i64, round to even.
  {"a64_lrint_25",
   "long a64_lrint_25(long a){ double d; __builtin_memcpy(&d,&a,8); return __builtin_lrint(d); }\n",
   {D_2_5}, "Rounding", 1, "-fno-math-errno"},
  {"a64_lrint_05",
   "long a64_lrint_05(long a){ double d; __builtin_memcpy(&d,&a,8); return __builtin_lrint(d); }\n",
   {D_0_5}, "Rounding", 1, "-fno-math-errno"},
  {"a64_lrint_neg25",
   "long a64_lrint_neg25(long a){ double d; __builtin_memcpy(&d,&a,8); return __builtin_lrint(d); }\n",
   {D_NEG25}, "Rounding", 1, "-fno-math-errno"},

  // FRINTX + FCVTZS — lrintf: float -> i64, round to even.
  {"a64_lrintf_45",
   "long a64_lrintf_45(long a){ float f; int i=(int)a; __builtin_memcpy(&f,&i,4); return __builtin_lrintf(f); }\n",
   {0x40900000ULL /*4.5f*/}, "Rounding", 1, "-fno-math-errno"},

  // FRINTX — rint: round double to integral double, ties even.
  {"a64_rint_25",
   "long a64_rint_25(long a){ double d; __builtin_memcpy(&d,&a,8); double r=__builtin_rint(d);"
   " long o; __builtin_memcpy(&o,&r,8); return o; }\n",
   {D_2_5}, "Rounding", 1, "-fno-math-errno"},

  // FRINTI — nearbyint: round double to integral double using FPCR (ties even).
  {"a64_nearbyint_neg25",
   "long a64_nearbyint_neg25(long a){ double d; __builtin_memcpy(&d,&a,8); double r=__builtin_nearbyint(d);"
   " long o; __builtin_memcpy(&o,&r,8); return o; }\n",
   {D_NEG25}, "Rounding", 1, "-fno-math-errno"},

  // FCVTNS v.4s — vector float -> i32, round to nearest even, per lane.
  {"a64_fcvtns4",
   "#include <arm_neon.h>\n"
   "long a64_fcvtns4(long a){\n"
   "  float xf[4]; for(int i=0;i<4;i++) xf[i]=(float)((int)a+i)+0.5f;\n"
   "  float32x4_t v=vld1q_f32(xf); int32x4_t r=vcvtnq_s32_f32(v);\n"
   "  return (long)(vgetq_lane_s32(r,0)+vgetq_lane_s32(r,1)*7\n"
   "        +vgetq_lane_s32(r,2)*13+vgetq_lane_s32(r,3)*17); }\n",
   {2}, "Rounding", 2, "-fno-math-errno"},

  // FRINTN v.4s — vector round to integral float, ties even, per lane.
  {"a64_frintn4",
   "#include <arm_neon.h>\n"
   "long a64_frintn4(long a){\n"
   "  float xf[4]; for(int i=0;i<4;i++) xf[i]=(float)((int)a+i)+0.5f;\n"
   "  float32x4_t v=vld1q_f32(xf); float32x4_t r=vrndnq_f32(v);\n"
   "  int o[4]; vst1q_f32((float*)o,r);\n"
   "  return (long)(unsigned)(o[0]+o[1]*7+o[2]*13+o[3]*17); }\n",
   {2}, "Rounding", 2, "-fno-math-errno"},
};

static const std::vector<RoundTripTC> kArm32Round = {
  // VRINTX — round float to integral, ties even (recompiler must select vrint*,
  // not a roundeven/round library call).
  {"arm_rintf_25",
   "unsigned arm_rintf_25(unsigned a){ float f; __builtin_memcpy(&f,&a,4); float r=__builtin_rintf(f);"
   " unsigned o; __builtin_memcpy(&o,&r,4); return o; }\n",
   {F_2_5}, A32RND},
  {"arm_rintf_neg25",
   "unsigned arm_rintf_neg25(unsigned a){ float f; __builtin_memcpy(&f,&a,4); float r=__builtin_rintf(f);"
   " unsigned o; __builtin_memcpy(&o,&r,4); return o; }\n",
   {F_NEG25}, A32RND},

  // VRINTR — nearbyint: round float to integral using FPSCR (ties even).
  {"arm_nearbyintf_25",
   "unsigned arm_nearbyintf_25(unsigned a){ float f; __builtin_memcpy(&f,&a,4); float r=__builtin_nearbyintf(f);"
   " unsigned o; __builtin_memcpy(&o,&r,4); return o; }\n",
   {F_2_5}, A32RND},

  // VRINTA — round float to integral, ties away (stays FLOAT_ROUND; also checks
  // the recompiler enables the ARMv8 FP baseline for llvm.round).
  {"arm_roundf_25",
   "unsigned arm_roundf_25(unsigned a){ float f; __builtin_memcpy(&f,&a,4); float r=__builtin_roundf(f);"
   " unsigned o; __builtin_memcpy(&o,&r,4); return o; }\n",
   {F_2_5}, A32RND},

  // VRINTM / VRINTP / VRINTZ — floor / ceil / trunc to integral float.
  {"arm_floorf_25",
   "unsigned arm_floorf_25(unsigned a){ float f; __builtin_memcpy(&f,&a,4); float r=__builtin_floorf(f);"
   " unsigned o; __builtin_memcpy(&o,&r,4); return o; }\n",
   {F_2_5}, A32RND},
  {"arm_ceilf_25",
   "unsigned arm_ceilf_25(unsigned a){ float f; __builtin_memcpy(&f,&a,4); float r=__builtin_ceilf(f);"
   " unsigned o; __builtin_memcpy(&o,&r,4); return o; }\n",
   {F_2_5}, A32RND},
  {"arm_truncf_neg25",
   "unsigned arm_truncf_neg25(unsigned a){ float f; __builtin_memcpy(&f,&a,4); float r=__builtin_truncf(f);"
   " unsigned o; __builtin_memcpy(&o,&r,4); return o; }\n",
   {F_NEG25}, A32RND},

  // VCVT.U32.F32 / .U32.F64 — unsigned float->int must truncate as unsigned;
  // 3e9 is in [2^31, 2^32) so a signed FPToSI would saturate to INT32_MAX.
  // These use the default cortex-a15 target (vcvt.u32 exists on ARMv7).
  {"arm_f2u_big",
   "unsigned arm_f2u_big(unsigned a){ float f; __builtin_memcpy(&f,&a,4); return (unsigned)f; }\n",
   {0x4F32D05EULL /*3e9f*/}, "Rounding", 1, "-fno-math-errno"},
  {"arm_d2u_big",
   "unsigned arm_d2u_big(unsigned a){ double d=(double)a*1500000000.0; return (unsigned)d; }\n",
   {2}, "Rounding", 1, "-fno-math-errno"},

  // VMOVDRR (vmov dN, rLo, rHi) — assemble a double from a GPR pair (soft-float
  // ABI).  The generic VMOV fallthrough copied only the high source GPR into the
  // 8-byte D reg, dropping the low half; the (u64)hi<<32|lo bitcast forces it.
  {"arm_vmovdrr",
   "unsigned arm_vmovdrr(unsigned lo, unsigned hi){\n"
   "  unsigned long long bits=((unsigned long long)hi<<32)|lo; double d;\n"
   "  __builtin_memcpy(&d,&bits,8); return (unsigned)d; }\n",
   {0xC0000000ULL, 0x41E65A0BULL /*3e9*/}, "Rounding", 1, "-fno-math-errno"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(Rounding, X64RoundModeRT,
                         ::testing::ValuesIn(kX64Round), rtTCName);
INSTANTIATE_TEST_SUITE_P(Rounding, A64RoundModeRT,
                         ::testing::ValuesIn(kA64Round), rtTCName);
INSTANTIATE_TEST_SUITE_P(Rounding, ARM32RoundModeRT,
                         ::testing::ValuesIn(kArm32Round), rtTCName);
