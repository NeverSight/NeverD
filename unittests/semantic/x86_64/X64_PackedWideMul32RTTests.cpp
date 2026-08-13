//===- X64_PackedWideMul32RTTests.cpp - PMULDQ/PMULUDQ lane RT --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// x86 32->64 widening packed multiplies:
//
//   PMULDQ  (SSE4.1) : signed   dword[0]*dword[0] and dword[2]*dword[2]
//   PMULUDQ (SSE2)   : unsigned dword[0]*dword[0] and dword[2]*dword[2]
//
// Each 64-bit output lane is the product of the EVEN (low) dword of that qword
// lane from dst and src, sign- (PMULDQ) or zero- (PMULUDQ) extended to 64 bits.
// Three correctness aspects are weak-test blind spots that the existing probes
// (`pmuldq_simple` reads ONLY vr[0]; `pmuludq_packed` zeroes the odd dwords)
// leave open:
//
//   1. THE ODD DWORDS (indices 1 and 3) MUST BE IGNORED.  A lifter that read the
//      wrong dword offset, or did a full-width i128 multiply, would fold them in.
//      Here the odd dwords are seeded with NON-ZERO sentinels, so using them at
//      all changes the result.
//
//   2. SIGNEDNESS.  PMULDQ sign-extends each dword, PMULUDQ zero-extends; they
//      diverge whenever an even dword has its top bit set (e.g. -3 vs 0xFFFFFFFD).
//      Probes drive negative*negative, negative*positive and the 0x80000000
//      (INT_MIN) and 0xFFFFFFFF corners so the SEXT/ZEXT choice is observable.
//
//   3. BOTH OUTPUT LANES.  vr[0] (from dword 0) and vr[1] (from dword 2) are BOTH
//      folded into the return, so a transposed/merged lane shows.
//
// All 8 input dwords come from FOUR 64-bit args (va<-a|b, vb<-c|d) so clang can't
// constant-fold the vectors.  Pinned to the legacy SSE (non-VEX) encodings with
// `-mno-avx`.  The handler is believed correct (per-lane SUBBYTES+SEXT/ZEXT+
// INT_MULT, #133-era); this is a regression-locking guardrail round and the
// oracle compares original-Unicorn vs lifted-Unicorn (no hand-computed expected).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64PackedWideMul32RT : public SemanticRoundTripFixture,
                             public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64PackedWideMul32RT, Verify) { roundTripX64(GetParam()); }

// clang-format off
// va dwords = {a.lo, a.hi, b.lo, b.hi}; vb dwords = {c.lo, c.hi, d.lo, d.hi}.
// a.hi/b.hi and c.hi/d.hi are the ODD dwords that must be ignored.
#define WIDEMUL_FN(BUILTIN) \
  "typedef int v4i __attribute__((vector_size(16)));\n" \
  "typedef long long v2q __attribute__((vector_size(16)));\n" \
  "unsigned long f(unsigned long a, unsigned long b,\n" \
  "               unsigned long c, unsigned long d){\n" \
  "  v4i va={(int)a,(int)(a>>32),(int)b,(int)(b>>32)};\n" \
  "  v4i vb={(int)c,(int)(c>>32),(int)d,(int)(d>>32)};\n" \
  "  v2q vr=" BUILTIN "(va,vb);\n" \
  "  return (unsigned long)vr[0]*1000003ul + (unsigned long)vr[1]*9999991ul;}\n"

static const std::vector<RoundTripTC> kX64 = {
  // even dwords: lane0 = a.lo * c.lo, lane1 = b.lo * d.lo.
  // odd  dwords: a.hi/b.hi/c.hi/d.hi are non-zero sentinels (must be ignored).

  // ===== PMULDQ (signed): neg*neg (lane0) and neg*pos (lane1). =====
  {"pmuldq_signs",  WIDEMUL_FN("__builtin_ia32_pmuldq128"),
   {0x7FFFFFFFFFFFFFFDULL /*a: lo=-3, hi=0x7FFFFFFF*/,
    0x11111111FFFFFFFBULL /*b: lo=-5, hi=0x11111111*/,
    0xAAAAAAAA00000005ULL /*c: lo=5,  hi=0xAAAAAAAA*/,
    0x2222222200000007ULL /*d: lo=7,  hi=0x22222222*/},
   "PackedWideMul32", 1, "-msse4.1 -mno-avx"},

  // ===== PMULDQ corners: INT_MIN*INT_MIN, -1*INT_MIN. =====
  {"pmuldq_intmin", WIDEMUL_FN("__builtin_ia32_pmuldq128"),
   {0xDEADBEEF80000000ULL /*a.lo=INT_MIN*/, 0xCAFEF00DFFFFFFFFULL /*b.lo=-1*/,
    0x1234567880000000ULL /*c.lo=INT_MIN*/, 0x9ABCDEF080000000ULL /*d.lo=INT_MIN*/},
   "PackedWideMul32", 1, "-msse4.1 -mno-avx"},

  // ===== PMULUDQ (unsigned): same hi-bit dwords now mean huge positives. =====
  {"pmuludq_signs", WIDEMUL_FN("__builtin_ia32_pmuludq128"),
   {0x7FFFFFFFFFFFFFFDULL, 0x11111111FFFFFFFBULL,
    0xAAAAAAAA00000005ULL, 0x2222222200000007ULL},
   "PackedWideMul32", 1, "-msse2 -mno-avx"},

  // ===== PMULUDQ corner: 0xFFFFFFFF*0xFFFFFFFF = 0xFFFFFFFE00000001. =====
  {"pmuludq_max",   WIDEMUL_FN("__builtin_ia32_pmuludq128"),
   {0x00000000FFFFFFFFULL, 0xABCDEF01FFFFFFFEULL,
    0x55555555FFFFFFFFULL, 0x76543210FFFFFFFFULL},
   "PackedWideMul32", 1, "-msse2 -mno-avx"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(PackedWideMul32, X64PackedWideMul32RT,
                         ::testing::ValuesIn(kX64), rtTCName);
