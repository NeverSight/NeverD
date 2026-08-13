//===- AArch64_FMulxRTTests.cpp - FP multiply-extended (FMULX) ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for AArch64 `FMULX` (FP multiply-extended).  FMULX behaves
// like FMUL for ordinary operands but defines 0*Inf = +/-2.0 (so the Newton-
// Raphson reciprocal/rsqrt refinement steps stay well-defined at the
// boundaries).  The lifter (AArch64LiftSIMD.cpp) used a single whole-register
// `FLOAT_MULT`, which is wrong two ways:
//
//   * Vectors (.2s/.4s/.2d) collapse to ONE FP op: the float emitter infers a
//     scalar f32/f64 from the operand byte-width and (for 128-bit) truncates to
//     the low 64 bits, so every lane comes out wrong.
//   * Scalars get plain FMUL semantics: 0*Inf yields NaN instead of +/-2.0.
//
// Now mapped to the real `llvm.aarch64.neon.fmulx` intrinsic (per-lane, exact
// 0*Inf), so codegen emits a true `fmulx` and the recompiled code matches
// Unicorn bit-for-bit.  Data moves through integer `fmov`/`dup` (pure bit
// copies); f32 bit patterns: 2.0=0x40000000 3.0=0x40400000 6.0=0x40C00000
// +Inf=0x7F800000 -0.0=0x80000000 -2.0=0xC0000000.  FMULX is base ARMv8.0
// FP/NEON; the default AArch64 Unicorn CPU executes it natively.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64FMulxRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64FMulxRT, Verify) { roundTripAArch64(GetParam()); }

#define FMULXFLAGS "FMulx", 0, ""

// clang-format off
static const std::vector<RoundTripTC> kA64 = {
  // --- .2S vector (2 f32 lanes in a D register) ---
  // v0=[2.0,3.0] v1=[4.0,5.0] -> [8.0,15.0]; whole-reg FLOAT_MULT treats the
  // 64-bit pair as ONE double, corrupting both lanes.
  {"fmulx_2s",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n fmov d1,%2\\n"
   " fmulx v0.2s,v0.2s,v1.2s\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b)"
   ":\"v0\",\"v1\");"
   "return (long)r;}\n",
   {0x4040000040000000ULL, 0x40A0000040800000ULL}, FMULXFLAGS},

  // --- .4S vector (4 f32 lanes in a Q register, broadcast via dup) ---
  // all lanes 2.0*3.0=6.0; read back low 64 bits = [6.0,6.0].
  {"fmulx_4s",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"dup v0.4s,%w1\\n dup v1.4s,%w2\\n"
   " fmulx v0.4s,v0.4s,v1.4s\\n fmov %0,d0\""
   ":\"=r\"(r):\"r\"((unsigned int)a),\"r\"((unsigned int)b)"
   ":\"v0\",\"v1\");"
   "return (long)r;}\n",
   {0x40000000ULL, 0x40400000ULL}, FMULXFLAGS},

  // --- .2D vector (2 f64 lanes, broadcast via dup), read the HIGH lane ---
  // whole-reg FLOAT_MULT keeps only lane0 (the truncated low double); lane1
  // comes back 0, so reading v0.d[1] exposes the dropped lane.
  {"fmulx_2d_hi",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"dup v0.2d,%1\\n dup v1.2d,%2\\n"
   " fmulx v0.2d,v0.2d,v1.2d\\n mov %0,v0.d[1]\""
   ":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b)"
   ":\"v0\",\"v1\");"
   "return (long)r;}\n",
   {0x4000000000000000ULL, 0x4008000000000000ULL}, FMULXFLAGS},

  // --- Scalar S: 0.0 * +Inf = +2.0 (FMUL would give NaN) ---
  {"fmulx_zero_inf_s",
   "long f(long a,long b){unsigned int r;"
   "__asm__ volatile(\"fmov s0,%w1\\n fmov s1,%w2\\n"
   " fmulx s0,s0,s1\\n fmov %w0,s0\""
   ":\"=r\"(r):\"r\"((unsigned int)a),\"r\"((unsigned int)b)"
   ":\"v0\",\"v1\");"
   "return (long)r;}\n",
   {0x00000000ULL, 0x7F800000ULL}, FMULXFLAGS},

  // --- Scalar S: -0.0 * +Inf = -2.0 (sign of the extended product) ---
  {"fmulx_neg_zero_inf_s",
   "long f(long a,long b){unsigned int r;"
   "__asm__ volatile(\"fmov s0,%w1\\n fmov s1,%w2\\n"
   " fmulx s0,s0,s1\\n fmov %w0,s0\""
   ":\"=r\"(r):\"r\"((unsigned int)a),\"r\"((unsigned int)b)"
   ":\"v0\",\"v1\");"
   "return (long)r;}\n",
   {0x80000000ULL, 0x7F800000ULL}, FMULXFLAGS},

  // --- Scalar S normal control (already correct: FMULX == FMUL for finites) ---
  // 3.0 * 2.0 = 6.0
  {"fmulx_scalar_s",
   "long f(long a,long b){unsigned int r;"
   "__asm__ volatile(\"fmov s0,%w1\\n fmov s1,%w2\\n"
   " fmulx s0,s0,s1\\n fmov %w0,s0\""
   ":\"=r\"(r):\"r\"((unsigned int)a),\"r\"((unsigned int)b)"
   ":\"v0\",\"v1\");"
   "return (long)r;}\n",
   {0x40400000ULL, 0x40000000ULL}, FMULXFLAGS},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(FMulx, AArch64FMulxRT,
                         ::testing::ValuesIn(kA64), rtTCName);
