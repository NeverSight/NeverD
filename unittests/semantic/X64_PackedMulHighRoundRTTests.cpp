//===- X64_PackedMulHighRoundRTTests.cpp - PMULHW/UW/RSW lane RT -*- C++ -===//
//
// x86 packed "multiply, keep the HIGH word" family — all per-16-bit-lane:
//
//   PMULHW   (SSE2)  : signed   a*b, result = (a*b)[31:16]
//   PMULHUW  (SSE2)  : unsigned a*b, result = (a*b)[31:16]
//   PMULHRSW (SSSE3) : signed Q15 rounding multiply, ((a*b + 0x4000) >> 15)[15:0]
//
// Three correctness aspects are classic "weak-test masking" blind spots that the
// pre-existing coverage leaves open:
//
//   1. SIGNEDNESS.  PMULHW sign-extends each word, PMULHUW zero-extends.  The two
//      diverge only when an operand's top bit is set (e.g. 0xFFFF*0xFFFF: signed
//      (-1*-1)=1 -> high 0x0000, but unsigned 65535*65535=0xFFFE0001 -> high
//      0xFFFE).  A test using only small positive lanes can't tell the two apart.
//
//   2. PER-LANE ROUTING.  The lifter builds the 128-bit result one 64-bit half at
//      a time (BuildHalf(0)/BuildHalf(8) + CONCAT).  Reading a SINGLE result lane
//      (the `_mm_extract_epi16(r,0)` idiom that the existing PMULHRSW/PMULHW
//      probes use) would not notice a transposed lane, a wrong half-merge, or a
//      bad SUBBYTES offset on lanes 1..7.  Every probe here folds ALL EIGHT
//      result words into the return via a hash, so any mis-routed lane shows.
//
//   3. PMULHRSW ROUNDING / SCALE CORNER.  result = (a*b + 0x4000) >> 15 must match
//      the spec's ((a*b >> 14) + 1) >> 1, including the saturating corner
//      0x8000*0x8000 = 0x40000000 -> 0x8000 and the +0x4000 round-up carry; a
//      missing round bias or a logical (vs arithmetic) shift would corrupt the
//      negative lanes.
//
// All 16 input words are seeded at runtime from FOUR 64-bit arguments (va from
// a|b, vb from c|d), so clang cannot constant-fold the vectors away and the exact
// corner bit-patterns (0x8000/0xFFFF/0x7FFF/round-boundary) are forced into known
// lanes.  The handlers are believed correct (cf. #155 PALIGNR-driven maddubs),
// so this is a regression-locking guardrail round; the oracle compares the
// original program's Unicorn result against the lifted+recompiled one, so no
// expected value is hand-computed.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64PackedMulHighRoundRT : public SemanticRoundTripFixture,
                                public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64PackedMulHighRoundRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
#define V8H "typedef short v8h __attribute__((vector_size(16)));\n"

// va <- {a, b} (8 words), vb <- {c, d} (8 words); fold all 8 result words.
#define MULHI_FN(BUILTIN) \
  V8H \
  "unsigned long f(unsigned long a, unsigned long b,\n" \
  "               unsigned long c, unsigned long d){\n" \
  "  v8h va={(short)a,(short)(a>>16),(short)(a>>32),(short)(a>>48),\n" \
  "          (short)b,(short)(b>>16),(short)(b>>32),(short)(b>>48)};\n" \
  "  v8h vc={(short)c,(short)(c>>16),(short)(c>>32),(short)(c>>48),\n" \
  "          (short)d,(short)(d>>16),(short)(d>>32),(short)(d>>48)};\n" \
  "  v8h vr=" BUILTIN "(va,vc);\n" \
  "  unsigned long h=0; for(int i=0;i<8;i++) h=h*131u+(unsigned short)vr[i];\n" \
  "  return h;}\n"

// Corner-rich operand quad: the per-lane (va,vb) pairs are
//   lane0 0x8000*0x8000  lane1 0xFFFF*0xFFFF  lane2 0x7FFF*0x7FFF
//   lane3 0x8000*0x7FFF   lane4 0x1234*0xEDCB  lane5 0x0001*0x8000
//   lane6 0x4000*0x0002   lane7 0xABCD*0x5555
static const std::vector<RoundTripTC> kX64 = {
  // ===== PMULHW — signed high word, all lanes folded. =====
  {"pmulhw_corner",  MULHI_FN("__builtin_ia32_pmulhw128"),
   {0x80007FFFFFFF8000ULL, 0xABCD400000011234ULL,
    0x7FFF7FFFFFFF8000ULL, 0x5555000280000EDCULL},
   "PackedMulHighRound", 1, "-mssse3"},
  {"pmulhw_mix",     MULHI_FN("__builtin_ia32_pmulhw128"),
   {0x0001FFFF8001FFFEULL, 0x7FFE8000C0007001ULL,
    0xFFFF80007FFF0002ULL, 0x8000AAAA5555FFFFULL},
   "PackedMulHighRound", 1, "-mssse3"},

  // ===== PMULHUW — unsigned high word; same inputs expose signed/unsigned split.
  {"pmulhuw_corner", MULHI_FN("__builtin_ia32_pmulhuw128"),
   {0x80007FFFFFFF8000ULL, 0xABCD400000011234ULL,
    0x7FFF7FFFFFFF8000ULL, 0x5555000280000EDCULL},
   "PackedMulHighRound", 1, "-mssse3"},
  {"pmulhuw_mix",    MULHI_FN("__builtin_ia32_pmulhuw128"),
   {0x0001FFFF8001FFFEULL, 0x7FFE8000C0007001ULL,
    0xFFFF80007FFF0002ULL, 0x8000AAAA5555FFFFULL},
   "PackedMulHighRound", 1, "-mssse3"},

  // ===== PMULHRSW — Q15 rounding multiply; corners + round-up carry. =====
  {"pmulhrsw_corner", MULHI_FN("__builtin_ia32_pmulhrsw128"),
   {0x80007FFFFFFF8000ULL, 0xABCD400000011234ULL,
    0x7FFF7FFFFFFF8000ULL, 0x5555000280000EDCULL},
   "PackedMulHighRound", 1, "-mssse3"},
  {"pmulhrsw_mix",    MULHI_FN("__builtin_ia32_pmulhrsw128"),
   {0x0001FFFF8001FFFEULL, 0x7FFE8000C0007001ULL,
    0xFFFF80007FFF0002ULL, 0x8000AAAA5555FFFFULL},
   "PackedMulHighRound", 1, "-mssse3"},
  // Round-boundary lanes: products just below/at a 0x8000-step so the +0x4000
  // bias decides the result word (carry vs no-carry, and sign of the fill).
  {"pmulhrsw_round",  MULHI_FN("__builtin_ia32_pmulhrsw128"),
   {0x4000400040004000ULL, 0xC0004001BFFF3FFFULL,
    0x0002000200020002ULL, 0xFFFE0002FFFE8000ULL},
   "PackedMulHighRound", 1, "-mssse3"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(PackedMulHighRound, X64PackedMulHighRoundRT,
                         ::testing::ValuesIn(kX64), rtTCName);
