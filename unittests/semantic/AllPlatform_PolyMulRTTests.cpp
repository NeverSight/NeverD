//===- AllPlatform_PolyMulRTTests.cpp - Polynomial multiply -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for polynomial (carry-less, GF(2)[x]) multiply, which is a
// distinct operation from ordinary integer multiply:
//
//   * AArch64 PMUL (`pmul v.8b/.16b`)  — same-width per-byte poly multiply.
//   * ARM32   VMUL.p8 (`vmul.p8 d/q`)  — same-width per-byte poly multiply.
//     Both were lifted as a full-width INT_MULT placeholder (wrong result and
//     not per-lane); now mapped to @llvm.aarch64.neon.pmul / @llvm.arm.neon.vmulp.
//
//   * PMULL p8 (`vmull_p8`) and x86 PCLMULQDQ are included as controls — these
//     carry-less paths were already correct and must stay green.
//
// Inputs are chosen so the poly product differs from the integer product (e.g.
// byte 0x53*0x53 = 0x69 integer vs 0x05 poly), so a regression to INT_MULT
// would change the result and fail the roundtrip.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64PolyMulRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64PolyMulRT, Verify) { roundTripX64(GetParam()); }

class A64PolyMulRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64PolyMulRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32PolyMulRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32PolyMulRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kX64PolyMul = {
  // PCLMULQDQ — 64x64 -> 128 carry-less multiply (control; already correct).
  {"x64_clmul",
   "#include <wmmintrin.h>\n"
   "#include <emmintrin.h>\n"
   "unsigned long long x64_clmul(unsigned long long a){\n"
   "  __m128i va=_mm_set_epi64x((long long)(a*3+1),(long long)a);\n"
   "  __m128i vb=_mm_set_epi64x((long long)(a^0x1234567ULL),(long long)(a|0x80ULL));\n"
   "  __m128i r=_mm_clmulepi64_si128(va,vb,0x00);\n"
   "  unsigned long long lo=(unsigned long long)_mm_cvtsi128_si64(r);\n"
   "  unsigned long long hi=(unsigned long long)_mm_cvtsi128_si64(_mm_srli_si128(r,8));\n"
   "  return lo ^ (hi*131);\n"
   "}\n",
   {0x53A7C19FULL}, "PolyMul", 1, "-mpclmul -msse2 -ffreestanding"},

  // PCLMULQDQ selecting the high 64-bit lanes (imm 0x11).
  {"x64_clmul_hi",
   "#include <wmmintrin.h>\n"
   "#include <emmintrin.h>\n"
   "unsigned long long x64_clmul_hi(unsigned long long a){\n"
   "  __m128i va=_mm_set_epi64x((long long)a,(long long)(a*7+3));\n"
   "  __m128i vb=_mm_set_epi64x((long long)(a*5+9),(long long)(a^0xABCDULL));\n"
   "  __m128i r=_mm_clmulepi64_si128(va,vb,0x11);\n"
   "  unsigned long long lo=(unsigned long long)_mm_cvtsi128_si64(r);\n"
   "  unsigned long long hi=(unsigned long long)_mm_cvtsi128_si64(_mm_srli_si128(r,8));\n"
   "  return lo + hi;\n"
   "}\n",
   {0x9E3779B9ULL}, "PolyMul", 1, "-mpclmul -msse2 -ffreestanding"},
  // NOTE: x86 GFNI (GF2P8MULB / GF2P8AFFINEQB / GF2P8AFFINEINVQB) is not
  // emulated by this Unicorn build (UC_ERR_INSN_INVALID); it needs a TCG helper
  // family in the fork.  Left for a dedicated Unicorn task — see docs.
};

