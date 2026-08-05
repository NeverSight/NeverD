//===- AArch64_MulHighLongRTTests.cpp - high/long multiply edges ----------===//
//
// Roundtrip guard-rail probes for the AArch64 high-half and widening
// multiply / multiply-accumulate family, exercised at the SIGNED-vs-UNSIGNED
// boundary that distinguishes them:
//
//   * SMULH/UMULH  — upper 64 bits of a 64x64 -> 128 product.  Signed and
//     unsigned high halves diverge for any operand with bit63 set (the lift
//     must SEXT vs ZEXT to 128 bits, not reuse one width).
//   * SMADDL/UMADDL/SMSUBL/UMSUBL — 32x32 -> 64 multiply (+/-) a 64-bit
//     accumulator.  Only the low 32 bits (Wn/Wm) feed the product, so the high
//     32 bits of the source X registers MUST be ignored; probes seed garbage
//     high bits to catch a lift that reads the full 64-bit register.
//   * SMULL/UMULL and SMNEGL/UMNEGL — Capstone-6 aliases of the *MADDL/*MSUBL
//     forms with the zero register, surfaced with op_count==3.
//   * MADD/MSUB — 64-bit fused multiply add/sub.
//
// Inputs hit INT_MIN, all-ones, and negative*positive so a signed/unsigned or
// width confusion in the lift diverges from the Unicorn truth.  Base ARMv8 —
// no target feature needed.  AArch64 only.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64MulHighLongRT : public SemanticRoundTripFixture,
                             public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64MulHighLongRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

// 64-bit x*y high half (2 args).
#define H2(op) \
  "unsigned long f(unsigned long a, unsigned long b){ unsigned long r;" \
  " __asm__(\"" op " %0,%1,%2\" : \"=r\"(r) : \"r\"(a),\"r\"(b)); return r; }\n"
// widening 32x32 (+/- accumulator) -> 64 (3 args; Wn/Wm from low 32 of a/b).
#define L3(op) \
  "unsigned long f(unsigned long a, unsigned long b, unsigned long c){ unsigned long r;" \
  " __asm__(\"" op " %0,%w1,%w2,%3\" : \"=r\"(r) : \"r\"(a),\"r\"(b),\"r\"(c)); return r; }\n"
// widening 32x32 -> 64 alias, no accumulator (SMULL/UMULL/SMNEGL/UMNEGL).
#define L2(op) \
  "unsigned long f(unsigned long a, unsigned long b){ unsigned long r;" \
  " __asm__(\"" op " %0,%w1,%w2\" : \"=r\"(r) : \"r\"(a),\"r\"(b)); return r; }\n"
// 64-bit fused multiply add/sub (4 operands).
#define M3(op) \
  "unsigned long f(unsigned long a, unsigned long b, unsigned long c){ unsigned long r;" \
  " __asm__(\"" op " %0,%1,%2,%3\" : \"=r\"(r) : \"r\"(a),\"r\"(b),\"r\"(c)); return r; }\n"

static const std::vector<RoundTripTC> kA64 = {
  // ===== SMULH / UMULH: signed vs unsigned high half diverge on bit63. =====
  // -1 * -1: signed high = 0 (product 1); unsigned high = 0xFFFFFFFFFFFFFFFE.
  {"a64_smulh_m1",   H2("smulh"), {0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL}, "MulHighLong", 1, ""},
  {"a64_umulh_m1",   H2("umulh"), {0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL}, "MulHighLong", 1, ""},
  // INT_MIN * INT_MIN: signed high = 0x4000000000000000.
  {"a64_smulh_imin", H2("smulh"), {0x8000000000000000ULL, 0x8000000000000000ULL}, "MulHighLong", 1, ""},
  {"a64_umulh_imin", H2("umulh"), {0x8000000000000000ULL, 0x8000000000000000ULL}, "MulHighLong", 1, ""},
  // negative * positive.
  {"a64_smulh_negpos", H2("smulh"), {0xFFFFFFFFFFFFFFF6ULL /*-10*/, 0x0000000100000000ULL}, "MulHighLong", 1, ""},
  {"a64_umulh_negpos", H2("umulh"), {0xFFFFFFFFFFFFFFF6ULL, 0x0000000100000000ULL}, "MulHighLong", 1, ""},
  // mixed large operands.
  {"a64_smulh_mix",  H2("smulh"), {0x123456789ABCDEF0ULL, 0xFEDCBA9876543210ULL}, "MulHighLong", 1, ""},
  {"a64_umulh_mix",  H2("umulh"), {0x123456789ABCDEF0ULL, 0xFEDCBA9876543210ULL}, "MulHighLong", 1, ""},

  // ===== SMADDL / UMADDL: Wn/Wm = low 32; garbage high bits must be ignored. =====
  // Wn=-5 (0xFFFFFFFB), Wm=1000, Xa=100 -> signed -4900; high garbage 0x1111.../0x2222...
  {"a64_smaddl_neg", L3("smaddl"), {0x11111111FFFFFFFBULL, 0x22222222000003E8ULL, 100}, "MulHighLong", 1, ""},
  // Wn=0xFFFFFFFF, Wm=0xFFFFFFFF, Xa=1 -> unsigned 0xFFFFFFFE00000002.
  {"a64_umaddl_max", L3("umaddl"), {0xAAAAAAAAFFFFFFFFULL, 0xBBBBBBBBFFFFFFFFULL, 1}, "MulHighLong", 1, ""},
  // SMSUBL: Xa - sext(Wn)*sext(Wm); Wn=-7, Wm=9, Xa=50 -> 50-(-63)=113.
  {"a64_smsubl_neg", L3("smsubl"), {0x12345678FFFFFFF9ULL /*-7*/, 0x9ULL, 50}, "MulHighLong", 1, ""},
  // UMSUBL: Xa - zext(Wn)*zext(Wm).
  {"a64_umsubl",     L3("umsubl"), {0xCAFEBABE00000007ULL, 0x9ULL, 1000}, "MulHighLong", 1, ""},

  // ===== SMULL/UMULL + SMNEGL/UMNEGL aliases (op_count==3, XZR). =====
  {"a64_smull_neg",  L2("smull"),  {0xDEAD0000FFFFFFFBULL /*-5*/, 0x3E8ULL /*1000*/}, "MulHighLong", 1, ""},
  {"a64_umull_max",  L2("umull"),  {0xFFFFFFFFULL, 0xFFFFFFFFULL}, "MulHighLong", 1, ""},
  {"a64_smnegl",     L2("smnegl"), {0xBEEF0000FFFFFFFBULL /*-5*/, 0x64ULL /*100*/}, "MulHighLong", 1, ""},
  {"a64_umnegl",     L2("umnegl"), {0x10ULL, 0x20ULL}, "MulHighLong", 1, ""},

  // ===== MADD / MSUB (64-bit fused). =====
  {"a64_madd", M3("madd"), {0xFFFFFFFFFFFFFFFBULL /*-5*/, 7, 100}, "MulHighLong", 1, ""},
  {"a64_msub", M3("msub"), {6, 7, 0x0000000100000000ULL}, "MulHighLong", 1, ""},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(MulHighLong, AArch64MulHighLongRT,
                         ::testing::ValuesIn(kA64), rtTCName);
