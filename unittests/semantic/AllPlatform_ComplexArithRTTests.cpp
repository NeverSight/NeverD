//===- AllPlatform_ComplexArithRTTests.cpp - complex SIMD ------*- C++ -*-===//
//
// Roundtrip probes for the ARMv8.3 complex-number SIMD instructions, which the
// lifter implemented as wrong placeholders:
//
//   * AArch64 FCADD / ARM32 VCADD — rotated complex add.  FCADD #90 computes
//     {re = Vn.re - Vm.im, im = Vn.im + Vm.re}; #270 swaps the signs.  The
//     lifter emitted a plain whole-register FLOAT_ADD (AArch64) / INT_SUB
//     (ARM32) — neither the rotation nor per-lane.
//   * AArch64 FCMLA / ARM32 VCMLA — rotated complex multiply-accumulate.  The
//     lifter emitted a whole-register INT_MULT+INT_ADD (integer ops on FP data,
//     no rotation, no per-lane).
//
// Distinct per-lane real/imag values make the broken and correct lowerings
// diverge.  x86 has no such instruction; the x64 control uses scalar complex
// arithmetic and must keep passing.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64ComplexRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64ComplexRT, Verify) { roundTripX64(GetParam()); }

class A64ComplexRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64ComplexRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32ComplexRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32ComplexRT, Verify) { roundTripARM32(GetParam()); }

// FCADD/FCMLA need an ARMv8.3 (FEAT_FCMA) baseline + the MAX Unicorn CPU.
#define A64V83 "Complex", 1, \
  "-march=armv8.3-a -ffreestanding -fno-math-errno", false, \
  "aarch64-linux-gnu", UC_CPU_ARM64_MAX
#define A32V83 "Complex", 1, \
  "-march=armv8.3-a -mfpu=neon-fp-armv8 -ffreestanding -fno-math-errno", false, \
  "armv8.3a-linux-gnueabihf", UC_CPU_ARM_MAX

// clang-format off

static const std::vector<RoundTripTC> kX64 = {
  // Control: scalar complex multiply (no FCMLA on x86) — must stay correct.
  {"x64_cmul",
   "long x64_cmul(long a){ float ar=(float)(a+1),ai=(float)(a+2),br=(float)(a+3),bi=(float)(a+4);"
   " float rr=ar*br-ai*bi, ri=ar*bi+ai*br; int o0,o1; __builtin_memcpy(&o0,&rr,4); __builtin_memcpy(&o1,&ri,4);"
   " return (long)(unsigned)(o0^(o1*7)); }\n",
   {6}, "Complex", 2, ""},
};

static const std::vector<RoundTripTC> kA64 = {
  // FCADD #90 — {re=Vn.re-Vm.im, im=Vn.im+Vm.re} per complex pair.
  {"a64_fcadd90",
   "#include <arm_neon.h>\n"
   "long a64_fcadd90(long a){ float va[4],vb[4];"
   " va[0]=(float)(a+1);va[1]=(float)(a+2);va[2]=(float)(a+3);va[3]=(float)(a+4);"
   " vb[0]=(float)(a+5);vb[1]=(float)(a+6);vb[2]=(float)(a+7);vb[3]=(float)(a+8);"
   " float32x4_t r=vcaddq_rot90_f32(vld1q_f32(va),vld1q_f32(vb)); int o[4]; vst1q_f32((float*)o,r);"
   " return (long)(unsigned)(o[0]^(o[1]*7)^(o[2]*13)^(o[3]*17)); }\n",
   {6}, A64V83},
  // FCADD #270 — {re=Vn.re+Vm.im, im=Vn.im-Vm.re}.
  {"a64_fcadd270",
   "#include <arm_neon.h>\n"
   "long a64_fcadd270(long a){ float va[4],vb[4];"
   " va[0]=(float)(a+1);va[1]=(float)(a+2);va[2]=(float)(a+3);va[3]=(float)(a+4);"
   " vb[0]=(float)(a+5);vb[1]=(float)(a+6);vb[2]=(float)(a+7);vb[3]=(float)(a+8);"
   " float32x4_t r=vcaddq_rot270_f32(vld1q_f32(va),vld1q_f32(vb)); int o[4]; vst1q_f32((float*)o,r);"
   " return (long)(unsigned)(o[0]^(o[1]*7)^(o[2]*13)^(o[3]*17)); }\n",
   {6}, A64V83},
  // FCMLA rot0 + rot90 = full complex multiply-accumulate.
  {"a64_fcmla",
   "#include <arm_neon.h>\n"
   "long a64_fcmla(long a){ float va[4],vb[4],vc[4];"
   " va[0]=(float)(a+1);va[1]=(float)(a+2);va[2]=(float)(a+3);va[3]=(float)(a+4);"
   " vb[0]=(float)(a+5);vb[1]=(float)(a+6);vb[2]=(float)(a+7);vb[3]=(float)(a+8);"
   " vc[0]=(float)(a+9);vc[1]=(float)(a+10);vc[2]=(float)(a+11);vc[3]=(float)(a+12);"
   " float32x4_t acc=vld1q_f32(vc);"
   " acc=vcmlaq_f32(acc,vld1q_f32(va),vld1q_f32(vb));"
   " acc=vcmlaq_rot90_f32(acc,vld1q_f32(va),vld1q_f32(vb));"
   " int o[4]; vst1q_f32((float*)o,acc);"
   " return (long)(unsigned)(o[0]^(o[1]*7)^(o[2]*13)^(o[3]*17)); }\n",
   {4}, A64V83},
};

