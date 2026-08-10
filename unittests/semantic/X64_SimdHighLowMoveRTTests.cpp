//===- X64_SimdHighLowMoveRTTests.cpp - MOVHPS/MOVLPS family ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// x86 partial-qword vector moves operate on ONE 64-bit half of an XMM and
// MUST preserve / select the correct other half:
//
//   MOVHPS/MOVHPD m64, xmm    store: m64 = xmm[127:64]   (HIGH qword)
//   MOVLPS/MOVLPD m64, xmm    store: m64 = xmm[63:0]     (LOW qword)
//   MOVHPS/MOVHPD xmm, m64    load:  xmm[127:64] = m64, xmm[63:0]  preserved
//   MOVLPS/MOVLPD xmm, m64    load:  xmm[63:0]  = m64, xmm[127:64] preserved
//   VMOVHPS/VMOVLPS (VEX)     3-op non-destructive load + 2-op store
//   VMOVLHPS xmm1,xmm2,xmm3   xmm1 = { xmm2[63:0],   xmm3[63:0]  }
//   VMOVHLPS xmm1,xmm2,xmm3   xmm1 = { xmm2[127:64], xmm3[127:64]}
//
// The lifter had a family of bugs here:
//   (a) Legacy MOVHPS/MOVHPD *store* form took SUBBYTES offset 0 (the LOW
//       qword) for every MOV{L,H}P{S,D} — so `movhps [mem],xmm` stored the
//       LOW half instead of the HIGH half.
//   (b) Legacy MOV{L,H}P{S,D} *load* form did a flat `COPY Dst, Src` of the
//       8-byte memory value into the 16-byte XMM, clobbering the half that the
//       instruction must PRESERVE.
//   (c) The VEX VMOV{L,H}P{S,D}/VMOVLHPS/VMOVHLPS handler did `COPY Dst, Src`
//       (Src = last operand) for everything: the load form dropped the merge
//       source, the store form silently dropped the memory write entirely
//       (operandWrite() returns a discarded ram(0) placeholder), and the
//       lane-select moves picked the wrong halves.
//
// These were masked the classic way: X64_SSEMovConvRTTests.cpp *claims* (in its
// header comment) to cover MOVHPD/MOVLPD but only ever exercises full-register
// MOVAPS/MOVUPS copies — the partial-qword forms had zero roundtrip coverage.
//
// Each probe seeds an XMM with distinct low/high qwords (or a sentinel memory
// cell) and folds both resulting halves into the return with position weights,
// so the wrong-half / dropped-store / lost-merge bugs all diverge (RED) while
// the fixed lifter matches the original (GREEN).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SimdHighLowMoveRT : public SemanticRoundTripFixture,
                             public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SimdHighLowMoveRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {

  // ===== Legacy store forms (RED: MOVHPS/MOVHPD stored the LOW half). =====
  {"movhps_store",
   "#include <immintrin.h>\n"
   "unsigned long f(unsigned long a){unsigned long lo=a*0x11u+1,hi=a*0x29u+2;\n"
   "  __m128i v=_mm_set_epi64x((long)hi,(long)lo);\n"
   "  unsigned long mem=0xC0FFEEu;\n"
   "  __asm__ volatile(\"movhps %1, %0\":\"=m\"(mem):\"x\"(v));\n"
   "  return mem;}\n",
   {42}, "HighLowMove", 1, "-msse2 -ffreestanding"},

  {"movhpd_store",
   "#include <immintrin.h>\n"
   "unsigned long f(unsigned long a){unsigned long lo=a*0x13u+3,hi=a*0x2Bu+4;\n"
   "  __m128i v=_mm_set_epi64x((long)hi,(long)lo);\n"
   "  unsigned long mem=0xC0FFEEu;\n"
   "  __asm__ volatile(\"movhpd %1, %0\":\"=m\"(mem):\"x\"(v));\n"
   "  return mem;}\n",
   {42}, "HighLowMove", 1, "-msse2 -ffreestanding"},

  // Low-store control forms (legacy path already correct).
  {"movlps_store_ctl",
   "#include <immintrin.h>\n"
   "unsigned long f(unsigned long a){unsigned long lo=a*0x11u+1,hi=a*0x29u+2;\n"
   "  __m128i v=_mm_set_epi64x((long)hi,(long)lo);\n"
   "  unsigned long mem=0xC0FFEEu;\n"
   "  __asm__ volatile(\"movlps %1, %0\":\"=m\"(mem):\"x\"(v));\n"
   "  return mem;}\n",
   {42}, "HighLowMove", 1, "-msse2 -ffreestanding"},

  {"movlpd_store_ctl",
   "#include <immintrin.h>\n"
   "unsigned long f(unsigned long a){unsigned long lo=a*0x13u+3,hi=a*0x2Bu+4;\n"
   "  __m128i v=_mm_set_epi64x((long)hi,(long)lo);\n"
   "  unsigned long mem=0xC0FFEEu;\n"
   "  __asm__ volatile(\"movlpd %1, %0\":\"=m\"(mem):\"x\"(v));\n"
   "  return mem;}\n",
   {42}, "HighLowMove", 1, "-msse2 -ffreestanding"},

  // ===== Legacy load forms (RED: flat COPY clobbered the preserved half). ===
  {"movhps_load",
   "#include <immintrin.h>\n"
   "unsigned long f(unsigned long a){unsigned long ls=a*0x13u+3,hs=a*0x37u+5;\n"
   "  __m128i v=_mm_set_epi64x((long)hs,(long)ls);\n"
   "  unsigned long mem=a*0x4Du+7;\n"
   "  __asm__ volatile(\"movhps %1, %0\":\"+x\"(v):\"m\"(mem));\n"
   "  unsigned long out[2]; _mm_storeu_si128((__m128i*)out,v);\n"
   "  return out[0]*7u+out[1]*13u;}\n",
   {42}, "HighLowMove", 1, "-msse2 -ffreestanding"},

  {"movlps_load",
   "#include <immintrin.h>\n"
   "unsigned long f(unsigned long a){unsigned long ls=a*0x13u+3,hs=a*0x37u+5;\n"
   "  __m128i v=_mm_set_epi64x((long)hs,(long)ls);\n"
   "  unsigned long mem=a*0x4Du+7;\n"
   "  __asm__ volatile(\"movlps %1, %0\":\"+x\"(v):\"m\"(mem));\n"
   "  unsigned long out[2]; _mm_storeu_si128((__m128i*)out,v);\n"
   "  return out[0]*7u+out[1]*13u;}\n",
   {42}, "HighLowMove", 1, "-msse2 -ffreestanding"},

  {"movhpd_load",
   "#include <immintrin.h>\n"
   "unsigned long f(unsigned long a){unsigned long ls=a*0x17u+9,hs=a*0x3Du+6;\n"
   "  __m128i v=_mm_set_epi64x((long)hs,(long)ls);\n"
   "  unsigned long mem=a*0x53u+8;\n"
   "  __asm__ volatile(\"movhpd %1, %0\":\"+x\"(v):\"m\"(mem));\n"
   "  unsigned long out[2]; _mm_storeu_si128((__m128i*)out,v);\n"
   "  return out[0]*7u+out[1]*13u;}\n",
   {42}, "HighLowMove", 1, "-msse2 -ffreestanding"},

  {"movlpd_load",
   "#include <immintrin.h>\n"
   "unsigned long f(unsigned long a){unsigned long ls=a*0x17u+9,hs=a*0x3Du+6;\n"
   "  __m128i v=_mm_set_epi64x((long)hs,(long)ls);\n"
   "  unsigned long mem=a*0x53u+8;\n"
   "  __asm__ volatile(\"movlpd %1, %0\":\"+x\"(v):\"m\"(mem));\n"
   "  unsigned long out[2]; _mm_storeu_si128((__m128i*)out,v);\n"
   "  return out[0]*7u+out[1]*13u;}\n",
   {42}, "HighLowMove", 1, "-msse2 -ffreestanding"},

  // ===== Legacy lane-select reg forms (guardrails; already correct). =====
  {"movlhps_ctl",
   "#include <immintrin.h>\n"
   "unsigned long f(unsigned long a){unsigned long dl=a*0x11u+1,dh=a*0x13u+2,\n"
   "    sl=a*0x17u+3,sh=a*0x1Du+4;\n"
   "  __m128i dst=_mm_set_epi64x((long)dh,(long)dl);\n"
   "  __m128i src=_mm_set_epi64x((long)sh,(long)sl);\n"
   "  __asm__ volatile(\"movlhps %1, %0\":\"+x\"(dst):\"x\"(src));\n"
   "  unsigned long out[2]; _mm_storeu_si128((__m128i*)out,dst);\n"
   "  return out[0]*7u+out[1]*13u;}\n",
   {42}, "HighLowMove", 1, "-msse2 -ffreestanding"},

  {"movhlps_ctl",
   "#include <immintrin.h>\n"
   "unsigned long f(unsigned long a){unsigned long dl=a*0x11u+1,dh=a*0x13u+2,\n"
   "    sl=a*0x17u+3,sh=a*0x1Du+4;\n"
   "  __m128i dst=_mm_set_epi64x((long)dh,(long)dl);\n"
   "  __m128i src=_mm_set_epi64x((long)sh,(long)sl);\n"
   "  __asm__ volatile(\"movhlps %1, %0\":\"+x\"(dst):\"x\"(src));\n"
   "  unsigned long out[2]; _mm_storeu_si128((__m128i*)out,dst);\n"
   "  return out[0]*7u+out[1]*13u;}\n",
   {42}, "HighLowMove", 1, "-msse2 -ffreestanding"},

  // ===== VEX store forms (RED: store silently dropped → ram(0)). =====
  {"vmovhps_store",
   "#include <immintrin.h>\n"
   "unsigned long f(unsigned long a){unsigned long lo=a*0x11u+1,hi=a*0x29u+2;\n"
   "  __m128i v=_mm_set_epi64x((long)hi,(long)lo);\n"
   "  unsigned long mem=0xC0FFEEu;\n"
   "  __asm__ volatile(\"vmovhps %1, %0\":\"=m\"(mem):\"x\"(v));\n"
   "  return mem;}\n",
   {42}, "HighLowMove", 1, "-mavx -ffreestanding"},

  {"vmovlps_store",
   "#include <immintrin.h>\n"
   "unsigned long f(unsigned long a){unsigned long lo=a*0x11u+1,hi=a*0x29u+2;\n"
   "  __m128i v=_mm_set_epi64x((long)hi,(long)lo);\n"
   "  unsigned long mem=0xC0FFEEu;\n"
   "  __asm__ volatile(\"vmovlps %1, %0\":\"=m\"(mem):\"x\"(v));\n"
   "  return mem;}\n",
   {42}, "HighLowMove", 1, "-mavx -ffreestanding"},

  // ===== VEX load forms (3-operand; RED: COPY dropped the merge source). ====
  {"vmovhps_load",
   "#include <immintrin.h>\n"
   "unsigned long f(unsigned long a){unsigned long s1l=a*0x13u+3,s1h=a*0x37u+5;\n"
   "  __m128i src1=_mm_set_epi64x((long)s1h,(long)s1l);\n"
   "  unsigned long mem=a*0x4Du+7;\n"
   "  __m128i dst=_mm_setzero_si128();\n"
   "  __asm__ volatile(\"vmovhps %2, %1, %0\":\"=x\"(dst):\"x\"(src1),\"m\"(mem));\n"
   "  unsigned long out[2]; _mm_storeu_si128((__m128i*)out,dst);\n"
   "  return out[0]*7u+out[1]*13u;}\n",
   {42}, "HighLowMove", 1, "-mavx -ffreestanding"},

  {"vmovlps_load",
   "#include <immintrin.h>\n"
   "unsigned long f(unsigned long a){unsigned long s1l=a*0x13u+3,s1h=a*0x37u+5;\n"
   "  __m128i src1=_mm_set_epi64x((long)s1h,(long)s1l);\n"
   "  unsigned long mem=a*0x4Du+7;\n"
   "  __m128i dst=_mm_setzero_si128();\n"
   "  __asm__ volatile(\"vmovlps %2, %1, %0\":\"=x\"(dst):\"x\"(src1),\"m\"(mem));\n"
   "  unsigned long out[2]; _mm_storeu_si128((__m128i*)out,dst);\n"
   "  return out[0]*7u+out[1]*13u;}\n",
   {42}, "HighLowMove", 1, "-mavx -ffreestanding"},

  // ===== VEX lane-select reg forms (3-operand; RED: wrong halves). =====
  {"vmovlhps",
   "#include <immintrin.h>\n"
   "unsigned long f(unsigned long a){unsigned long s1l=a*0x11u+1,s1h=a*0x13u+2,\n"
   "    s2l=a*0x17u+3,s2h=a*0x1Du+4;\n"
   "  __m128i s1=_mm_set_epi64x((long)s1h,(long)s1l);\n"
   "  __m128i s2=_mm_set_epi64x((long)s2h,(long)s2l);\n"
   "  __m128i dst=_mm_setzero_si128();\n"
   "  __asm__ volatile(\"vmovlhps %2, %1, %0\":\"=x\"(dst):\"x\"(s1),\"x\"(s2));\n"
   "  unsigned long out[2]; _mm_storeu_si128((__m128i*)out,dst);\n"
   "  return out[0]*7u+out[1]*13u;}\n",
   {42}, "HighLowMove", 1, "-mavx -ffreestanding"},

  {"vmovhlps",
   "#include <immintrin.h>\n"
   "unsigned long f(unsigned long a){unsigned long s1l=a*0x11u+1,s1h=a*0x13u+2,\n"
   "    s2l=a*0x17u+3,s2h=a*0x1Du+4;\n"
   "  __m128i s1=_mm_set_epi64x((long)s1h,(long)s1l);\n"
   "  __m128i s2=_mm_set_epi64x((long)s2h,(long)s2l);\n"
   "  __m128i dst=_mm_setzero_si128();\n"
   "  __asm__ volatile(\"vmovhlps %2, %1, %0\":\"=x\"(dst):\"x\"(s1),\"x\"(s2));\n"
   "  unsigned long out[2]; _mm_storeu_si128((__m128i*)out,dst);\n"
   "  return out[0]*7u+out[1]*13u;}\n",
   {42}, "HighLowMove", 1, "-mavx -ffreestanding"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(HighLowMove, X64SimdHighLowMoveRT,
                         ::testing::ValuesIn(kX64), rtTCName);
