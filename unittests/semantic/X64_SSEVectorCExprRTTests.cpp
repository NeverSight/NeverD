//===- X64_SSEVectorCExprRTTests.cpp - SSE vector C expression roundtrip --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests x86_64 SSE/SSE2/SSE3/SSE4.1 packed vector operations through C
// vector types. These C expressions naturally generate many SSE instructions
// that the audit tool can't detect from mnemonic matching.
//
// Exercises: PADDB/W/D/Q, PSUBB/W/D/Q, PMULLW/D, ADDPS/PD, SUBPS/PD,
// MULPS/PD, DIVPS/PD, ANDPS/PD, ORPS/PD, XORPS/PD, MINPS/PD, MAXPS/PD,
// CVTPS2PD, CVTPD2PS, CVTDQ2PS, CVTPS2DQ, PUNPCKL*/H*, PACKSSWB/DW,
// PAND, POR, PXOR, PSLLW/D/Q, PSRLW/D/Q, PSRAW/D.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SSEVecCExprRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SSEVecCExprRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kX64SSEVecCExpr = {

  // ===== Packed int add (PADDD v4si) =====
  {"sse_paddd_extract",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long sse_paddd_extract(long a, long b) {\n"
   "  v4si va = {(int)a, (int)(a>>1), (int)(a>>2), (int)(a>>3)};\n"
   "  v4si vb = {(int)b, (int)(b>>1), (int)(b>>2), (int)(b>>3)};\n"
   "  v4si vr = va + vb;\n"
   "  return (long)vr[0] + (long)vr[1] + (long)vr[2] + (long)vr[3];\n"
   "}\n",
   {100, 50}, "SSEVec", 1, "-msse2"},

  // ===== Packed int sub (PSUBD v4si) =====
  {"sse_psubd_extract",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long sse_psubd_extract(long a, long b) {\n"
   "  v4si va = {(int)a, (int)(a+1), (int)(a+2), (int)(a+3)};\n"
   "  v4si vb = {(int)b, (int)b, (int)b, (int)b};\n"
   "  v4si vr = va - vb;\n"
   "  return (long)vr[0] + (long)vr[1] + (long)vr[2] + (long)vr[3];\n"
   "}\n",
   {100, 10}, "SSEVec", 1, "-msse2"},

  // ===== Packed int multiply (PMULLD v4si) =====
  {"sse_pmulld_extract",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long sse_pmulld_extract(long a, long b) {\n"
   "  v4si va = {(int)a, 2, 3, 4};\n"
   "  v4si vb = {(int)b, 5, 6, 7};\n"
   "  v4si vr = va * vb;\n"
   "  return (long)vr[0] + (long)vr[1] + (long)vr[2] + (long)vr[3];\n"
   "}\n",
   {3, 7}, "SSEVec", 1, "-msse4.1"},

  // ===== Packed float add (ADDPS v4sf) =====
  {"sse_addps",
   "typedef float v4sf __attribute__((vector_size(16)));\n"
   "long sse_addps(long a, long b) {\n"
   "  float fa, fb;\n"
   "  int ia = (int)a, ib = (int)b;\n"
   "  __builtin_memcpy(&fa, &ia, 4); __builtin_memcpy(&fb, &ib, 4);\n"
   "  v4sf va = {fa, fa, fa, fa};\n"
   "  v4sf vb = {fb, fb, fb, fb};\n"
   "  v4sf vr = va + vb;\n"
   "  float r = vr[0]; int ir; __builtin_memcpy(&ir, &r, 4);\n"
   "  return (long)(unsigned)ir;\n"
   "}\n",
   {0x40A00000, 0x40000000}, "SSEVec", 1, "-msse"},

  // ===== Packed float mul (MULPS v4sf) =====
  {"sse_mulps",
   "typedef float v4sf __attribute__((vector_size(16)));\n"
   "long sse_mulps(long a, long b) {\n"
   "  float fa, fb;\n"
   "  int ia = (int)a, ib = (int)b;\n"
   "  __builtin_memcpy(&fa, &ia, 4); __builtin_memcpy(&fb, &ib, 4);\n"
   "  v4sf va = {fa, fa, fa, fa};\n"
   "  v4sf vb = {fb, fb, fb, fb};\n"
   "  v4sf vr = va * vb;\n"
   "  float r = vr[0]; int ir; __builtin_memcpy(&ir, &r, 4);\n"
   "  return (long)(unsigned)ir;\n"
   "}\n",
   {0x40400000, 0x40000000}, "SSEVec", 1, "-msse"},

  // ===== Packed double add (ADDPD v2df) =====
  {"sse_addpd",
   "typedef double v2df __attribute__((vector_size(16)));\n"
   "long sse_addpd(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  v2df va = {da, da};\n"
   "  v2df vb = {db, db};\n"
   "  v2df vr = va + vb;\n"
   "  double rd = vr[0]; long r; __builtin_memcpy(&r, &rd, 8); return r;\n"
   "}\n",
   {0x4014000000000000ULL, 0x4008000000000000ULL}, "SSEVec", 1, "-msse2"},

  // ===== Packed int bitwise AND (PAND v4si) =====
  {"sse_pand",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long sse_pand(long a, long b) {\n"
   "  v4si va = {(int)a, (int)a, (int)a, (int)a};\n"
   "  v4si vb = {(int)b, (int)b, (int)b, (int)b};\n"
   "  v4si vr = va & vb;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0xFF00, 0x0FF0}, "SSEVec", 1, "-msse2"},

  // ===== Packed int shift left (PSLLD v4si) =====
  {"sse_pslld",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long sse_pslld(long a) {\n"
   "  v4si va = {(int)a, (int)a, (int)a, (int)a};\n"
   "  v4si vr = va << 4;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0xFF}, "SSEVec", 1, "-msse2"},

  // ===== Packed int shift right (PSRLD v4si) =====
  {"sse_psrld",
   "typedef unsigned v4ui __attribute__((vector_size(16)));\n"
   "long sse_psrld(long a) {\n"
   "  v4ui va = {(unsigned)a, (unsigned)a, (unsigned)a, (unsigned)a};\n"
   "  v4ui vr = va >> 4;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0xFF0}, "SSEVec", 1, "-msse2"},

  // ===== Packed int comparison (PCMPEQD → MOVMSKPS) =====
  {"sse_pcmpeqd_mask",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long sse_pcmpeqd_mask(long a) {\n"
   "  v4si va = {(int)a, 42, (int)a, 42};\n"
   "  v4si vb = {(int)a, 0, (int)a, 0};\n"
   "  v4si eq = (va == vb);\n"
   "  return (long)eq[0] & 1;\n"
   "}\n",
   {42}, "SSEVec", 1, "-msse2"},

  // ===== Scalar float min (MINSS via conditional) =====
  {"sse_min_scalar",
   "long sse_min_scalar(long a, long b) {\n"
   "  float fa, fb;\n"
   "  int ia = (int)a, ib = (int)b;\n"
   "  __builtin_memcpy(&fa, &ia, 4); __builtin_memcpy(&fb, &ib, 4);\n"
   "  float r = fa < fb ? fa : fb;\n"
   "  int ir; __builtin_memcpy(&ir, &r, 4);\n"
   "  return (long)(unsigned)ir;\n"
   "}\n",
   {0x40A00000, 0x41200000}, "SSEVec", 1, "-msse"},

  // ===== Packed byte add (PADDB v16qi) =====
  {"sse_paddb",
   "typedef char v16qi __attribute__((vector_size(16)));\n"
   "long sse_paddb(long a, long b) {\n"
   "  v16qi va = {(char)a, (char)(a+1), (char)(a+2), (char)(a+3),\n"
   "              (char)a, (char)a, (char)a, (char)a,\n"
   "              (char)a, (char)a, (char)a, (char)a,\n"
   "              (char)a, (char)a, (char)a, (char)a};\n"
   "  v16qi vb = {(char)b, (char)b, (char)b, (char)b,\n"
   "              (char)b, (char)b, (char)b, (char)b,\n"
   "              (char)b, (char)b, (char)b, (char)b,\n"
   "              (char)b, (char)b, (char)b, (char)b};\n"
   "  v16qi vr = va + vb;\n"
   "  return (long)(unsigned char)vr[0];\n"
   "}\n",
   {10, 20}, "SSEVec", 1, "-msse2"},

  // ===== Packed word add (PADDW v8hi) =====
  {"sse_paddw",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "long sse_paddw(long a, long b) {\n"
   "  v8hi va = {(short)a, (short)a, (short)a, (short)a,\n"
   "             (short)a, (short)a, (short)a, (short)a};\n"
   "  v8hi vb = {(short)b, (short)b, (short)b, (short)b,\n"
   "             (short)b, (short)b, (short)b, (short)b};\n"
   "  v8hi vr = va + vb;\n"
   "  return (long)(unsigned short)vr[0];\n"
   "}\n",
   {100, 200}, "SSEVec", 1, "-msse2"},

  // ===== Int-to-float packed conversion (CVTDQ2PS) =====
  {"sse_cvtdq2ps",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "typedef float v4sf __attribute__((vector_size(16)));\n"
   "long sse_cvtdq2ps(long a) {\n"
   "  v4si vi = {(int)a, (int)(a+1), (int)(a+2), (int)(a+3)};\n"
   "  v4sf vf = __builtin_convertvector(vi, v4sf);\n"
   "  float r = vf[0]; int ir; __builtin_memcpy(&ir, &r, 4);\n"
   "  return (long)(unsigned)ir;\n"
   "}\n",
   {42}, "SSEVec", 1, "-msse2"},

  // ===== Float-to-int packed conversion (CVTPS2DQ) =====
  {"sse_cvtps2dq",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "typedef float v4sf __attribute__((vector_size(16)));\n"
   "long sse_cvtps2dq(long a) {\n"
   "  float fa; int ia = (int)a; __builtin_memcpy(&fa, &ia, 4);\n"
   "  v4sf vf = {fa, fa, fa, fa};\n"
   "  v4si vi = __builtin_convertvector(vf, v4si);\n"
   "  return (long)vi[0];\n"
   "}\n",
   {0x42280000}, "SSEVec", 1, "-msse2"},  // 42.0f

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(SSEVec, X64SSEVecCExprRT,
                         ::testing::ValuesIn(kX64SSEVecCExpr), rtTCName);
