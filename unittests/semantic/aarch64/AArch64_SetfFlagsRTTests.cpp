//===- AArch64_SetfFlagsRTTests.cpp - SETF8/SETF16 flag setting --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for the AArch64 FlagM instructions SETF8 / SETF16
// ("evaluate into flags").  Given a Wn operand they set NZV from the low 8
// (SETF8) or low 16 (SETF16) bits as if an 8/16-bit signed value had been
// produced, leaving C unchanged:
//
//   N = operand<msb>                 (msb = 7 for SETF8, 15 for SETF16)
//   Z = (operand<msb:0> == 0)
//   V = operand<msb+1> EOR operand<msb>
//   C unchanged
//
// The old lifter computed Z = (whole 32-bit == 0) and N = (whole 32-bit < 0,
// i.e. bit 31), never distinguished SETF8 from SETF16, and never wrote V — so
// the byte/halfword flag semantics were entirely wrong.  Small values masked
// the bug (low byte == full word, bit7 == bit31 == 0), so it went unnoticed.
//
// Each probe runs the instruction via inline asm and captures the resulting
// N/Z/V with setcc-equivalent `cset` (mi/eq/vs), folding them into distinct
// return bits so any single wrong flag changes the result.  SETF8/SETF16 are
// FEAT_FlagM (ARMv8.4), so select Unicorn's MAX CPU explicitly; Unicorn's
// default is an A72 model without FlagM.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64SetfFlagsRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64SetfFlagsRT, Verify) { roundTripAArch64(GetParam()); }

// SETF8 wrapper: run `setf8 Wn`, capture N(mi)/Z(eq)/V(vs) into bits 0/1/2.
#define SETF8_FN \
  "long f(long a){unsigned int n,z,v;" \
  "__asm__ volatile(\"setf8 %w3\\n\\t\"" \
  "\"cset %w0, mi\\n\\t\"\"cset %w1, eq\\n\\t\"\"cset %w2, vs\"" \
  ":\"=&r\"(n),\"=&r\"(z),\"=&r\"(v):\"r\"((unsigned int)a):\"cc\");" \
  "return (long)n|((long)z<<1)|((long)v<<2);}\n"

// SETF16 wrapper.
#define SETF16_FN \
  "long f(long a){unsigned int n,z,v;" \
  "__asm__ volatile(\"setf16 %w3\\n\\t\"" \
  "\"cset %w0, mi\\n\\t\"\"cset %w1, eq\\n\\t\"\"cset %w2, vs\"" \
  ":\"=&r\"(n),\"=&r\"(z),\"=&r\"(v):\"r\"((unsigned int)a):\"cc\");" \
  "return (long)n|((long)z<<1)|((long)v<<2);}\n"

// clang-format off
static const std::vector<RoundTripTC> kA64 = {
  // --- SETF8: msb = bit 7 ---
  // low byte == 0 but upper bits set: Z must come from bits[7:0], not the
  // whole word.  bit8=1,bit7=0 => V=1.  (old: Z=0 from 0x100!=0, V unset)
  {"setf8_z_low8",  SETF8_FN,  {0x100ULL},        "SetfFlags", 0,
   "-march=armv8.4-a", false, "", UC_CPU_ARM64_MAX},
  // bit7 set (but bit31 clear): N must come from bit 7, not bit 31.
  // bit8=0,bit7=1 => V=1.  (old: N=0 from bit31, V unset)
  {"setf8_n_bit7",  SETF8_FN,  {0x80ULL},         "SetfFlags", 0,
   "-march=armv8.4-a", false, "", UC_CPU_ARM64_MAX},
  // all-ones low byte: N=1 (bit7), Z=0, V = bit8(0) ^ bit7(1) = 1.
  {"setf8_allones", SETF8_FN,  {0xFFULL},         "SetfFlags", 0,
   "-march=armv8.4-a", false, "", UC_CPU_ARM64_MAX},
  // bit8==bit7==1 => V=0 (no signed 8-bit overflow); N=1, Z=0.
  {"setf8_no_ovf",  SETF8_FN,  {0x180ULL},        "SetfFlags", 0,
   "-march=armv8.4-a", false, "", UC_CPU_ARM64_MAX},

  // --- SETF16: msb = bit 15 ---
  // low 16 bits == 0, bit16 set: Z from bits[15:0]; V = bit16(1)^bit15(0)=1.
  {"setf16_z_low16", SETF16_FN, {0x10000ULL},     "SetfFlags", 0,
   "-march=armv8.4-a", false, "", UC_CPU_ARM64_MAX},
  // bit15 set (bit31 clear): N from bit 15.  V = bit16(0)^bit15(1)=1.
  {"setf16_n_bit15", SETF16_FN, {0x8000ULL},      "SetfFlags", 0,
   "-march=armv8.4-a", false, "", UC_CPU_ARM64_MAX},
  // bit16==bit15==1 => V=0; N=1, Z=0.
  {"setf16_no_ovf",  SETF16_FN, {0x18000ULL},     "SetfFlags", 0,
   "-march=armv8.4-a", false, "", UC_CPU_ARM64_MAX},

  // --- Control: large value where bit7==bit31==1, bit8==bit7 (V=0).
  // Correct (N=bit7=1) and old (N=bit31=1) agree, V correct=0 matches the
  // unwritten/entry V=0 — passes both RED and GREEN, illustrating how only
  // sub-word-sensitive inputs expose the bug.  Returns 1, not 0.
  {"setf8_control", SETF8_FN,  {0x80000180ULL},   "SetfFlags", 0,
   "-march=armv8.4-a", false, "", UC_CPU_ARM64_MAX},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(SetfFlags, AArch64SetfFlagsRT,
                         ::testing::ValuesIn(kA64), rtTCName);
