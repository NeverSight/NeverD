//===- X64_SimdExtractMemRTTests.cpp - PEXTR*/EXTRACTPS mem store -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// x86 SIMD lane-extract instructions with a MEMORY destination must STORE the
// extracted element to memory:
//   PEXTRB/W/D/Q  r/m, xmm, imm    (SSE4.1; the PEXTRW r/m16 form is SSE4.1)
//   EXTRACTPS     r/m32, xmm, imm  (SSE4.1)
//   VPEXTRB/W/D/Q + VEXTRACTPS     (AVX VEX-encoded forms)
//
// The lifter wrote the extracted value into `operandWrite(operands[0])`, which
// for a MEMORY operand returns a discarded ram(0) placeholder instead of issuing
// a STORE (`storeToMem` was never called).  So `pextrd [mem],xmm,imm` and the
// whole family silently DROPPED the memory write and left the cell unchanged.
// The register-destination forms (the common `_mm_extract_*` idiom) were the
// only ones under coverage, so the memory forms had zero roundtrip coverage —
// the same class of weak-test masking as the x86 atomic mem-store gap.
//
// Each probe seeds the destination cell with a sentinel via "+m", extracts a
// lane into it, and folds the cell into the return; the dropped store leaves the
// sentinel (RED), while the fixed lifter writes the lane (GREEN, == original).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SimdExtractMemRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SimdExtractMemRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {

  // ===== SSE4.1 extract-to-memory (RED before fix: store dropped). =====
  {"pextrd_mem",
   "#include <immintrin.h>\n"
   "unsigned long f(unsigned a){unsigned mem=0xABCD1234u;\n"
   "  __m128i v=_mm_set_epi32((int)(a*0x111+1),(int)(a*0x55+2),(int)(a*0x303+3),(int)(a+4));\n"
   "  __asm__ volatile(\"pextrd $2,%1,%0\":\"+m\"(mem):\"x\"(v):);\n"
   "  return (unsigned long)mem*7ul+a*13ul;}\n",
   {42}, "SimdExtractMem", 1, "-msse4.1 -ffreestanding"},

  {"pextrq_mem",
   "#include <immintrin.h>\n"
   "unsigned long f(unsigned long a){unsigned long mem=0xABCDEF0123456789ul;\n"
   "  __m128i v=_mm_set_epi64x((long)(a*0x3003+1),(long)(a+2));\n"
   "  __asm__ volatile(\"pextrq $1,%1,%0\":\"+m\"(mem):\"x\"(v):);\n"
   "  return mem*7ul+a*13ul;}\n",
   {42}, "SimdExtractMem", 1, "-msse4.1 -ffreestanding"},

  {"pextrw_mem",
   "#include <immintrin.h>\n"
   "unsigned long f(unsigned a){unsigned short mem=0xBEEF;\n"
   "  __m128i v=_mm_set_epi32((int)(a*0x111+1),(int)(a*0x55+2),(int)(a*0x70003+3),(int)(a+4));\n"
   "  __asm__ volatile(\"pextrw $3,%1,%0\":\"+m\"(mem):\"x\"(v):);\n"
   "  return (unsigned long)mem*7ul+a*13ul;}\n",
   {42}, "SimdExtractMem", 1, "-msse4.1 -ffreestanding"},

  {"pextrb_mem",
   "#include <immintrin.h>\n"
   "unsigned long f(unsigned a){unsigned char mem=0xAB;\n"
   "  __m128i v=_mm_set_epi32((int)(a*0x111+1),(int)(a*0x4005+2),(int)(a*0x303+3),(int)(a+4));\n"
   "  __asm__ volatile(\"pextrb $5,%1,%0\":\"+m\"(mem):\"x\"(v):);\n"
   "  return (unsigned long)mem*7ul+a*13ul;}\n",
   {42}, "SimdExtractMem", 1, "-msse4.1 -ffreestanding"},

  {"extractps_mem",
   "#include <immintrin.h>\n"
   "unsigned long f(unsigned a){unsigned mem=0x99887766u;\n"
   "  __m128 v=_mm_castsi128_ps(_mm_set_epi32((int)(a*0x111+1),(int)(a*0x55+2),(int)(a*0x303+3),(int)(a+4)));\n"
   "  __asm__ volatile(\"extractps $2,%1,%0\":\"+m\"(mem):\"x\"(v):);\n"
   "  return (unsigned long)mem*7ul+a*13ul;}\n",
   {42}, "SimdExtractMem", 1, "-msse4.1 -ffreestanding"},

  // ===== AVX VEX extract-to-memory. =====
  {"vpextrd_mem",
   "#include <immintrin.h>\n"
   "unsigned long f(unsigned a){unsigned mem=0xABCD1234u;\n"
   "  __m128i v=_mm_set_epi32((int)(a*0x111+1),(int)(a*0x55+2),(int)(a*0x303+3),(int)(a+4));\n"
   "  __asm__ volatile(\"vpextrd $2,%1,%0\":\"+m\"(mem):\"x\"(v):);\n"
   "  return (unsigned long)mem*7ul+a*13ul;}\n",
   {42}, "SimdExtractMem", 1, "-mavx -ffreestanding"},

  {"vpextrq_mem",
   "#include <immintrin.h>\n"
   "unsigned long f(unsigned long a){unsigned long mem=0xABCDEF0123456789ul;\n"
   "  __m128i v=_mm_set_epi64x((long)(a*0x3003+1),(long)(a+2));\n"
   "  __asm__ volatile(\"vpextrq $1,%1,%0\":\"+m\"(mem):\"x\"(v):);\n"
   "  return mem*7ul+a*13ul;}\n",
   {42}, "SimdExtractMem", 1, "-mavx -ffreestanding"},

  {"vpextrw_mem",
   "#include <immintrin.h>\n"
   "unsigned long f(unsigned a){unsigned short mem=0xBEEF;\n"
   "  __m128i v=_mm_set_epi32((int)(a*0x111+1),(int)(a*0x55+2),(int)(a*0x70003+3),(int)(a+4));\n"
   "  __asm__ volatile(\"vpextrw $3,%1,%0\":\"+m\"(mem):\"x\"(v):);\n"
   "  return (unsigned long)mem*7ul+a*13ul;}\n",
   {42}, "SimdExtractMem", 1, "-mavx -ffreestanding"},

  {"vpextrb_mem",
   "#include <immintrin.h>\n"
   "unsigned long f(unsigned a){unsigned char mem=0xAB;\n"
   "  __m128i v=_mm_set_epi32((int)(a*0x111+1),(int)(a*0x4005+2),(int)(a*0x303+3),(int)(a+4));\n"
   "  __asm__ volatile(\"vpextrb $5,%1,%0\":\"+m\"(mem):\"x\"(v):);\n"
   "  return (unsigned long)mem*7ul+a*13ul;}\n",
   {42}, "SimdExtractMem", 1, "-mavx -ffreestanding"},

  {"vextractps_mem",
   "#include <immintrin.h>\n"
   "unsigned long f(unsigned a){unsigned mem=0x99887766u;\n"
   "  __m128 v=_mm_castsi128_ps(_mm_set_epi32((int)(a*0x111+1),(int)(a*0x55+2),(int)(a*0x303+3),(int)(a+4)));\n"
   "  __asm__ volatile(\"vextractps $1,%1,%0\":\"+m\"(mem):\"x\"(v):);\n"
   "  return (unsigned long)mem*7ul+a*13ul;}\n",
   {42}, "SimdExtractMem", 1, "-mavx -ffreestanding"},

  // ===== Register-destination controls (guardrail; already correct). =====
  {"pextrd_reg",
   "#include <immintrin.h>\n"
   "unsigned long f(unsigned a){unsigned r;\n"
   "  __m128i v=_mm_set_epi32((int)(a*0x111+1),(int)(a*0x55+2),(int)(a*0x303+3),(int)(a+4));\n"
   "  __asm__ volatile(\"pextrd $2,%1,%0\":\"=r\"(r):\"x\"(v):);\n"
   "  return (unsigned long)r*7ul+a*13ul;}\n",
   {42}, "SimdExtractMem", 1, "-msse4.1 -ffreestanding"},

  {"extractps_reg",
   "#include <immintrin.h>\n"
   "unsigned long f(unsigned a){unsigned r;\n"
   "  __m128 v=_mm_castsi128_ps(_mm_set_epi32((int)(a*0x111+1),(int)(a*0x55+2),(int)(a*0x303+3),(int)(a+4)));\n"
   "  __asm__ volatile(\"extractps $2,%1,%0\":\"=r\"(r):\"x\"(v):);\n"
   "  return (unsigned long)r*7ul+a*13ul;}\n",
   {42}, "SimdExtractMem", 1, "-msse4.1 -ffreestanding"},

  // PEXTRB/W to a 32-bit GPR must zero-extend the 1/2-byte element (the byte/
  // word forms must not leak the neighbouring lanes into the upper GPR bytes).
  {"pextrb_reg",
   "#include <immintrin.h>\n"
   "unsigned long f(unsigned a){unsigned r;\n"
   "  __m128i v=_mm_set_epi32((int)(a*0x111+1),(int)(a*0x4005+2),(int)(a*0x303+3),(int)(a+4));\n"
   "  __asm__ volatile(\"pextrb $5,%1,%0\":\"=r\"(r):\"x\"(v):);\n"
   "  return (unsigned long)r*7ul+a*13ul;}\n",
   {42}, "SimdExtractMem", 1, "-msse4.1 -ffreestanding"},

  {"pextrw_reg",
   "#include <immintrin.h>\n"
   "unsigned long f(unsigned a){unsigned r;\n"
   "  __m128i v=_mm_set_epi32((int)(a*0x111+1),(int)(a*0x55+2),(int)(a*0x70003+3),(int)(a+4));\n"
   "  __asm__ volatile(\"pextrw $3,%1,%0\":\"=r\"(r):\"x\"(v):);\n"
   "  return (unsigned long)r*7ul+a*13ul;}\n",
   {42}, "SimdExtractMem", 1, "-msse4.1 -ffreestanding"},

  {"vpextrb_reg",
   "#include <immintrin.h>\n"
   "unsigned long f(unsigned a){unsigned r;\n"
   "  __m128i v=_mm_set_epi32((int)(a*0x111+1),(int)(a*0x4005+2),(int)(a*0x303+3),(int)(a+4));\n"
   "  __asm__ volatile(\"vpextrb $5,%1,%0\":\"=r\"(r):\"x\"(v):);\n"
   "  return (unsigned long)r*7ul+a*13ul;}\n",
   {42}, "SimdExtractMem", 1, "-mavx -ffreestanding"},

  {"vpextrw_reg",
   "#include <immintrin.h>\n"
   "unsigned long f(unsigned a){unsigned r;\n"
   "  __m128i v=_mm_set_epi32((int)(a*0x111+1),(int)(a*0x55+2),(int)(a*0x70003+3),(int)(a+4));\n"
   "  __asm__ volatile(\"vpextrw $3,%1,%0\":\"=r\"(r):\"x\"(v):);\n"
   "  return (unsigned long)r*7ul+a*13ul;}\n",
   {42}, "SimdExtractMem", 1, "-mavx -ffreestanding"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(SimdExtractMem, X64SimdExtractMemRT,
                         ::testing::ValuesIn(kX64), rtTCName);
