//===- AllPlatform_DotProductRTTests.cpp - SDOT/UDOT dot product -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for the SIMD byte dot-product accumulate instructions
// (AArch64 SDOT/UDOT, FEAT_DotProd).  Each 32-bit lane accumulates the sum of
// four byte products; these were lifted as a full-width INT_MULT+INT_ADD
// placeholder (whole register as one integer — completely wrong).  Now lifted
// per-lane.  The indexed form broadcasts one 4-byte group of the second source.
//
// Requires +dotprod; select Unicorn's MAX AArch64 CPU explicitly.  All values
// are bounded so no result overflows the 32-bit accumulator unexpectedly.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64DotProductRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64DotProductRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32DotProductRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32DotProductRT, Verify) { roundTripARM32(GetParam()); }

#define A64DOT                                                                 \
  "DotProduct", 1, "-march=armv8.2-a+dotprod", false, "", UC_CPU_ARM64_MAX

// AArch32 dot product needs an ARMv8.2 target + the MAX CPU for emulation
// (the default fixture pins cortex-a15, which is ARMv7 without FEAT_DotProd).
#define A32DOT "DotProduct", 1, \
  "-march=armv8.2-a+dotprod -mfpu=neon-fp-armv8", false, \
  "armv8.2a-linux-gnueabihf", UC_CPU_ARM_MAX

// clang-format off

static const std::vector<RoundTripTC> kA64Dot = {
  // UDOT q-reg: 4x u32 accumulators, each from 4 unsigned byte products.
  {"a64_udot16",
   "#include <arm_neon.h>\n"
   "unsigned long a64_udot16(unsigned long a){\n"
   "  unsigned char xa[16], xb[16];\n"
   "  for(int i=0;i<16;i++){ xa[i]=(unsigned char)(a*(i+1)); xb[i]=(unsigned char)(a*3+i*5); }\n"
   "  uint8x16_t va=vld1q_u8(xa), vb=vld1q_u8(xb);\n"
   "  uint32x4_t acc=vdupq_n_u32((unsigned)(a&0xFFF));\n"
   "  acc=vdotq_u32(acc,va,vb);\n"
   "  return (unsigned long)(vgetq_lane_u32(acc,0)+vgetq_lane_u32(acc,1)*7u\n"
   "        +vgetq_lane_u32(acc,2)*13u+vgetq_lane_u32(acc,3)*17u);\n"
   "}\n",
   {0x37ULL}, A64DOT},

  // SDOT q-reg: signed bytes (include negatives) -> 4x s32 accumulators.
  {"a64_sdot16",
   "#include <arm_neon.h>\n"
   "long a64_sdot16(long a){\n"
   "  signed char xa[16], xb[16];\n"
   "  for(int i=0;i<16;i++){ xa[i]=(signed char)(a*(i+1)-50); xb[i]=(signed char)(a*3-i*5); }\n"
   "  int8x16_t va=vld1q_s8(xa), vb=vld1q_s8(xb);\n"
   "  int32x4_t acc=vdupq_n_s32((int)(a&0x7F)-64);\n"
   "  acc=vdotq_s32(acc,va,vb);\n"
   "  return (long)(vgetq_lane_s32(acc,0)+vgetq_lane_s32(acc,1)*7\n"
   "        +vgetq_lane_s32(acc,2)*13+vgetq_lane_s32(acc,3)*17);\n"
   "}\n",
   {0x29ULL}, A64DOT},

  // UDOT d-reg: 2x u32 accumulators (8-byte sources).
  {"a64_udot8",
   "#include <arm_neon.h>\n"
   "unsigned long a64_udot8(unsigned long a){\n"
   "  unsigned char xa[8], xb[8];\n"
   "  for(int i=0;i<8;i++){ xa[i]=(unsigned char)(a*(i+2)); xb[i]=(unsigned char)(a+i*9); }\n"
   "  uint8x8_t va=vld1_u8(xa), vb=vld1_u8(xb);\n"
   "  uint32x2_t acc=vdup_n_u32((unsigned)(a&0xFF));\n"
   "  acc=vdot_u32(acc,va,vb);\n"
   "  return (unsigned long)(vget_lane_u32(acc,0)+vget_lane_u32(acc,1)*7u);\n"
   "}\n",
   {0x4BULL}, A64DOT},

  // UDOT indexed: broadcast one 4-byte group of the second source.
  {"a64_udot_idx",
   "#include <arm_neon.h>\n"
   "unsigned long a64_udot_idx(unsigned long a){\n"
   "  unsigned char xa[16], xb[16];\n"
   "  for(int i=0;i<16;i++){ xa[i]=(unsigned char)(a*(i+1)+i); xb[i]=(unsigned char)(a*2+i*3); }\n"
   "  uint8x16_t va=vld1q_u8(xa), vb=vld1q_u8(xb);\n"
   "  uint32x4_t acc=vdupq_n_u32((unsigned)(a&0x3FF));\n"
   "  acc=vdotq_laneq_u32(acc,va,vb,2);\n"
   "  return (unsigned long)(vgetq_lane_u32(acc,0)+vgetq_lane_u32(acc,1)*7u\n"
   "        +vgetq_lane_u32(acc,2)*13u+vgetq_lane_u32(acc,3)*17u);\n"
   "}\n",
   {0x55ULL}, A64DOT},
  // NOTE: i8mm (USDOT/SUDOT mixed-sign dot, SMMLA/UMMLA/USMMLA int8 matmul) is
  // ARMv8.6 — this Unicorn build raises UC_ERR_EXCEPTION (undefined insn) for it,
  // so those instructions can't be roundtrip-verified.  Unicorn MAX covers up to
  // ~v8.2 dotprod, not v8.6 i8mm.  See the Unicorn unsupported-instructions doc.
};

