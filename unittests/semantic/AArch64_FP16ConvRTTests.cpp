//===- AArch64_FP16ConvRTTests.cpp - half-precision FP conversions -*- C++ -*-===//
//
// Roundtrip probes for AArch64 half-precision (FEAT_FP16) conversions:
//   - int <-> fp16 (SCVTF/UCVTF, FCVTZS/FCVTZU), scalar and vector .4H
//   - fp16 <-> fp32 / fp16 <-> fp64 (FCVT), scalar
//
// Follow-up to #290 (fp16 arithmetic): the conversion handlers in
// AArch64LiftFP.cpp had the same .8H/.4H gap (only .4S/.2S/.2D recognised), and
// the emitter's FLOAT_INT2FLOAT / FLOAT_FLOAT2FLOAT hardcoded float/double for
// the destination so a half target became double.  Now the destination
// precision follows the output width (h/s/d) and the lane size covers .8H/.4H.
//
// Data is moved via integer fmov (bit copies); the fp16 bit patterns are passed
// as integer inputs.  Requires -march=armv8.2-a+fp16; the default AArch64
// Unicorn MAX CPU executes fp16 conversions natively.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64FP16ConvRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64FP16ConvRT, Verify) { roundTripAArch64(GetParam()); }

#define FP16FLAGS "FP16Conv", 0, "-march=armv8.2-a+fp16"

// clang-format off
static const std::vector<RoundTripTC> kA64 = {
  // --- int -> fp16 scalar (SCVTF/UCVTF) ---
  // int 3 -> 3.0h (0x4200)
  {"scvtf_h_w",
   "long f(long a){unsigned int r;"
   "__asm__ volatile(\"scvtf h0,%w1\\n fmov %w0,s0\":\"=r\"(r):\"r\"((int)a):\"v0\");"
   "return (long)r;}\n",
   {3ULL}, FP16FLAGS},

  {"ucvtf_h_w",
   "long f(long a){unsigned int r;"
   "__asm__ volatile(\"ucvtf h0,%w1\\n fmov %w0,s0\":\"=r\"(r):\"r\"((unsigned)a):\"v0\");"
   "return (long)r;}\n",
   {100ULL}, FP16FLAGS},

  // --- fp16 -> int scalar (FCVTZS/FCVTZU), truncating ---
  // 3.5h (0x4300) -> 3
  {"fcvtzs_w_h",
   "long f(long a){unsigned int r;"
   "__asm__ volatile(\"fmov s0,%w1\\n fcvtzs %w0,h0\":\"=r\"(r):\"r\"((unsigned)a):\"v0\");"
   "return (long)r;}\n",
   {0x4300ULL}, FP16FLAGS},

  // -2.5h (0xC100) -> -2
  {"fcvtzs_w_h_neg",
   "long f(long a){int r;"
   "__asm__ volatile(\"fmov s0,%w1\\n fcvtzs %w0,h0\":\"=r\"(r):\"r\"((unsigned)a):\"v0\");"
   "return (long)r;}\n",
   {0xC100ULL}, FP16FLAGS},

  {"fcvtzu_w_h",
   "long f(long a){unsigned int r;"
   "__asm__ volatile(\"fmov s0,%w1\\n fcvtzu %w0,h0\":\"=r\"(r):\"r\"((unsigned)a):\"v0\");"
   "return (long)r;}\n",
   {0x5640ULL}, FP16FLAGS}, // 100.0h -> 100

  // --- fp16 <-> fp32 (FCVT) ---
  // 1.5h (0x3E00) -> 1.5f (0x3FC00000)
  {"fcvt_s_h",
   "long f(long a){unsigned int r;"
   "__asm__ volatile(\"fmov s0,%w1\\n fcvt s0,h0\\n fmov %w0,s0\":\"=r\"(r):\"r\"((unsigned)a):\"v0\");"
   "return (long)r;}\n",
   {0x3E00ULL}, FP16FLAGS},

  // 2.5f (0x40200000) -> 2.5h (0x4100)
  {"fcvt_h_s",
   "long f(long a){unsigned int r;"
   "__asm__ volatile(\"fmov s0,%w1\\n fcvt h0,s0\\n fmov %w0,s0\":\"=r\"(r):\"r\"((unsigned)a):\"v0\");"
   "return (long)r;}\n",
   {0x40200000ULL}, FP16FLAGS},

  // --- fp16 <-> fp64 (FCVT) ---
  // 0.5h (0x3800) -> 0.5 (0x3FE0000000000000)
  {"fcvt_d_h",
   "long f(long a){unsigned long r;"
   "__asm__ volatile(\"fmov s0,%w1\\n fcvt d0,h0\\n fmov %0,d0\":\"=r\"(r):\"r\"((unsigned)a):\"v0\");"
   "return (long)r;}\n",
   {0x3800ULL}, FP16FLAGS},

  // 4.0 (0x4010000000000000) -> 4.0h (0x4400)
  {"fcvt_h_d",
   "long f(long a){unsigned int r;"
   "__asm__ volatile(\"fmov d0,%1\\n fcvt h0,d0\\n fmov %w0,s0\":\"=r\"(r):\"r\"((unsigned long)a):\"v0\");"
   "return (long)r;}\n",
   {0x4010000000000000ULL}, FP16FLAGS},

  // --- vector int16 -> fp16 (.4H per-lane SCVTF) ---
  // [1,2,3,4] -> [1.0,2.0,3.0,4.0]h
  {"scvtf_4h",
   "long f(long a){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n scvtf v0.4h,v0.4h\\n fmov %0,d0\":\"=r\"(r):\"r\"((unsigned long)a):\"v0\");"
   "return (long)r;}\n",
   {0x0004000300020001ULL}, FP16FLAGS},

  // --- vector fp16 -> int16 (.4H per-lane FCVTZS, truncating) ---
  // [1.5,2.5,3.5,4.5]h -> [1,2,3,4]
  {"fcvtzs_4h",
   "long f(long a){unsigned long r;"
   "__asm__ volatile(\"fmov d0,%1\\n fcvtzs v0.4h,v0.4h\\n fmov %0,d0\":\"=r\"(r):\"r\"((unsigned long)a):\"v0\");"
   "return (long)r;}\n",
   {0x4480430041003E00ULL}, FP16FLAGS},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(FP16Conv, AArch64FP16ConvRT,
                         ::testing::ValuesIn(kA64), rtTCName);
