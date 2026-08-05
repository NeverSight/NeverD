//===- X64_SSEIntProbeRTTests.cpp - SSSE3/SSE4.1 integer ---------*- C++ -*-===//
//
// Roundtrip probes for x86 SSSE3/SSE4.1 packed-integer intrinsics that clang
// -O2 rarely selects but that exercise distinct lifter paths:
//   * PMULHRSW (rounding mul high), PMADDUBSW (unsigned*signed mul-add).
//   * PHADDW/PHSUBW/PHADDSW (horizontal add/sub, saturating variant).
//   * PSIGNB/W/D (apply sign of second operand).
//   * PHMINPOSUW (horizontal min + position), MPSADBW (sum-of-abs-diff block).
//   * PACKUSDW (saturating pack dword->uword), PMOVSXBD/PMOVZXBW (extend).
//   * PALIGNR (byte concat shift), PBLENDVB (variable byte blend).
//   * PMULLD (32-bit packed multiply), PMAXSD/PMINUW (packed min/max).
//
// Each __m128i result is byte-hashed for a bit-exact scalar return so any
// lane-misplacement / saturation / rounding divergence surfaces.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SSEIntProbeRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SSEIntProbeRT, Verify) { roundTripX64(GetParam()); }

// Shared prologue: a byte-hash reducer for __m128i and the intrin header.
#define HDR \
  "#include <immintrin.h>\n" \
  "static unsigned hsh(__m128i v){ unsigned char b[16]; " \
  "_mm_storeu_si128((__m128i*)b,v); unsigned h=0; " \
  "for(int i=0;i<16;i++) h=h*131u+b[i]; return h; }\n"