static const std::vector<RoundTripTC> kA64PolyMul = {
  // PMUL .8b — same-width per-byte poly multiply (8 lanes).
  {"a64_pmul8d",
   "#include <arm_neon.h>\n"
   "unsigned long a64_pmul8d(unsigned long a){\n"
   "  unsigned char x=(unsigned char)a;\n"
   "  uint8x8_t ua={x,0x53,0xFF,0x01,0x80,0x7F,0x53,x};\n"
   "  uint8x8_t ub={0x53,x,0x02,0xFF,0x53,0x01,x,0x80};\n"
   "  uint8x8_t vr=vreinterpret_u8_p8(\n"
   "      vmul_p8(vreinterpret_p8_u8(ua),vreinterpret_p8_u8(ub)));\n"
   "  unsigned long r=0;\n"
   "  for(int i=0;i<8;i++) r=(r*131)^vr[i];\n"
   "  return r;\n"
   "}\n",
   {0x53ULL}, "PolyMul", 1, ""},

  // PMUL .16b — same-width per-byte poly multiply (16 lanes).
  {"a64_pmul8q",
   "#include <arm_neon.h>\n"
   "unsigned long a64_pmul8q(unsigned long a){\n"
   "  unsigned char x=(unsigned char)a;\n"
   "  uint8x16_t ua={x,0x53,0xFF,0x01,0x80,0x7F,0x53,x,\n"
   "                 0x11,0x22,x,0x44,0x53,0xAA,0x01,0xFF};\n"
   "  uint8x16_t ub={0x53,x,0x02,0xFF,0x53,0x01,x,0x80,\n"
   "                 x,0x53,0x33,x,0x55,0x53,0xFF,0x01};\n"
   "  uint8x16_t vr=vreinterpretq_u8_p8(\n"
   "      vmulq_p8(vreinterpretq_p8_u8(ua),vreinterpretq_p8_u8(ub)));\n"
   "  unsigned long r=0;\n"
   "  for(int i=0;i<16;i++) r=(r*131)^vr[i];\n"
   "  return r;\n"
   "}\n",
   {0xC7ULL}, "PolyMul", 1, ""},

  // PMULL p8 — widening 8->16 poly multiply (control; already correct).
  {"a64_pmull8",
   "#include <arm_neon.h>\n"
   "unsigned long a64_pmull8(unsigned long a){\n"
   "  unsigned char x=(unsigned char)a;\n"
   "  uint8x8_t ua={x,0x53,0xFF,0x01,0x80,0x7F,0x53,x};\n"
   "  uint8x8_t ub={0x53,x,0x02,0xFF,0x53,0x01,x,0x80};\n"
   "  uint16x8_t vr=vreinterpretq_u16_p16(\n"
   "      vmull_p8(vreinterpret_p8_u8(ua),vreinterpret_p8_u8(ub)));\n"
   "  unsigned long r=0;\n"
   "  for(int i=0;i<8;i++) r=(r*131)^vr[i];\n"
   "  return r;\n"
   "}\n",
   {0x9BULL}, "PolyMul", 1, ""},
};

static const std::vector<RoundTripTC> kARM32PolyMul = {
  // VMUL.p8 d-reg — same-width per-byte poly multiply (8 lanes).
  {"arm_pmul8d",
   "#include <arm_neon.h>\n"
   "unsigned arm_pmul8d(unsigned a){\n"
   "  unsigned char x=(unsigned char)a;\n"
   "  uint8x8_t ua={x,0x53,0xFF,0x01,0x80,0x7F,0x53,x};\n"
   "  uint8x8_t ub={0x53,x,0x02,0xFF,0x53,0x01,x,0x80};\n"
   "  uint8x8_t vr=vreinterpret_u8_p8(\n"
   "      vmul_p8(vreinterpret_p8_u8(ua),vreinterpret_p8_u8(ub)));\n"
   "  unsigned r=0;\n"
   "  for(int i=0;i<8;i++) r=(r*131)^vr[i];\n"
   "  return r;\n"
   "}\n",
   {0x53ULL}, "PolyMul", 1, "-mfpu=neon"},

  // VMUL.p8 q-reg — same-width per-byte poly multiply (16 lanes).
  {"arm_pmul8q",
   "#include <arm_neon.h>\n"
   "unsigned arm_pmul8q(unsigned a){\n"
   "  unsigned char x=(unsigned char)a;\n"
   "  uint8x16_t ua={x,0x53,0xFF,0x01,0x80,0x7F,0x53,x,\n"
   "                 0x11,0x22,x,0x44,0x53,0xAA,0x01,0xFF};\n"
   "  uint8x16_t ub={0x53,x,0x02,0xFF,0x53,0x01,x,0x80,\n"
   "                 x,0x53,0x33,x,0x55,0x53,0xFF,0x01};\n"
   "  uint8x16_t vr=vreinterpretq_u8_p8(\n"
   "      vmulq_p8(vreinterpretq_p8_u8(ua),vreinterpretq_p8_u8(ub)));\n"
   "  unsigned r=0;\n"
   "  for(int i=0;i<16;i++) r=(r*131)^vr[i];\n"
   "  return r;\n"
   "}\n",
   {0xC7ULL}, "PolyMul", 1, "-mfpu=neon"},

  // VMULL.p8 — widening 8->16 poly multiply (control; already correct).
  {"arm_pmull8",
   "#include <arm_neon.h>\n"
   "unsigned arm_pmull8(unsigned a){\n"
   "  unsigned char x=(unsigned char)a;\n"
   "  uint8x8_t ua={x,0x53,0xFF,0x01,0x80,0x7F,0x53,x};\n"
   "  uint8x8_t ub={0x53,x,0x02,0xFF,0x53,0x01,x,0x80};\n"
   "  uint16x8_t vr=vreinterpretq_u16_p16(\n"
   "      vmull_p8(vreinterpret_p8_u8(ua),vreinterpret_p8_u8(ub)));\n"
   "  unsigned r=0;\n"
   "  for(int i=0;i<8;i++) r=(r*131)^vr[i];\n"
   "  return r;\n"
   "}\n",
   {0x9BULL}, "PolyMul", 1, "-mfpu=neon"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(PolyMul, X64PolyMulRT,
                         ::testing::ValuesIn(kX64PolyMul), rtTCName);
INSTANTIATE_TEST_SUITE_P(PolyMul, A64PolyMulRT,
                         ::testing::ValuesIn(kA64PolyMul), rtTCName);
INSTANTIATE_TEST_SUITE_P(PolyMul, ARM32PolyMulRT,
                         ::testing::ValuesIn(kARM32PolyMul), rtTCName);
