//===- ARM32_ParallelSatHalveRTTests.cpp - ARMv6 parallel SIMD edges ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for the *halving* (H), *saturating* (Q) and *exchange* (SAX)
// members of the ARMv6 GE-setting parallel (SIMD-in-GPR) add/sub family, plus
// the plain GE-setting forms that the existing ARM32_ParallelSIMDRTTests left
// uncovered (USUB16/SSUB16/UASX with SEL).  These all share the one lane loop in
// ARMLiftCoreExt.cpp; the original suite only exercised UADD8/SADD8/USUB8/SSUB8/
// UADD16/SADD16/SASX, leaving these arms with zero roundtrip coverage:
//
//   * Halving  (SH*/UH*): result = (a +/- b) >> 1 per lane.  The intermediate is
//     sign- (signed) or zero- (unsigned) extended, then ARITHMETICALLY shifted
//     right by one — so an unsigned halving SUBTRACT with a<b (negative diff)
//     must keep the borrow's sign through the shift (e.g. UHSUB8 0x00,0xFF ->
//     (0-255)>>1 = 0x80, NOT 0x7F).  No GE flags.
//   * Saturating (Q*/UQ*): each lane clamps to its signed/unsigned range and the
//     Q/saturating forms leave GE unchanged (only the DSP QADD/QSUB set Q).
//   * Exchange (*ASX/*SAX): the second operand's halves are swapped; ASX does
//     low=sub/high=add, SAX does low=add/high=sub.  SAX (and its halving/sat
//     cousins) were entirely untested — only the SASX direction had a probe.
//
// GE-setting plain forms are paired with SEL so a wrong GE lane shows in the
// result byte selection; H/Q forms fold the raw lane result into the return.
// Inputs are chosen to hit carry/borrow, signed/unsigned clamp, and the
// negative-difference halving shift.  Driven via inline asm; ARM32 only.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32ParallelSatHalveRT : public SemanticRoundTripFixture,
                                public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32ParallelSatHalveRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

// Drive a single 3-operand parallel-SIMD op writing directly to the result reg
// (no GE consumer): orig runs the real opcode, recompiled runs NeverD's lift.
#define P3(op) \
  "unsigned f(unsigned a, unsigned b){ unsigned r;" \
  " __asm__ volatile(\"" op " %0,%1,%2\\n\" : \"=r\"(r) : \"r\"(a),\"r\"(b) : \"cc\");" \
  " return r; }\n"

// Drive a GE-setting op (scratch r4) then SEL so the GE lanes select bytes.
#define PSEL(op) \
  "unsigned f(unsigned a, unsigned b){ unsigned r;" \
  " __asm__ volatile(\"" op " r4,%1,%2\\nsel %0,%1,%2\\n\"" \
  " : \"=&r\"(r) : \"r\"(a),\"r\"(b) : \"r4\",\"cc\");" \
  " return r; }\n"