// clang-format off
static const std::vector<RoundTripTC> kSSEInt = {
  // PMULHRSW: (a*b + 0x4000) >> 15, signed 8x i16.
  {"pmulhrsw",
   HDR
   "long pmulhrsw(long a){\n"
   "  short x=(short)a;\n"
   "  __m128i va=_mm_set_epi16(x,-x,32767,-32768,1,-1,16384,-16384);\n"
   "  __m128i vb=_mm_set_epi16(16384,16384,32767,-32768,32767,32767,2,2);\n"
   "  return (long)hsh(_mm_mulhrs_epi16(va,vb));\n"
   "}\n",
   {300}, "SSEIntProbe", 2, "-msse4.1 -ffreestanding"},

  // PMADDUBSW: unsigned bytes * signed bytes, add pairs, saturate to i16.
  {"pmaddubsw",
   HDR
   "long pmaddubsw(long a){\n"
   "  unsigned char x=(unsigned char)a;\n"
   "  __m128i va=_mm_set_epi8(x,255,1,128,200,5,3,17,x,254,2,127,199,6,4,18);\n"
   "  __m128i vb=_mm_set_epi8(127,-128,100,-100,1,-1,50,-50,127,-128,100,-100,1,-1,50,-50);\n"
   "  return (long)hsh(_mm_maddubs_epi16(va,vb));\n"
   "}\n",
   {88}, "SSEIntProbe", 2, "-msse4.1 -ffreestanding"},

  // PHADDW: horizontal add adjacent i16 pairs across two vectors.
  {"phaddw",
   HDR
   "long phaddw(long a){\n"
   "  short x=(short)a;\n"
   "  __m128i va=_mm_set_epi16(x,-x,1000,-1000,32767,1,2,3);\n"
   "  __m128i vb=_mm_set_epi16(4,5,6,7,-32768,8,9,10);\n"
   "  return (long)hsh(_mm_hadd_epi16(va,vb));\n"
   "}\n",
   {77}, "SSEIntProbe", 2, "-msse4.1 -ffreestanding"},

  // PHADDSW: saturating horizontal add of i16 pairs.
  {"phaddsw",
   HDR
   "long phaddsw(long a){\n"
   "  short x=(short)a;\n"
   "  __m128i va=_mm_set_epi16(32767,32767,(short)-32768,(short)-32768,x,-x,1,2);\n"
   "  __m128i vb=_mm_set_epi16(20000,20000,-20000,-20000,3,4,5,6);\n"
   "  return (long)hsh(_mm_hadds_epi16(va,vb));\n"
   "}\n",
   {123}, "SSEIntProbe", 2, "-msse4.1 -ffreestanding"},

  // PHSUBW: horizontal subtract adjacent i16 pairs.
  {"phsubw",
   HDR
   "long phsubw(long a){\n"
   "  short x=(short)a;\n"
   "  __m128i va=_mm_set_epi16(x,-x,1000,500,32767,-32768,2,3);\n"
   "  __m128i vb=_mm_set_epi16(4,5,6,7,8,9,10,11);\n"
   "  return (long)hsh(_mm_hsub_epi16(va,vb));\n"
   "}\n",
   {55}, "SSEIntProbe", 2, "-msse4.1 -ffreestanding"},

  // PSIGNB: negate/zero/keep bytes per sign of second operand.
  {"psignb",
   HDR
   "long psignb(long a){\n"
   "  signed char x=(signed char)a;\n"
   "  __m128i va=_mm_set_epi8(x,-100,100,-1,0,127,-128,50,x,-100,100,-1,0,127,-128,50);\n"
   "  __m128i vb=_mm_set_epi8(-1,0,1,-1,1,0,-1,1,-1,0,1,-1,1,0,-1,1);\n"
   "  return (long)hsh(_mm_sign_epi8(va,vb));\n"
   "}\n",
   {33}, "SSEIntProbe", 2, "-msse4.1 -ffreestanding"},

  // PHMINPOSUW: minimum unsigned i16 + its position (SSE4.1).
  {"phminposuw",
   HDR
   "long phminposuw(long a){\n"
   "  unsigned short x=(unsigned short)a;\n"
   "  __m128i v=_mm_set_epi16(x,50000,3,40000,7,60000,x+1,2);\n"
   "  return (long)hsh(_mm_minpos_epu16(v));\n"
   "}\n",
   {9}, "SSEIntProbe", 2, "-msse4.1 -ffreestanding"},

  // MPSADBW: 4-wide sum-of-absolute-differences blocks (SSE4.1).
  {"mpsadbw",
   HDR
   "long mpsadbw(long a){\n"
   "  unsigned char x=(unsigned char)a;\n"
   "  __m128i va=_mm_set_epi8(x,255,1,128,200,5,3,17,x,254,2,127,199,6,4,18);\n"
   "  __m128i vb=_mm_set_epi8(10,20,30,40,50,60,70,80,90,100,110,120,130,140,150,160);\n"
   "  return (long)hsh(_mm_mpsadbw_epu8(va,vb,5));\n"
   "}\n",
   {88}, "SSEIntProbe", 2, "-msse4.1 -ffreestanding"},

  // PACKUSDW: saturating pack i32 -> u16 (SSE4.1).
  {"packusdw",
   HDR
   "long packusdw(long a){\n"
   "  int x=(int)a;\n"
   "  __m128i va=_mm_set_epi32(x,-1,70000,3);\n"
   "  __m128i vb=_mm_set_epi32(0x7FFFFFFF,(int)0x80000000,65535,65536);\n"
   "  return (long)hsh(_mm_packus_epi32(va,vb));\n"
   "}\n",
   {40000}, "SSEIntProbe", 2, "-msse4.1 -ffreestanding"},

  // PMOVSXBD: sign-extend low 4 bytes -> 4x i32 (SSE4.1).
  {"pmovsxbd",
   HDR
   "long pmovsxbd(long a){\n"
   "  signed char x=(signed char)a;\n"
   "  __m128i v=_mm_set_epi8(0,0,0,0,0,0,0,0,0,0,0,0,x,-1,127,-128);\n"
   "  return (long)hsh(_mm_cvtepi8_epi32(v));\n"
   "}\n",
   {251}, "SSEIntProbe", 2, "-msse4.1 -ffreestanding"},

  // PMOVZXBW: zero-extend low 8 bytes -> 8x i16 (SSE4.1).
  {"pmovzxbw",
   HDR
   "long pmovzxbw(long a){\n"
   "  unsigned char x=(unsigned char)a;\n"
   "  __m128i v=_mm_set_epi8(0,0,0,0,0,0,0,0,x,255,1,128,200,5,3,17);\n"
   "  return (long)hsh(_mm_cvtepu8_epi16(v));\n"
   "}\n",
   {200}, "SSEIntProbe", 2, "-msse4.1 -ffreestanding"},

  // PALIGNR: concat vb:va and byte-shift right by 5.
  {"palignr",
   HDR
   "long palignr(long a){\n"
   "  unsigned char x=(unsigned char)a;\n"
   "  __m128i va=_mm_set_epi8(x,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15);\n"
   "  __m128i vb=_mm_set_epi8(16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31);\n"
   "  return (long)hsh(_mm_alignr_epi8(vb,va,5));\n"
   "}\n",
   {99}, "SSEIntProbe", 2, "-msse4.1 -ffreestanding"},

  // PBLENDVB: per-byte variable blend by sign bit of mask (SSE4.1).
  {"pblendvb",
   HDR
   "long pblendvb(long a){\n"
   "  unsigned char x=(unsigned char)a;\n"
   "  __m128i va=_mm_set_epi8(x,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15);\n"
   "  __m128i vb=_mm_set_epi8(100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115);\n"
   "  __m128i mask=_mm_set_epi8(-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0);\n"
   "  return (long)hsh(_mm_blendv_epi8(va,vb,mask));\n"
   "}\n",
   {77}, "SSEIntProbe", 2, "-msse4.1 -ffreestanding"},

  // PMULLD: 32-bit packed low multiply (SSE4.1).
  {"pmulld",
   HDR
   "long pmulld(long a){\n"
   "  int x=(int)a;\n"
   "  __m128i va=_mm_set_epi32(x,-x,0x10001,123456);\n"
   "  __m128i vb=_mm_set_epi32(65537,3,0x10001,654321);\n"
   "  return (long)hsh(_mm_mullo_epi32(va,vb));\n"
   "}\n",
   {30000}, "SSEIntProbe", 2, "-msse4.1 -ffreestanding"},

  // PMAXSD / PMINUW combined (SSE4.1 packed signed-max / unsigned-min).
  {"pmaxsd_pminuw",
   HDR
   "long pmaxsd_pminuw(long a){\n"
   "  int x=(int)a;\n"
   "  __m128i va=_mm_set_epi32(x,-x,0x7FFFFFFF,(int)0x80000000);\n"
   "  __m128i vb=_mm_set_epi32(0,-1,1,0x40000000);\n"
   "  __m128i mx=_mm_max_epi32(va,vb);\n"
   "  __m128i mn=_mm_min_epu16(va,vb);\n"
   "  return (long)(hsh(mx)*131u+hsh(mn));\n"
   "}\n",
   {12345}, "SSEIntProbe", 2, "-msse4.1 -ffreestanding"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(SSEIntProbe, X64SSEIntProbeRT,
                         ::testing::ValuesIn(kSSEInt), rtTCName);
