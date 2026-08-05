//===- AllPlatform_VectorReduceRTTests.cpp - NEON reductions ----*- C++ -*-===//
//
// Roundtrip probes for horizontal vector reductions, a historically bug-prone
// area (whole-register placeholders, missing sign extension on widening
// reductions, wrong lane ordering).  Each probe feeds signed/boundary/distinct
// lane values so a broken lowering diverges from the hardware result:
//
//   * AArch64 ADDV / SADDLV / UADDLV / SMAXV / UMAXV / SMINV / UMINV — the
//     widening reductions (ADDLV) must sign/zero-extend each lane before the
//     sum, and the signed min/max reductions must order negatives correctly.
//   * ARM32 pairwise reductions (VPADD / VPADDL / VPMAX / VPMIN) chained into a
//     full reduction.
//   * x86 reductions are clang-autovectorized SSE controls.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64ReduceRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64ReduceRT, Verify) { roundTripX64(GetParam()); }

class A64ReduceRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64ReduceRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32ReduceRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32ReduceRT, Verify) { roundTripARM32(GetParam()); }

// arm_neon.h probes need -ffreestanding so the intrinsic header does not pull in
// stdlib.h under the -nostdlib cross build.
#define NEON_FLAGS "-ffreestanding -fno-math-errno"

// clang-format off

static const std::vector<RoundTripTC> kX64 = {
  // Autovectorized SSE reductions (controls + signed-correctness probes).
  {"x64_sum_s32",
   "int x64_sum_s32(long a){ int v[8]; for(int i=0;i<8;i++) v[i]=(int)(a*(i+1))-(i*7);"
   " int s=0; for(int i=0;i<8;i++) s+=v[i]; return s; }\n",
   {3}, "Reduce", 2, ""},
  {"x64_sum_s16",
   "long x64_sum_s16(long a){ short v[16]; for(int i=0;i<16;i++) v[i]=(short)(a*(i+1)-i*9);"
   " int s=0; for(int i=0;i<16;i++) s+=v[i]; return s; }\n",
   {5}, "Reduce", 2, ""},
  {"x64_max_s32",
   "int x64_max_s32(long a){ int v[8]; for(int i=0;i<8;i++) v[i]=(int)(a-i*i*13);"
   " int m=v[0]; for(int i=1;i<8;i++) if(v[i]>m)m=v[i]; return m; }\n",
   {4}, "Reduce", 2, ""},
  {"x64_min_s32",
   "int x64_min_s32(long a){ int v[8]; for(int i=0;i<8;i++) v[i]=(int)(a-i*i*13);"
   " int m=v[0]; for(int i=1;i<8;i++) if(v[i]<m)m=v[i]; return m; }\n",
   {4}, "Reduce", 2, ""},
  {"x64_max_u8",
   "long x64_max_u8(long a){ unsigned char v[16]; for(int i=0;i<16;i++) v[i]=(unsigned char)(a*i+7*i);"
   " unsigned m=0; for(int i=0;i<16;i++) if(v[i]>m)m=v[i]; return (long)m; }\n",
   {9}, "Reduce", 2, ""},
  {"x64_sad",
   "long x64_sad(long a){ unsigned char v[16],w[16]; for(int i=0;i<16;i++){v[i]=(unsigned char)(a*i);w[i]=(unsigned char)(a*i-3*i);}"
   " unsigned s=0; for(int i=0;i<16;i++){int d=(int)v[i]-(int)w[i]; s+=(d<0?-d:d);} return (long)s; }\n",
   {11}, "Reduce", 2, ""},
};

