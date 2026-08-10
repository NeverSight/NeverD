//===- AArch64_ScalarShiftRegRTTests.cpp - scalar SSHL/USHL D-form *- C++ -*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// AArch64 NEON SSHL/USHL also exist in a SCALAR (64-bit `d`-register) form:
//
//   USHL Dd, Dn, Dm   Dd = Dn << Dm.byte[0]   (logical;  byte[0] signed:
//   SSHL Dd, Dn, Dm   Dd = Dn << Dm.byte[0]    negative => right shift,
//                                               arithmetic for SSHL)
//
// The vector forms decode with a valid `vas` arrangement, so the lifter picks a
// per-lane ElemSz and emits the correct positive-left / negative-right SELECT.
// The scalar form leaves `vas` INVALID, so ElemSz stays 0 — and UNLIKE its
// siblings SQSHL/UQSHL/SQSHLU and SLI/SRI (which all do `if (ElemSz==0)
// ElemSz=Dst.Size` to treat the whole register as one lane) the SSHL/USHL
// handler had no such fallback.  It dropped to a bare full-width `INT_LEFT`,
// which:
//   * uses the WHOLE Dm register as the count (not just the signed low byte), and
//   * can only ever shift LEFT.
// A negative scalar amount (a right shift) therefore became a giant left shift
// that the saturating INT_LEFT flushes to 0 — the memory/register value was
// silently destroyed.  Positive small amounts happened to work (left == left),
// which is exactly why the gap went unnoticed.
//
// Each probe loads a value + a signed shift amount into d1/d2, runs the scalar
// shift, and returns the result.  Negative-amount cases are RED before the fix
// (recompiled returns 0); positive-amount controls and a vector control are
// GREEN on both sides and guard against an over-broad fix.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64ScalarShiftRegRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64ScalarShiftRegRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kA64 = {

  // ===== USHL Dd,Dn,Dm with NEGATIVE amount => logical right shift (RED). =====
  // val=arg, sh=-4 -> arg >>L 4.
  {"ushl_d_right4",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long val=a, sh=(unsigned long)-4, out;\n"
   "  __asm__ volatile(\"fmov d1, %1\\n\\tfmov d2, %2\\n\\t\"\n"
   "                   \"ushl d0, d1, d2\\n\\tfmov %0, d0\\n\\t\"\n"
   "    :\"=r\"(out):\"r\"(val),\"r\"(sh):\"v0\",\"v1\",\"v2\");\n"
   "  return out;}\n",
   {0xF0F0F0F0F0F0F0F0ULL}, "ScalarShiftReg"},

  // val=arg, sh=-13 (odd amount) -> arg >>L 13.
  {"ushl_d_right13",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long val=a, sh=(unsigned long)-13, out;\n"
   "  __asm__ volatile(\"fmov d1, %1\\n\\tfmov d2, %2\\n\\t\"\n"
   "                   \"ushl d0, d1, d2\\n\\tfmov %0, d0\\n\\t\"\n"
   "    :\"=r\"(out):\"r\"(val),\"r\"(sh):\"v0\",\"v1\",\"v2\");\n"
   "  return out;}\n",
   {0x123456789ABCDEF0ULL}, "ScalarShiftReg"},

  // ===== SSHL Dd,Dn,Dm with NEGATIVE amount => ARITHMETIC right shift (RED). ==
  // val is a large negative i64; sh=-4 -> arithmetic >> 4 keeps the sign bits.
  {"sshl_d_right4",
   "long f(long a){\n"
   "  long val=a, sh=-4, out;\n"
   "  __asm__ volatile(\"fmov d1, %1\\n\\tfmov d2, %2\\n\\t\"\n"
   "                   \"sshl d0, d1, d2\\n\\tfmov %0, d0\\n\\t\"\n"
   "    :\"=r\"(out):\"r\"(val),\"r\"(sh):\"v0\",\"v1\",\"v2\");\n"
   "  return out;}\n",
   {0xFFFFFFFFFFFFFF00ULL}, "ScalarShiftReg"},

  // ===== Positive-amount controls: LEFT shift (GREEN before and after). =====
  {"ushl_d_left4",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long val=a, sh=4, out;\n"
   "  __asm__ volatile(\"fmov d1, %1\\n\\tfmov d2, %2\\n\\t\"\n"
   "                   \"ushl d0, d1, d2\\n\\tfmov %0, d0\\n\\t\"\n"
   "    :\"=r\"(out):\"r\"(val),\"r\"(sh):\"v0\",\"v1\",\"v2\");\n"
   "  return out;}\n",
   {0x00000000ABCDEF12ULL}, "ScalarShiftReg"},

  {"sshl_d_left8",
   "long f(long a){\n"
   "  long val=a, sh=8, out;\n"
   "  __asm__ volatile(\"fmov d1, %1\\n\\tfmov d2, %2\\n\\t\"\n"
   "                   \"sshl d0, d1, d2\\n\\tfmov %0, d0\\n\\t\"\n"
   "    :\"=r\"(out):\"r\"(val),\"r\"(sh):\"v0\",\"v1\",\"v2\");\n"
   "  return out;}\n",
   {0x0000000000000003ULL}, "ScalarShiftReg"},

  // ===== Vector control: ushl v0.8b with a per-lane negative amount. =====
  // Guards that adding the scalar fallback does not disturb the vector path.
  {"ushl_8b_vec",
   "#include <arm_neon.h>\n"
   "long f(long a){\n"
   "  uint8x8_t v={(unsigned char)a,2,3,4,5,6,7,8};\n"
   "  int8x8_t s={-1,-2,-3,-4,1,2,3,4};\n"
   "  uint8x8_t r=vshl_u8(v,s);\n"
   "  unsigned char o[8]; vst1_u8(o,r);\n"
   "  int acc=0; for(int i=0;i<8;i++) acc=acc*31+o[i];\n"
   "  return (long)(unsigned)acc;}\n",
   {0xF0ULL}, "ScalarShiftReg", 1, "-march=armv8-a+simd"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(ScalarShiftReg, A64ScalarShiftRegRT,
                         ::testing::ValuesIn(kA64), rtTCName);
