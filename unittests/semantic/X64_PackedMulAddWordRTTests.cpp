//===- X64_PackedMulAddWordRTTests.cpp - PMADDWD/VPMADDWD lane RT -*- C++ -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// PMADDWD (SSE2) / VPMADDWD (VEX.128) multiply-add of signed 16-bit words:
//
//   dword[i] = SInt(a[2i])*SInt(b[2i]) + SInt(a[2i+1])*SInt(b[2i+1])
//
// The product+sum is computed at 32 bits and WRAPS (PMADDWD never saturates).
// Three correctness aspects are classic weak-test blind spots that the existing
// probes (`pmaddwd_basic` and `pmaddwd_simple` both read ONLY lane r[0], with
// an all-ones or all-zero second operand) leave open:
//
//   1. ALL FOUR OUTPUT LANES.  Each dword is the reduction of its OWN word pair;
//      a lifter that mis-routed a lane, swapped the pair order, or read the
//      wrong word offset would still match a lane-0-only probe.  Here every
//      lane uses distinct word values and all four are folded into the return.
//
//   2. SIGNEDNESS.  The words are SIGN-extended before the multiply.  Probes
//      drive negative*negative, negative*positive and the 0x8000 (INT16_MIN)
//      corner so a zero-extend bug (which only diverges when a word's top bit is
//      set) is observable.
//
//   3. 32-BIT WRAP, NO SATURATION.  Two 0x8000*0x8000 products sum to
//      0x40000000+0x40000000 = 0x80000000 (INT32_MIN) — the unsaturated wrap.
//      A handler that clamped (PMADDWD has no saturating sibling; that is
//      PMADDUBSW) would diverge here.
//
// All 16 input words come from FOUR 64-bit args (va<-a|b, vb<-c|d) so clang
// cannot constant-fold the vectors.  Covered for both the legacy SSE2 encoding
// (`-mno-avx`) and the VEX.128 form (`-mavx2`, which still roundtrips — only the
// 256-bit ymm VEX forms are undecodable by the bundled Unicorn).  The handler
// is believed correct (per-lane SUBBYTES+SEXT+INT_MULT+INT_ADD); this is a
// regression-locking guardrail and the oracle compares original-Unicorn vs
// lifted-Unicorn (no hand-computed expected).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64PackedMulAddWordRT : public SemanticRoundTripFixture,
                              public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64PackedMulAddWordRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
// va words = {a.w0,a.w1,a.w2,a.w3, b.w0,b.w1,b.w2,b.w3}
// vb words = {c.w0,c.w1,c.w2,c.w3, d.w0,d.w1,d.w2,d.w3}
// lane0 = a.w0*c.w0 + a.w1*c.w1 ; lane1 = a.w2*c.w2 + a.w3*c.w3 ;
// lane2 = b.w0*d.w0 + b.w1*d.w1 ; lane3 = b.w2*d.w2 + b.w3*d.w3 .
#define MADDWD_FN(BUILTIN) \
  "typedef short v8s __attribute__((vector_size(16)));\n" \
  "typedef int   v4i __attribute__((vector_size(16)));\n" \
  "unsigned long f(unsigned long a, unsigned long b,\n" \
  "               unsigned long c, unsigned long d){\n" \
  "  v8s va={(short)a,(short)(a>>16),(short)(a>>32),(short)(a>>48),\n" \
  "          (short)b,(short)(b>>16),(short)(b>>32),(short)(b>>48)};\n" \
  "  v8s vb={(short)c,(short)(c>>16),(short)(c>>32),(short)(c>>48),\n" \
  "          (short)d,(short)(d>>16),(short)(d>>32),(short)(d>>48)};\n" \
  "  v4i vr=" BUILTIN "(va,vb);\n" \
  "  return (unsigned long)(unsigned)vr[0]*1000003ul\n" \
  "       + (unsigned long)(unsigned)vr[1]*9999991ul\n" \
  "       + (unsigned long)(unsigned)vr[2]*7919ul\n" \
  "       + (unsigned long)(unsigned)vr[3]*104729ul;}\n"

static const std::vector<RoundTripTC> kX64 = {
  // ===== PMADDWD mixed signs across all four lanes. =====
  {"pmaddwd_mixed", MADDWD_FN("__builtin_ia32_pmaddwd128"),
   {0x0001FFFF7FFF8000ULL /*a: 0x8000,0x7FFF,0xFFFF,0x0001*/,
    0x1234FEDC00038001ULL /*b*/,
    0x00027FFFFFFE0003ULL /*c*/,
    0xABCD0005FF0C0100ULL /*d*/},
   "PackedMulAddWord", 1, "-msse2 -mno-avx"},

  // ===== PMADDWD INT16_MIN*INT16_MIN wrap to INT32_MIN in lane0. =====
  // a.w0=a.w1=0x8000, c.w0=c.w1=0x8000 -> lane0 = 2*0x40000000 = 0x80000000.
  {"pmaddwd_wrap_intmin", MADDWD_FN("__builtin_ia32_pmaddwd128"),
   {0x1111222280008000ULL, 0x3333444455556666ULL,
    0xAAAABBBB80008000ULL, 0x7777888899990000ULL},
   "PackedMulAddWord", 1, "-msse2 -mno-avx"},

  // ===== PMADDWD all-negative words (neg*neg => positive products). =====
  {"pmaddwd_allneg", MADDWD_FN("__builtin_ia32_pmaddwd128"),
   {0xFFFFFFFEFFFDFFFCULL, 0x8001800280038004ULL,
    0xFF00FE00FD00FC00ULL, 0x8000FFFF8000FFFFULL},
   "PackedMulAddWord", 1, "-msse2 -mno-avx"},

  // ===== VPMADDWD (VEX.128) — same probes through the VEX decode/emit path. ===
  {"vpmaddwd_mixed", MADDWD_FN("__builtin_ia32_pmaddwd128"),
   {0x0001FFFF7FFF8000ULL, 0x1234FEDC00038001ULL,
    0x00027FFFFFFE0003ULL, 0xABCD0005FF0C0100ULL},
   "PackedMulAddWord", 1, "-mavx2"},

  {"vpmaddwd_wrap_intmin", MADDWD_FN("__builtin_ia32_pmaddwd128"),
   {0x1111222280008000ULL, 0x3333444455556666ULL,
    0xAAAABBBB80008000ULL, 0x7777888899990000ULL},
   "PackedMulAddWord", 1, "-mavx2"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(PackedMulAddWord, X64PackedMulAddWordRT,
                         ::testing::ValuesIn(kX64), rtTCName);
