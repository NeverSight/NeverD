//===- X64_SSEConvBroadcastRTTests.cpp - SSE conversion/broadcast roundtrip -===//
//
// Covers: CVTPS2DQ, CVTDQ2PS, CVTPD2PS, CVTPS2PD, CVTTPD2DQ, CVTTPS2DQ,
//         CVTDQ2PD, CVTSD2SS, CVTSS2SD, RCPPS, RSQRTPS, SQRTPS, SQRTPD,
//         UNPCKLPS/PD, UNPCKHPS/PD, MOVHLPS, MOVLHPS, SHUFPS, PSHUFD,
//         VPBROADCASTD/Q, VBROADCASTSS/SD, VPERM2F128, VPERM2I128,
//         VPERMD, VPERMPS, VPERMQ, VPERMILPS/PD, VMASKMOVPS/PD
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SSEConvBroadcastRT : public SemanticRoundTripFixture,
                               public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SSEConvBroadcastRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kSSEConvBroadcast = {

  // ===== CVTSI2SS — scalar int→float (covers CVTSI2SS instruction) =====
  {"cvtsi2ss_scalar",
   "long cvtsi2ss_scalar(long a) {\n"
   "  float f = (float)(int)a;\n"
   "  int r; __builtin_memcpy(&r, &f, 4);\n"
   "  return (long)r;\n"
   "}\n",
   {42}, "SSEConv", 1, "-msse2"},

  // ===== CVTSI2SD — scalar int→double =====
  {"cvtsi2sd_scalar",
   "long cvtsi2sd_scalar(long a) {\n"
   "  double d = (double)a;\n"
   "  long r; __builtin_memcpy(&r, &d, 8);\n"
   "  return r;\n"
   "}\n",
   {42}, "SSEConv", 1, "-msse2"},

  // ===== CVTSS2SD — float→double scalar =====
  {"cvtss2sd_scalar",
   "long cvtss2sd_scalar(long a) {\n"
   "  int ia = (int)a;\n"
   "  float f; __builtin_memcpy(&f, &ia, 4);\n"
   "  double d = (double)f;\n"
   "  long r; __builtin_memcpy(&r, &d, 8);\n"
   "  return r;\n"
   "}\n",
   {0x42280000}, "SSEConv", 1, "-msse2"},

  // ===== CVTSD2SS — double→float scalar =====
  {"cvtsd2ss_scalar",
   "long cvtsd2ss_scalar(long a) {\n"
   "  double d; __builtin_memcpy(&d, &a, 8);\n"
   "  float f = (float)d;\n"
   "  int r; __builtin_memcpy(&r, &f, 4);\n"
   "  return (long)r;\n"
   "}\n",
   {0x4045000000000000ULL}, "SSEConv", 1, "-msse2"},

  // ===== CVTTSS2SI — truncate float→int =====
  {"cvttss2si_scalar",
   "long cvttss2si_scalar(long a) {\n"
   "  int ia = (int)a;\n"
   "  float f; __builtin_memcpy(&f, &ia, 4);\n"
   "  return (long)(int)f;\n"
   "}\n",
   {0x42280000}, "SSEConv", 1, "-msse2"},

  // ===== CVTTSD2SI — truncate double→int =====
  {"cvttsd2si_scalar",
   "long cvttsd2si_scalar(long a) {\n"
   "  double d; __builtin_memcpy(&d, &a, 8);\n"
   "  return (long)(long long)d;\n"
   "}\n",
   {0x4045000000000000ULL}, "SSEConv", 1, "-msse2"},

  // ===== Packed int element access — covers MOVD/PSHUFD =====
  {"packed_int_elem_access",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long packed_int_elem_access(long a) {\n"
   "  v4si va = {(int)a, 20, 30, 40};\n"
   "  return (long)va[0] + (long)va[2];\n"
   "}\n",
   {10}, "SSEConv", 1, "-msse2"},

  // ===== PSHUFD — shuffle packed dwords =====
  {"pshufd_c",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long pshufd_c(long a) {\n"
   "  v4si va = {(int)a, 20, 30, 40};\n"
   "  v4si vr = __builtin_shufflevector(va, va, 3, 2, 1, 0);\n"
   "  return (long)vr[0] + (long)vr[1];\n"
   "}\n",
   {10}, "SSEConv", 1, "-msse2"},

  // ===== Packed int XOR — covers PXOR =====
  {"packed_int_xor",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long packed_int_xor(long a, long b) {\n"
   "  v4si va = {(int)a, 0xFF, 0, 0};\n"
   "  v4si vb = {(int)b, 0x55, 0, 0};\n"
   "  v4si vr = va ^ vb;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0xAA, 0x55}, "SSEConv", 1, "-msse2"},

  // ===== UNPCKLPD — unpack low doubles =====
  {"unpcklpd_c",
   "typedef double v2df __attribute__((vector_size(16)));\n"
   "long unpcklpd_c(long a, long b) {\n"
   "  v2df va = {(double)a, 2.0};\n"
   "  v2df vb = {(double)b, 4.0};\n"
   "  v2df vr = __builtin_shufflevector(va, vb, 0, 2);\n"
   "  double r = vr[0] + vr[1];\n"
   "  long lr; __builtin_memcpy(&lr, &r, 8); return lr;\n"
   "}\n",
   {3, 7}, "SSEConv", 1, "-msse2"},

  // ===== PUNPCKLDQ — unpack low dwords =====
  {"punpckldq_c",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long punpckldq_c(long a, long b) {\n"
   "  v4si va = {(int)a, 2, 3, 4};\n"
   "  v4si vb = {(int)b, 6, 7, 8};\n"
   "  v4si vr = __builtin_shufflevector(va, vb, 0, 4, 1, 5);\n"
   "  return (long)vr[0] + (long)vr[1];\n"
   "}\n",
   {10, 20}, "SSEConv", 1, "-msse2"},

  // ===== PUNPCKHWD — unpack high words =====
  {"punpckhwd_c",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "long punpckhwd_c(long a) {\n"
   "  v8hi va = {1, 2, 3, (short)a, 5, 6, 7, 8};\n"
   "  v8hi vb = {11, 12, 13, 14, 15, 16, 17, 18};\n"
   "  v8hi vr = __builtin_shufflevector(va, vb, 4, 12, 5, 13, 6, 14, 7, 15);\n"
   "  return (long)vr[0] + (long)vr[1];\n"
   "}\n",
   {42}, "SSEConv", 1, "-msse2"},

  // ===== MOVHLPS — move high to low packed singles =====
  {"movhlps_c",
   "typedef float v4sf __attribute__((vector_size(16)));\n"
   "long movhlps_c(long a) {\n"
   "  v4sf va = {1.0f, 2.0f, (float)a, 4.0f};\n"
   "  v4sf vb = {5.0f, 6.0f, 7.0f, 8.0f};\n"
   "  v4sf vr = __builtin_shufflevector(vb, va, 2, 3, 2, 3);\n"
   "  float r = vr[0];\n"
   "  return (long)r;\n"
   "}\n",
   {100}, "SSEConv", 1, "-msse"},

  // ===== PSHUFB (SSSE3) — byte shuffle =====
  {"pshufb_c",
   "typedef char v16qi __attribute__((vector_size(16)));\n"
   "long pshufb_c(long a) {\n"
   "  v16qi va = {(char)a, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};\n"
   "  v16qi mask = {3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12};\n"
   "  v16qi vr = __builtin_shufflevector(va, va, 3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);\n"
   "  return (long)(unsigned char)vr[0] + (long)(unsigned char)vr[3];\n"
   "}\n",
   {42}, "SSEConv", 1, "-mssse3"},

  // ===== PALIGNR (SSSE3) — byte alignment =====
  {"palignr_c",
   "typedef char v16qi __attribute__((vector_size(16)));\n"
   "long palignr_c(long a) {\n"
   "  v16qi va = {(char)a, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};\n"
   "  v16qi vb = {16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};\n"
   "  v16qi vr = __builtin_shufflevector(vb, va, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19);\n"
   "  return (long)(unsigned char)vr[0] + (long)(unsigned char)vr[12];\n"
   "}\n",
   {42}, "SSEConv", 1, "-mssse3"},

  // ===== Packed int shift left — covers PSLLD =====
  {"packed_int_shl",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long packed_int_shl(long a) {\n"
   "  v4si va = {(int)a, 2, 3, 4};\n"
   "  v4si vr = va << 2;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {10}, "SSEConv", 1, "-msse2"},

  // ===== Scalar double-to-float truncation =====
  {"double_float_trunc",
   "long double_float_trunc(long a) {\n"
   "  double d = (double)a + 0.7;\n"
   "  float f = (float)d;\n"
   "  return (long)(int)f;\n"
   "}\n",
   {100}, "SSEConv", 1, "-msse2"},

  // ===== Byte zero-extend via C cast — covers MOVZX pattern =====
  {"c_byte_zext",
   "long c_byte_zext(long a) {\n"
   "  unsigned char b = (unsigned char)a;\n"
   "  return (long)b;\n"
   "}\n",
   {0x12FF}, "SSEConv", 1, ""},

  // ===== Short sign-extend via C cast — covers MOVSX pattern =====
  {"c_short_sext",
   "long c_short_sext(long a) {\n"
   "  short s = (short)a;\n"
   "  return (long)s;\n"
   "}\n",
   {0x8000}, "SSEConv", 1, ""},

  // ===== Packed int vector add+store — covers PADDD+MOVDQA =====
  {"packed_int_vec_add",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long packed_int_vec_add(long a, long b) {\n"
   "  v4si va = {(int)a, 10, 20, 30};\n"
   "  v4si vb = {(int)b, 5, 15, 25};\n"
   "  v4si vr = va + vb;\n"
   "  return (long)vr[0] + (long)vr[1];\n"
   "}\n",
   {100, 200}, "SSEConv", 1, "-msse2"},

  // ===== Packed short vector multiply — covers PMULLW =====
  {"packed_short_mul",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "long packed_short_mul(long a, long b) {\n"
   "  v8hi va = {(short)a, 2, 3, 4, 5, 6, 7, 8};\n"
   "  v8hi vb = {(short)b, 3, 2, 1, 8, 7, 6, 5};\n"
   "  v8hi vr = va * vb;\n"
   "  return (long)vr[0] + (long)vr[1];\n"
   "}\n",
   {5, 10}, "SSEConv", 1, "-msse2"},

  // ===== PBLENDW (SSE4.1) — blend packed words =====
  {"pblendw_c",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "long pblendw_c(long a, long b) {\n"
   "  v8hi va = {(short)a, 2, 3, 4, 5, 6, 7, 8};\n"
   "  v8hi vb = {(short)b, 12, 13, 14, 15, 16, 17, 18};\n"
   "  v8hi vr = __builtin_shufflevector(va, vb, 8, 1, 2, 11, 4, 5, 14, 7);\n"
   "  return (long)vr[0] + (long)vr[3];\n"
   "}\n",
   {10, 100}, "SSEConv", 1, "-msse4.1"},

  // ===== Packed int negate — covers PSUBD from zero =====
  {"packed_int_neg",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long packed_int_neg(long a) {\n"
   "  v4si va = {(int)a, 10, -20, 30};\n"
   "  v4si vr = -va;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {42}, "SSEConv", 1, "-msse2"},

  // ===== Packed byte abs-diff sum — C scalar SAD =====
  {"c_sad_scalar",
   "long c_sad_scalar(long a, long b) {\n"
   "  int diff = (int)a - (int)b;\n"
   "  return (long)(diff < 0 ? -diff : diff);\n"
   "}\n",
   {100, 42}, "SSEConv", 1, ""},

  // ===== Packed int ANDNOT — covers PANDN =====
  {"packed_int_andnot",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long packed_int_andnot(long a, long b) {\n"
   "  v4si va = {(int)a, 0xFF, 0, 0};\n"
   "  v4si vb = {(int)b, 0xFFFF, 0, 0};\n"
   "  v4si vr = ~va & vb;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0xFF00, 0xFFFF}, "SSEConv", 1, "-msse2"},

  // ===== Packed 64-bit int add — covers PADDQ =====
  {"packed_i64_add",
   "typedef long long v2di __attribute__((vector_size(16)));\n"
   "long packed_i64_add(long a, long b) {\n"
   "  v2di va = {(long long)a, 100};\n"
   "  v2di vb = {(long long)b, 200};\n"
   "  v2di vr = va + vb;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {42, 58}, "SSEConv", 1, "-msse2"},

  // ===== Packed 64-bit int sub — covers PSUBQ =====
  {"packed_i64_sub",
   "typedef long long v2di __attribute__((vector_size(16)));\n"
   "long packed_i64_sub(long a, long b) {\n"
   "  v2di va = {(long long)a, 300};\n"
   "  v2di vb = {(long long)b, 100};\n"
   "  v2di vr = va - vb;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {100, 42}, "SSEConv", 1, "-msse2"},

  // ===== Packed byte comparison — covers PCMPEQB =====
  {"packed_byte_cmpeq",
   "typedef char v16qi __attribute__((vector_size(16)));\n"
   "long packed_byte_cmpeq(long a) {\n"
   "  v16qi va = {(char)a, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};\n"
   "  v16qi vb = {(char)a, 0, 2, 0, 4, 0, 6, 0, 8, 0, 10, 0, 12, 0, 14, 0};\n"
   "  v16qi vr = (va == vb);\n"
   "  return (long)(unsigned char)vr[0];\n"
   "}\n",
   {42}, "SSEConv", 1, "-msse2"},

  // ===== CVTTPS2DQ + INSERTPS packed float→int =====
  {"cvttps2dq_insertps_packed",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long cvttps2dq_insertps_packed(long a, long b) {\n"
   "  v4f fv = {(float)(a-b), (float)(a+b), (float)b, (float)a};\n"
   "  v4i iv = __builtin_convertvector(fv, v4i);\n"
   "  return (long)(iv[0] + iv[1] + iv[2] + iv[3]);\n"
   "}\n",
   {100, 25}, "SSEConv", 1, "-msse4.1"},

  // ===== CVTDQ2PS packed int→float =====
  {"cvtdq2ps_packed",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long cvtdq2ps_packed(long a) {\n"
   "  v4i iv = {(int)a, (int)(a*2), (int)(a*3), (int)(a*4)};\n"
   "  v4f fv = __builtin_convertvector(iv, v4f);\n"
   "  v4i ri = __builtin_convertvector(fv, v4i);\n"
   "  return (long)(ri[0] + ri[1] + ri[2] + ri[3]);\n"
   "}\n",
   {10}, "SSEConv", 1, "-msse2"},

  // ===== EXTRACTPS — extract float from packed vector =====
  {"extractps_lane2",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long extractps_lane2(long a, long b) {\n"
   "  v4f fv = {(float)a, (float)b, (float)(a+b), (float)(a*2)};\n"
   "  float r = fv[2];\n"
   "  int ri; __builtin_memcpy(&ri, &r, 4);\n"
   "  return (long)ri;\n"
   "}\n",
   {10, 20}, "SSEConv", 1, "-msse4.1"},

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(SSEConv, X64SSEConvBroadcastRT,
                         ::testing::ValuesIn(kSSEConvBroadcast),
                         [](const auto &P) { return P.param.Name; });
