//===- X64_PackedMulHighRTTests.cpp - PMULHW/PMULHUW high-half RT -*- C++ -===//
//
// PMULHW (SSE2, signed) / PMULHUW (SSE2, unsigned) multiply each pair of 16-bit
// word lanes and write the HIGH 16 bits of the 32-bit product back per lane:
//
//   PMULHW :  dst[i] = (SInt16(a[i]) * SInt16(b[i])) >> 16      (sign-extend)
//   PMULHUW:  dst[i] = (UInt16(a[i]) * UInt16(b[i])) >> 16      (zero-extend)
//
// The ONLY difference between the two is the EXTENSION used before the multiply
// (sign vs zero), which only changes the high word when an operand's top bit is
// set.  That makes signedness a classic weak-test blind spot:
//
//   1. SIGNEDNESS.  A handler that zero-extended PMULHW (or sign-extended
//      PMULHUW) would still match for small positive inputs.  These probes drive
//      negative*negative (-1 * -1 -> high 0 signed vs 0xFFFE unsigned), the
//      0x8000 (INT16_MIN) corner, and negative*positive so the wrong extension
//      is observable in the folded return.
//
//   2. ALL EIGHT LANES.  Each output word is the high half of its OWN word pair;
//      a mis-routed / swapped / wrong-offset lane would survive a lane-0-only
//      probe.  Every one of the eight result words is folded into the return.
//
//   3. HIGH HALF, not low.  PMULH* keep bits [31:16] of the product (PMULLW
//      keeps the low half).  A handler taking the wrong SUBBYTES offset diverges
//      whenever the product's low and high words differ.
//
// Coverage gap motivating this round: the unsigned PMULHUW had ZERO roundtrip
// coverage, and PMULHW was only exercised indirectly (a C-level `(x*y)>>16` in
// AllPlatform_VectorAlgo5 that clang may lower to smull+shrn rather than a
// guaranteed pmulhw).  PMULHRSW (rounding) and PMADDWD (the adjacent word
// multiply-add family) already have dedicated probes; this fills the plain
// high-half signed/unsigned sibling.
//
// All sixteen input words come from FOUR 64-bit args (va<-a|b, vb<-c|d) so clang
// cannot constant-fold the vectors.  Covered for both the legacy SSE2 encoding
// (`-msse2 -mno-avx`) and the VEX.128 form (`-mavx2`, which still roundtrips —
// only the 256-bit ymm VEX forms are undecodable by the bundled Unicorn).  The
// handler is believed correct (per-lane SUBBYTES + SEXT/ZEXT + INT_MULT + take
// high SUBBYTES); this is a regression-locking guardrail and the oracle compares
// original-Unicorn vs lifted-Unicorn (no hand-computed expected).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64PackedMulHighRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64PackedMulHighRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
// va words = {a.w0,a.w1,a.w2,a.w3, b.w0,b.w1,b.w2,b.w3}
// vb words = {c.w0,c.w1,c.w2,c.w3, d.w0,d.w1,d.w2,d.w3}
// out[i]   = high16( ext(va[i]) * ext(vb[i]) ) ; all eight folded into the return.
#define MULH_FN(BUILTIN) \
  "typedef short v8s __attribute__((vector_size(16)));\n" \
  "unsigned long f(unsigned long a, unsigned long b,\n" \
  "               unsigned long c, unsigned long d){\n" \
  "  v8s va={(short)a,(short)(a>>16),(short)(a>>32),(short)(a>>48),\n" \
  "          (short)b,(short)(b>>16),(short)(b>>32),(short)(b>>48)};\n" \
  "  v8s vb={(short)c,(short)(c>>16),(short)(c>>32),(short)(c>>48),\n" \
  "          (short)d,(short)(d>>16),(short)(d>>32),(short)(d>>48)};\n" \
  "  v8s vr=" BUILTIN "(va,vb);\n" \
  "  unsigned s=0; for(int i=0;i<8;i++) s=s*31u+(unsigned short)vr[i];\n" \
  "  return (unsigned long)s;}\n"

static const std::vector<RoundTripTC> kX64 = {
  // ===== PMULHW — signed: mixed signs across all eight lanes. =====
  // words include 0x8000(INT16_MIN), 0x7FFF, 0xFFFF(-1) so sign-extend matters.
  {"pmulhw_mixed", MULH_FN("__builtin_ia32_pmulhw128"),
   {0x80007FFFFFFF0002ULL, 0x1234FEDC00038001ULL,
    0x00027FFFFFFE0003ULL, 0xABCD0005FF0C0100ULL},
   "PackedMulHigh", 1, "-msse2 -mno-avx"},

  // ===== PMULHW — neg*neg (=> positive products) and the INT16_MIN^2 corner. =====
  // 0x8000*0x8000 = +0x40000000 -> high 0x4000 (same under both extensions, a
  // deliberate non-diverging lane); 0xFFFF*0xFFFF = +1 -> high 0x0000.
  {"pmulhw_negneg", MULH_FN("__builtin_ia32_pmulhw128"),
   {0xFFFFFFFE80008000ULL, 0x8001800280038004ULL,
    0xFFFF800180008002ULL, 0x8000FFFF8000FFFFULL},
   "PackedMulHigh", 1, "-msse2 -mno-avx"},

  // ===== PMULHUW — unsigned: SAME data as pmulhw_mixed.  High-bit operands make
  //       the zero-extend diverge from sign-extend (e.g. 0xFFFF*2 -> high 0x0001
  //       unsigned vs 0xFFFF signed). =====
  {"pmulhuw_highbit", MULH_FN("__builtin_ia32_pmulhuw128"),
   {0x80007FFFFFFF0002ULL, 0x1234FEDC00038001ULL,
    0x00027FFFFFFE0003ULL, 0xABCD0005FF0C0100ULL},
   "PackedMulHigh", 1, "-msse2 -mno-avx"},

  // ===== PMULHUW — large unsigned products across the upper range. =====
  // 0xFFFF*0xFFFF = 0xFFFE0001 -> high 0xFFFE (the all-ones unsigned corner).
  {"pmulhuw_wide", MULH_FN("__builtin_ia32_pmulhuw128"),
   {0xFFFFFFFEFFFDFFFCULL, 0x8001800280038004ULL,
    0xFF00FE00FD00FC00ULL, 0xFFFFC000FFFF8000ULL},
   "PackedMulHigh", 1, "-msse2 -mno-avx"},

  // ===== VPMULHW (VEX.128) — same signed probe through the VEX decode/emit path. =
  {"vpmulhw_mixed", MULH_FN("__builtin_ia32_pmulhw128"),
   {0x80007FFFFFFF0002ULL, 0x1234FEDC00038001ULL,
    0x00027FFFFFFE0003ULL, 0xABCD0005FF0C0100ULL},
   "PackedMulHigh", 1, "-mavx2"},

  // ===== VPMULHUW (VEX.128) — same unsigned high-bit probe through VEX. =====
  {"vpmulhuw_highbit", MULH_FN("__builtin_ia32_pmulhuw128"),
   {0x80007FFFFFFF0002ULL, 0x1234FEDC00038001ULL,
    0x00027FFFFFFE0003ULL, 0xABCD0005FF0C0100ULL},
   "PackedMulHigh", 1, "-mavx2"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(PackedMulHigh, X64PackedMulHighRT,
                         ::testing::ValuesIn(kX64), rtTCName);
