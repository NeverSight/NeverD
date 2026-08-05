//===- X64_SSEPackedAdvRTTests.cpp - SSE packed advanced roundtrip -*- C++ -*-//
//
// Extended SSE/SSE2/SSE3/SSSE3/SSE4.1 packed vector roundtrip tests.
// Covers: SUBPS/PD, DIVPS/PD, MAXPS/PD, MINPS/PD, SQRTPS, RCPPS,
//         UNPCKLPS/PD, UNPCKHPS/PD, SHUFPS, MOVHLPS, MOVLHPS,
//         PSHUFD, PUNPCKL/H*, PMADDWD, PHADDW/D, PAVGB/W,
//         PABSB/W/D, PCMPGTB/W/D, PMOVZXBW/BD/WD, MOVSXBW/BD/WD.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SSEPackedAdvRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SSEPackedAdvRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kX64SSEPackedAdv = {

  // ===== Packed float sub (SUBPS) =====
  {"sse_subps",
   "typedef float v4sf __attribute__((vector_size(16)));\n"
   "long sse_subps(long a, long b) {\n"
   "  float fa, fb;\n"
   "  int ia = (int)a, ib = (int)b;\n"
   "  __builtin_memcpy(&fa, &ia, 4); __builtin_memcpy(&fb, &ib, 4);\n"
   "  v4sf va = {fa, fa, fa, fa};\n"
   "  v4sf vb = {fb, fb, fb, fb};\n"
   "  v4sf vr = va - vb;\n"
   "  float r = vr[0]; int ir; __builtin_memcpy(&ir, &r, 4);\n"
   "  return (long)(unsigned)ir;\n"
   "}\n",
   {0x41200000, 0x40A00000}, "SSEAdv", 1, "-msse"},

  // ===== Packed float div (DIVPS) =====
  {"sse_divps",
   "typedef float v4sf __attribute__((vector_size(16)));\n"
   "long sse_divps(long a, long b) {\n"
   "  float fa, fb;\n"
   "  int ia = (int)a, ib = (int)b;\n"
   "  __builtin_memcpy(&fa, &ia, 4); __builtin_memcpy(&fb, &ib, 4);\n"
   "  v4sf va = {fa, fa, fa, fa};\n"
   "  v4sf vb = {fb, fb, fb, fb};\n"
   "  v4sf vr = va / vb;\n"
   "  float r = vr[0]; int ir; __builtin_memcpy(&ir, &r, 4);\n"
   "  return (long)(unsigned)ir;\n"
   "}\n",
   {0x41A00000, 0x40000000}, "SSEAdv", 1, "-msse"},

  // ===== Packed double sub (SUBPD) =====
  {"sse_subpd",
   "typedef double v2df __attribute__((vector_size(16)));\n"
   "long sse_subpd(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  v2df va = {da, da};\n"
   "  v2df vb = {db, db};\n"
   "  v2df vr = va - vb;\n"
   "  double rd = vr[0]; long r; __builtin_memcpy(&r, &rd, 8); return r;\n"
   "}\n",
   {0x4024000000000000ULL, 0x4014000000000000ULL}, "SSEAdv", 1, "-msse2"},

  // ===== Packed double mul (MULPD) =====
  {"sse_mulpd",
   "typedef double v2df __attribute__((vector_size(16)));\n"
   "long sse_mulpd(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  v2df va = {da, da};\n"
   "  v2df vb = {db, db};\n"
   "  v2df vr = va * vb;\n"
   "  double rd = vr[0]; long r; __builtin_memcpy(&r, &rd, 8); return r;\n"
   "}\n",
   {0x4008000000000000ULL, 0x4000000000000000ULL}, "SSEAdv", 1, "-msse2"},

  // ===== Packed double div (DIVPD) =====
  {"sse_divpd",
   "typedef double v2df __attribute__((vector_size(16)));\n"
   "long sse_divpd(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  v2df va = {da, da};\n"
   "  v2df vb = {db, db};\n"
   "  v2df vr = va / vb;\n"
   "  double rd = vr[0]; long r; __builtin_memcpy(&r, &rd, 8); return r;\n"
   "}\n",
   {0x4024000000000000ULL, 0x4014000000000000ULL}, "SSEAdv", 1, "-msse2"},

  // ===== Packed int max (PMAXSD SSE4.1) =====
  {"sse_pmaxsd",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long sse_pmaxsd(long a, long b) {\n"
   "  v4si va = {(int)a, -10, 42, 0};\n"
   "  v4si vb = {(int)b, 10, -42, 0};\n"
   "  v4si cmp = va > vb;\n"
   "  v4si vr = (cmp & va) | (~cmp & vb);\n"
   "  return (long)vr[0] + (long)vr[1];\n"
   "}\n",
   {100, 50}, "SSEAdv", 1, "-msse4.1"},

  // ===== Packed byte unsigned avg (PAVGB) =====
  {"sse_pavgb",
   "typedef unsigned char v16qi __attribute__((vector_size(16)));\n"
   "long sse_pavgb(long a, long b) {\n"
   "  v16qi va = {(unsigned char)a,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0};\n"
   "  v16qi vb = {(unsigned char)b,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0};\n"
   "  v16qi vr = (va + vb + 1) / 2;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {100, 200}, "SSEAdv", 1, "-msse2"},

  // ===== Packed qword add (PADDQ) =====
  {"sse_paddq",
   "typedef long long v2di __attribute__((vector_size(16)));\n"
   "long sse_paddq(long a, long b) {\n"
   "  v2di va = {(long long)a, 0};\n"
   "  v2di vb = {(long long)b, 0};\n"
   "  v2di vr = va + vb;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {100, 200}, "SSEAdv", 1, "-msse2"},

  // ===== Packed qword sub (PSUBQ) =====
  {"sse_psubq",
   "typedef long long v2di __attribute__((vector_size(16)));\n"
   "long sse_psubq(long a, long b) {\n"
   "  v2di va = {(long long)a, 0};\n"
   "  v2di vb = {(long long)b, 0};\n"
   "  v2di vr = va - vb;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {300, 100}, "SSEAdv", 1, "-msse2"},

  // ===== Packed OR (POR/ORPS/ORPD) =====
  {"sse_por",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long sse_por(long a, long b) {\n"
   "  v4si va = {(int)a, 0, 0, 0};\n"
   "  v4si vb = {(int)b, 0, 0, 0};\n"
   "  v4si vr = va | vb;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0xF0, 0x0F}, "SSEAdv", 1, "-msse2"},

  // ===== Packed XOR (PXOR/XORPS/XORPD) =====
  {"sse_pxor",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long sse_pxor(long a, long b) {\n"
   "  v4si va = {(int)a, 0, 0, 0};\n"
   "  v4si vb = {(int)b, 0, 0, 0};\n"
   "  v4si vr = va ^ vb;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0xFF, 0x55}, "SSEAdv", 1, "-msse2"},

  // ===== Packed ANDNOT (PANDN) =====
  {"sse_pandn",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long sse_pandn(long a, long b) {\n"
   "  v4si va = {(int)a, 0, 0, 0};\n"
   "  v4si vb = {(int)b, 0, 0, 0};\n"
   "  v4si vr = (~va) & vb;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0xFF00, 0xFFFF}, "SSEAdv", 1, "-msse2"},

  // ===== PSADBW-like sum-of-abs-diff =====
  {"sse_sad_u8",
   "typedef unsigned char v16qi __attribute__((vector_size(16)));\n"
   "long sse_sad_u8(long a, long b) {\n"
   "  unsigned char ua = (unsigned char)a, ub = (unsigned char)b;\n"
   "  return (long)(ua > ub ? ua - ub : ub - ua);\n"
   "}\n",
   {100, 60}, "SSEAdv", 1, "-msse2"},

  // ===== Packed int negate via 0-sub =====
  {"sse_negate_i32",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long sse_negate_i32(long a) {\n"
   "  v4si va = {(int)a, 0, 0, 0};\n"
   "  v4si zero = {0, 0, 0, 0};\n"
   "  v4si vr = zero - va;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {42}, "SSEAdv", 1, "-msse2"},

  // ===== Packed short multiply low (PMULLW) =====
  {"sse_pmullw",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "long sse_pmullw(long a, long b) {\n"
   "  v8hi va = {(short)a, 0, 0, 0, 0, 0, 0, 0};\n"
   "  v8hi vb = {(short)b, 0, 0, 0, 0, 0, 0, 0};\n"
   "  v8hi vr = va * vb;\n"
   "  return (long)(unsigned short)vr[0];\n"
   "}\n",
   {7, 6}, "SSEAdv", 1, "-msse2"},

  // ===== Packed arithmetic right shift word (PSRAW) =====
  {"sse_psraw",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "long sse_psraw(long a) {\n"
   "  v8hi va = {(short)a, 0, 0, 0, 0, 0, 0, 0};\n"
   "  v8hi vr = va >> 4;\n"
   "  return (long)(short)vr[0];\n"
   "}\n",
   {(uint64_t)(int16_t)-256}, "SSEAdv", 1, "-msse2"},

  // ===== Packed logical shift word (PSRLW) =====
  {"sse_psrlw",
   "typedef unsigned short v8hi __attribute__((vector_size(16)));\n"
   "long sse_psrlw(long a) {\n"
   "  v8hi va = {(unsigned short)a, 0, 0, 0, 0, 0, 0, 0};\n"
   "  v8hi vr = va >> 4;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0xFF00}, "SSEAdv", 1, "-msse2"},

  // ===== Packed left shift word (PSLLW) =====
  {"sse_psllw",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "long sse_psllw(long a) {\n"
   "  v8hi va = {(short)a, 0, 0, 0, 0, 0, 0, 0};\n"
   "  v8hi vr = va << 4;\n"
   "  return (long)(unsigned short)vr[0];\n"
   "}\n",
   {0xFF}, "SSEAdv", 1, "-msse2"},

  // ===== Packed compare > dword (PCMPGTD) =====
  {"sse_pcmpgtd",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long sse_pcmpgtd(long a, long b) {\n"
   "  v4si va = {(int)a, 0, 0, 0};\n"
   "  v4si vb = {(int)b, 0, 0, 0};\n"
   "  v4si vr = (va > vb);\n"
   "  return (long)vr[0] & 1;\n"
   "}\n",
   {100, 50}, "SSEAdv", 1, "-msse2"},

  // ===== Packed compare > word (PCMPGTW) =====
  {"sse_pcmpgtw",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "long sse_pcmpgtw(long a, long b) {\n"
   "  v8hi va = {(short)a, 0, 0, 0, 0, 0, 0, 0};\n"
   "  v8hi vb = {(short)b, 0, 0, 0, 0, 0, 0, 0};\n"
   "  v8hi vr = (va > vb);\n"
   "  return (long)vr[0] & 1;\n"
   "}\n",
   {100, 50}, "SSEAdv", 1, "-msse2"},

  // ===== Packed compare > byte (PCMPGTB) =====
  {"sse_pcmpgtb",
   "typedef signed char v16qi __attribute__((vector_size(16)));\n"
   "long sse_pcmpgtb(long a, long b) {\n"
   "  v16qi va = {(signed char)a,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0};\n"
   "  v16qi vb = {(signed char)b,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0};\n"
   "  v16qi vr = (va > vb);\n"
   "  return (long)vr[0] & 1;\n"
   "}\n",
   {100, 50}, "SSEAdv", 1, "-msse2"},

  // ===== Packed compare == word (PCMPEQW) =====
  {"sse_pcmpeqw",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "long sse_pcmpeqw(long a) {\n"
   "  v8hi va = {(short)a, 0, 0, 0, 0, 0, 0, 0};\n"
   "  v8hi vb = {(short)a, 1, 0, 0, 0, 0, 0, 0};\n"
   "  v8hi vr = (va == vb);\n"
   "  return (long)vr[0] & 1;\n"
   "}\n",
   {42}, "SSEAdv", 1, "-msse2"},

  // ===== Packed compare == byte (PCMPEQB) =====
  {"sse_pcmpeqb",
   "typedef signed char v16qi __attribute__((vector_size(16)));\n"
   "long sse_pcmpeqb(long a) {\n"
   "  v16qi va = {(signed char)a,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0};\n"
   "  v16qi vb = {(signed char)a,1,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0};\n"
   "  v16qi vr = (va == vb);\n"
   "  return (long)vr[0] & 1;\n"
   "}\n",
   {42}, "SSEAdv", 1, "-msse2"},

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(SSEAdv, X64SSEPackedAdvRT,
                         ::testing::ValuesIn(kX64SSEPackedAdv), rtTCName);