static const std::vector<RoundTripTC> kA64 = {
  // Non-widening sum (ADDV) — 4x i32 with negatives.
  {"a64_vaddvq_s32",
   "#include <arm_neon.h>\n"
   "long a64_vaddvq_s32(long a){ int v[4]; v[0]=(int)(a-100);v[1]=(int)(-a*3);v[2]=(int)(a*5-7);v[3]=(int)(-a-9);"
   " return (long)vaddvq_s32(vld1q_s32(v)); }\n",
   {40}, "Reduce", 1, NEON_FLAGS},
  // Widening sum SADDLV — 16x i8 with negatives must sign-extend each lane.
  {"a64_vaddlvq_s8",
   "#include <arm_neon.h>\n"
   "long a64_vaddlvq_s8(long a){ signed char v[16]; for(int i=0;i<16;i++) v[i]=(signed char)(a*(i+1)-i*23);"
   " return (long)vaddlvq_s8(vld1q_s8(v)); }\n",
   {7}, "Reduce", 1, NEON_FLAGS},
  // Widening sum UADDLV — 16x u8 (no wrap, zero-extend).
  {"a64_vaddlvq_u8",
   "#include <arm_neon.h>\n"
   "long a64_vaddlvq_u8(long a){ unsigned char v[16]; for(int i=0;i<16;i++) v[i]=(unsigned char)(a*(i+3)+i*17);"
   " return (long)vaddlvq_u8(vld1q_u8(v)); }\n",
   {9}, "Reduce", 1, NEON_FLAGS},
  // Widening sum SADDLV — 8x i16 -> i32.
  {"a64_vaddlvq_s16",
   "#include <arm_neon.h>\n"
   "long a64_vaddlvq_s16(long a){ short v[8]; for(int i=0;i<8;i++) v[i]=(short)(a*(i+1)-i*333);"
   " return (long)vaddlvq_s16(vld1q_s16(v)); }\n",
   {50}, "Reduce", 1, NEON_FLAGS},
  // Signed max/min reductions (SMAXV/SMINV) — negatives must order correctly.
  {"a64_vmaxvq_s32",
   "#include <arm_neon.h>\n"
   "long a64_vmaxvq_s32(long a){ int v[4]; v[0]=(int)(-a*4);v[1]=(int)(a-9);v[2]=(int)(-a-1);v[3]=(int)(a*2-5);"
   " return (long)vmaxvq_s32(vld1q_s32(v)); }\n",
   {15}, "Reduce", 1, NEON_FLAGS},
  {"a64_vminvq_s8",
   "#include <arm_neon.h>\n"
   "long a64_vminvq_s8(long a){ signed char v[16]; for(int i=0;i<16;i++) v[i]=(signed char)(a*(i+1)-i*29);"
   " return (long)vminvq_s8(vld1q_s8(v)); }\n",
   {6}, "Reduce", 1, NEON_FLAGS},
  {"a64_vmaxvq_u16",
   "#include <arm_neon.h>\n"
   "long a64_vmaxvq_u16(long a){ unsigned short v[8]; for(int i=0;i<8;i++) v[i]=(unsigned short)(a*(i+2)+i*101);"
   " return (long)vmaxvq_u16(vld1q_u16(v)); }\n",
   {30}, "Reduce", 1, NEON_FLAGS},
  // Autovectorized signed reductions (no intrinsic) — cross-check.
  {"a64_sum_s16",
   "long a64_sum_s16(long a){ short v[16]; for(int i=0;i<16;i++) v[i]=(short)(a*(i+1)-i*9);"
   " int s=0; for(int i=0;i<16;i++) s+=v[i]; return s; }\n",
   {5}, "Reduce", 2, ""},
};

static const std::vector<RoundTripTC> kArm32 = {
  // Pairwise add long (VPADDL) reduction over signed bytes.
  {"arm_vpaddlq_s8",
   "#include <arm_neon.h>\n"
   "long arm_vpaddlq_s8(long a){ signed char v[16]; for(int i=0;i<16;i++) v[i]=(signed char)(a*(i+1)-i*23);"
   " int16x8_t w=vpaddlq_s8(vld1q_s8(v)); int32x4_t x=vpaddlq_s16(w); int64x2_t y=vpaddlq_s32(x);"
   " return (long)(vgetq_lane_s64(y,0)+vgetq_lane_s64(y,1)); }\n",
   {7}, "Reduce", 1, NEON_FLAGS},
  // Pairwise add (VPADD) folding 4 i32 -> 1.
  {"arm_vpadd_s32",
   "#include <arm_neon.h>\n"
   "long arm_vpadd_s32(long a){ int v[4]; v[0]=(int)(a-100);v[1]=(int)(-a*3);v[2]=(int)(a*5-7);v[3]=(int)(-a-9);"
   " int32x4_t q=vld1q_s32(v); int32x2_t lo=vget_low_s32(q),hi=vget_high_s32(q);"
   " int32x2_t s=vpadd_s32(lo,hi); s=vpadd_s32(s,s); return (long)vget_lane_s32(s,0); }\n",
   {40}, "Reduce", 1, NEON_FLAGS},
  // Pairwise max/min over signed shorts.
  {"arm_vpmax_s16",
   "#include <arm_neon.h>\n"
   "long arm_vpmax_s16(long a){ short v[4]; v[0]=(short)(-a*4);v[1]=(short)(a-9);v[2]=(short)(-a-1);v[3]=(short)(a*2-5);"
   " int16x4_t d=vld1_s16(v); d=vpmax_s16(d,d); d=vpmax_s16(d,d); return (long)vget_lane_s16(d,0); }\n",
   {15}, "Reduce", 1, NEON_FLAGS},
  // Autovectorized signed reductions (cross-check the lifter's NEON path).
  {"arm_sum_s32",
   "int arm_sum_s32(long a){ int v[8]; for(int i=0;i<8;i++) v[i]=(int)(a*(i+1))-(i*7);"
   " int s=0; for(int i=0;i<8;i++) s+=v[i]; return s; }\n",
   {3}, "Reduce", 2, ""},
  {"arm_max_s16",
   "long arm_max_s16(long a){ short v[16]; for(int i=0;i<16;i++) v[i]=(short)(a-i*i*7);"
   " int m=v[0]; for(int i=1;i<16;i++) if(v[i]>m)m=v[i]; return m; }\n",
   {4}, "Reduce", 2, ""},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(Reduce, X64ReduceRT, ::testing::ValuesIn(kX64),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(Reduce, A64ReduceRT, ::testing::ValuesIn(kA64),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(Reduce, ARM32ReduceRT, ::testing::ValuesIn(kArm32),
                         rtTCName);