static const std::vector<RoundTripTC> kArm32 = {
  // VCADD #90 (AArch32 complex add).
  {"arm_vcadd90",
   "#include <arm_neon.h>\n"
   "long arm_vcadd90(long a){ float va[4],vb[4];"
   " va[0]=(float)(a+1);va[1]=(float)(a+2);va[2]=(float)(a+3);va[3]=(float)(a+4);"
   " vb[0]=(float)(a+5);vb[1]=(float)(a+6);vb[2]=(float)(a+7);vb[3]=(float)(a+8);"
   " float32x4_t r=vcaddq_rot90_f32(vld1q_f32(va),vld1q_f32(vb)); int o[4]; vst1q_f32((float*)o,r);"
   " return (long)(unsigned)(o[0]^(o[1]*7)^(o[2]*13)^(o[3]*17)); }\n",
   {6}, A32V83},
  // VCMLA rot0 + rot90 = complex multiply-accumulate.  clang 17 cannot lower
  // the vcmlaq_f32 builtin for AArch32, so drive the instruction via inline asm
  // (its integrated assembler accepts `vcmla.f32`).
  {"arm_vcmla",
   "long arm_vcmla(long a){ float va[4],vb[4],vc[4];"
   " va[0]=(float)(a+1);va[1]=(float)(a+2);va[2]=(float)(a+3);va[3]=(float)(a+4);"
   " vb[0]=(float)(a+5);vb[1]=(float)(a+6);vb[2]=(float)(a+7);vb[3]=(float)(a+8);"
   " vc[0]=(float)(a+9);vc[1]=(float)(a+10);vc[2]=(float)(a+11);vc[3]=(float)(a+12);"
   " __asm__ volatile(\"vld1.32 {q1},[%1]\\nvld1.32 {q2},[%2]\\nvld1.32 {q0},[%0]\\nvcmla.f32 q0,q1,q2,#0\\nvcmla.f32 q0,q1,q2,#90\\nvst1.32 {q0},[%0]\\n\" : : \"r\"(vc),\"r\"(va),\"r\"(vb) : \"q0\",\"q1\",\"q2\",\"memory\");"
   " int o[4]; __builtin_memcpy(o,vc,16);"
   " return (long)(unsigned)(o[0]^(o[1]*7)^(o[2]*13)^(o[3]*17)); }\n",
   {4}, A32V83},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(Complex, X64ComplexRT, ::testing::ValuesIn(kX64),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(Complex, A64ComplexRT, ::testing::ValuesIn(kA64),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(Complex, ARM32ComplexRT, ::testing::ValuesIn(kArm32),
                         rtTCName);