static const std::vector<RoundTripTC> kArm32Dot = {
  // VUDOT d-reg: 2x u32 accumulators from unsigned byte products.
  {"arm_udot8",
   "#include <arm_neon.h>\n"
   "unsigned arm_udot8(unsigned a){\n"
   "  unsigned char xa[8],xb[8];\n"
   "  for(int i=0;i<8;i++){ xa[i]=(unsigned char)(a*(i+1)); xb[i]=(unsigned char)(a*3+i*5); }\n"
   "  uint8x8_t va=vld1_u8(xa),vb=vld1_u8(xb);\n"
   "  uint32x2_t acc=vdup_n_u32((unsigned)(a&0xFFF));\n"
   "  acc=vdot_u32(acc,va,vb);\n"
   "  return vget_lane_u32(acc,0)+vget_lane_u32(acc,1)*7u;\n"
   "}\n",
   {0x37ULL}, A32DOT},

  // VSDOT q-reg: signed bytes (include negatives) -> 4x s32 accumulators.
  {"arm_sdot16",
   "#include <arm_neon.h>\n"
   "int arm_sdot16(int a){\n"
   "  signed char xa[16],xb[16];\n"
   "  for(int i=0;i<16;i++){ xa[i]=(signed char)(a*(i+1)-50); xb[i]=(signed char)(a*3-i*5); }\n"
   "  int8x16_t va=vld1q_s8(xa),vb=vld1q_s8(xb);\n"
   "  int32x4_t acc=vdupq_n_s32((int)(a&0x7F)-64);\n"
   "  acc=vdotq_s32(acc,va,vb);\n"
   "  return vgetq_lane_s32(acc,0)+vgetq_lane_s32(acc,1)*7\n"
   "        +vgetq_lane_s32(acc,2)*13+vgetq_lane_s32(acc,3)*17;\n"
   "}\n",
   {0x29ULL}, A32DOT},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(DotProduct, A64DotProductRT,
                         ::testing::ValuesIn(kA64Dot), rtTCName);
INSTANTIATE_TEST_SUITE_P(DotProduct, ARM32DotProductRT,
                         ::testing::ValuesIn(kArm32Dot), rtTCName);