static const std::vector<RoundTripTC> kArm32 = {
  // ===== Halving, 8-bit (no GE).  (a +/- b) >> 1 per byte. =====
  // UHADD8: unsigned, includes carrying bytes (sum >= 256).
  {"arm_uhadd8", P3("uhadd8"), {0xFF01FE80u, 0xFF80FE01u}, "ParSatHalve", 1, ""},
  // SHADD8: signed, includes -128+-128 = -256 -> >>1 = -128 (0x80).
  {"arm_shadd8", P3("shadd8"), {0x7F800140u, 0x7F800140u}, "ParSatHalve", 1, ""},
  // UHSUB8: unsigned subtract with a<b lane (0x00-0xFF) -> negative diff,
  // arithmetic >>1 keeps the borrow sign (0x80, not 0x7F).
  {"arm_uhsub8", P3("uhsub8"), {0x00FF1080u, 0xFF002080u}, "ParSatHalve", 1, ""},
  // SHSUB8: signed subtract, mixed-sign lanes.
  {"arm_shsub8", P3("shsub8"), {0x807F0110u, 0x7F800120u}, "ParSatHalve", 1, ""},

  // ===== Halving, 16-bit (no GE). =====
  {"arm_uhadd16", P3("uhadd16"), {0xFFFF0001u, 0x00020003u}, "ParSatHalve", 1, ""},
  {"arm_shadd16", P3("shadd16"), {0x80007FFFu, 0x80000001u}, "ParSatHalve", 1, ""},
  {"arm_uhsub16", P3("uhsub16"), {0x0000FFFFu, 0xFFFF0001u}, "ParSatHalve", 1, ""},
  {"arm_shsub16", P3("shsub16"), {0x80000001u, 0x7FFF0002u}, "ParSatHalve", 1, ""},

  // ===== Halving exchange (no GE): SH/UH ASX & SAX. =====
  {"arm_shasx", P3("shasx"), {0x00207FFFu, 0x7FFF0010u}, "ParSatHalve", 1, ""},
  {"arm_uhasx", P3("uhasx"), {0x7FFF0020u, 0x00107FFFu}, "ParSatHalve", 1, ""},
  {"arm_shsax", P3("shsax"), {0x7FFF0020u, 0x00107FFFu}, "ParSatHalve", 1, ""},
  {"arm_uhsax", P3("uhsax"), {0x00207FFFu, 0x7FFF0010u}, "ParSatHalve", 1, ""},

  // ===== Saturating, 8-bit (no GE). =====
  // UQADD8: 0x01+0xFF and 0xFE+0x02 -> 0x100 clamp to 0xFF.
  {"arm_uqadd8", P3("uqadd8"), {0x80017FFEu, 0x01FF7F02u}, "ParSatHalve", 1, ""},
  // QADD8 (signed): 0x7F+0x7F -> +clamp 0x7F; 0x80+0x80 -> -clamp 0x80.
  {"arm_qadd8", P3("qadd8"), {0x7F800102u, 0x7F800102u}, "ParSatHalve", 1, ""},
  // UQSUB8: 0x00-0xFF -> clamp to 0.
  {"arm_uqsub8", P3("uqsub8"), {0x00102030u, 0xFF002040u}, "ParSatHalve", 1, ""},
  // QSUB8 (signed): 0x7F-0x80 -> +clamp 0x7F; 0x80-0x7F -> -clamp 0x80.
  {"arm_qsub8", P3("qsub8"), {0x7F800110u, 0x807F0120u}, "ParSatHalve", 1, ""},

  // ===== Saturating, 16-bit (no GE). =====
  // QADD16: 0x7FFF+0x7FFF -> +clamp; 0x8000+0x8000 -> -clamp.
  {"arm_qadd16", P3("qadd16"), {0x7FFF8000u, 0x7FFF8000u}, "ParSatHalve", 1, ""},
  {"arm_uqadd16", P3("uqadd16"), {0xFFFF0001u, 0x00028000u}, "ParSatHalve", 1, ""},
  // UQSUB16: 0x0000-0xFFFF -> clamp to 0.
  {"arm_uqsub16", P3("uqsub16"), {0x00001000u, 0xFFFF2000u}, "ParSatHalve", 1, ""},
  {"arm_qsub16", P3("qsub16"), {0x7FFF8000u, 0x80007FFFu}, "ParSatHalve", 1, ""},

  // ===== Saturating exchange (no GE): Q/UQ ASX & SAX. =====
  {"arm_qasx", P3("qasx"), {0x7FFF8000u, 0x80007FFFu}, "ParSatHalve", 1, ""},
  {"arm_uqasx", P3("uqasx"), {0xFFFF0001u, 0x0001FFFFu}, "ParSatHalve", 1, ""},
  {"arm_qsax", P3("qsax"), {0x80007FFFu, 0x7FFF8000u}, "ParSatHalve", 1, ""},
  {"arm_uqsax", P3("uqsax"), {0x0000FFFFu, 0xFFFF0001u}, "ParSatHalve", 1, ""},

  // ===== Plain GE-setting forms the original suite missed, via SEL. =====
  // USUB16: GE[hw] = (Rn.hw >= Rm.hw) (no borrow), duplicated across the 2 bytes.
  {"arm_usub16_sel", PSEL("usub16"), {0x10002000u, 0x20001000u}, "ParSatHalve", 1, ""},
  // SSUB16: GE[hw] = (signed diff >= 0).
  {"arm_ssub16_sel", PSEL("ssub16"), {0x80007FFFu, 0x7FFF8000u}, "ParSatHalve", 1, ""},
  // UASX: low=sub (GE from no-borrow), high=add (GE from carry); halves swapped.
  {"arm_uasx_sel", PSEL("uasx"), {0xFFFF0001u, 0x0001FFFFu}, "ParSatHalve", 1, ""},
  // SASX already covered upstream; add the SAX direction with signed GE.
  {"arm_ssax_sel", PSEL("ssax"), {0x7FFF8000u, 0x80007FFFu}, "ParSatHalve", 1, ""},
  // USAX: low=add (GE carry), high=sub (GE no-borrow).
  {"arm_usax_sel", PSEL("usax"), {0x0001FFFFu, 0xFFFF0001u}, "ParSatHalve", 1, ""},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(ParSatHalve, ARM32ParallelSatHalveRT,
                         ::testing::ValuesIn(kArm32), rtTCName);
