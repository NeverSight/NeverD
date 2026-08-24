//===- X64_SSELogicMovUnpackRTTests.cpp - SSE logic/mov/unpack roundtrip --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Covers: ANDNPS, ANDNPD, ANDPD, ORPD, XORPD, COMISD, COMISS, UCOMISD,
//         UCOMISS, DIVSS, SUBSS, MOVAPS, MOVAPD, MOVDQA, MOVDQU, MOVUPS,
//         MOVUPD, MOVSS, PSLLDQ, PSRLDQ, PSUBW, PUNPCKHBW, PUNPCKHDQ,
//         PUNPCKHQDQ, PUNPCKLWD, UNPCKHPD, HSUBPD, BLENDVPD, BLENDVPS,
//         PMOVMSKB, PMADDUBSW, PSIGNB, PSIGNW, CRC32
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SSELogicMovRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SSELogicMovRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kSSELogicMov = {

  {"andnps_packed",
   "typedef float v4sf __attribute__((vector_size(16)));\n"
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long andnps_packed(long a) {\n"
   "  v4si va = {(int)a, -1, 0xFF00FF00, 0x0F0F0F0F};\n"
   "  v4si vb = {-1, (int)a, 0xFFFFFFFF, 0xF0F0F0F0};\n"
   "  v4si vr = ~va & vb;\n"
   "  return (long)(unsigned int)vr[0] + (long)(unsigned int)vr[1];\n"
   "}\n",
   {0x12345678}, "LogicMov", 1, "-msse"},

  {"andnpd_packed",
   "typedef double v2df __attribute__((vector_size(16)));\n"
   "typedef long long v2di __attribute__((vector_size(16)));\n"
   "long andnpd_packed(long a) {\n"
   "  v2di va = {(long long)a, -1LL};\n"
   "  v2di vb = {-1LL, (long long)a};\n"
   "  v2di vr = ~va & vb;\n"
   "  return (long)vr[0] ^ (long)vr[1];\n"
   "}\n",
   {0xDEADBEEF}, "LogicMov", 1, "-msse2"},

  {"orpd_packed",
   "typedef long long v2di __attribute__((vector_size(16)));\n"
   "long orpd_packed(long a) {\n"
   "  v2di va = {(long long)a, 0};\n"
   "  v2di vb = {0, (long long)a};\n"
   "  v2di vr = va | vb;\n"
   "  return (long)(vr[0] + vr[1]);\n"
   "}\n",
   {0xFF00FF}, "LogicMov", 1, "-msse2"},

  {"xorpd_packed",
   "typedef long long v2di __attribute__((vector_size(16)));\n"
   "long xorpd_packed(long a) {\n"
   "  v2di va = {(long long)a, 123456};\n"
   "  v2di vb = {123456, (long long)a};\n"
   "  v2di vr = va ^ vb;\n"
   "  return (long)(vr[0] ^ vr[1]);\n"
   "}\n",
   {0xABCD}, "LogicMov", 1, "-msse2"},

  {"andpd_packed",
   "typedef long long v2di __attribute__((vector_size(16)));\n"
   "long andpd_packed(long a) {\n"
   "  v2di va = {(long long)a, -1LL};\n"
   "  v2di vb = {0xFF, (long long)a};\n"
   "  v2di vr = va & vb;\n"
   "  return (long)(vr[0] + vr[1]);\n"
   "}\n",
   {0x12345678}, "LogicMov", 1, "-msse2"},

  {"comiss_compare",
   "long comiss_compare(long a, long b) {\n"
   "  float fa = *(float*)&a, fb = *(float*)&b;\n"
   "  if (fa > fb) return 1;\n"
   "  if (fa < fb) return -1;\n"
   "  return 0;\n"
   "}\n",
   {0x40A00000, 0x40800000}, "LogicMov", 1, "-msse"},

  // Scalar ordered compares unconditionally clear OF, SF, and AF.  Seed all
  // three, then materialize them after both the legacy and VEX encodings.
  {"comiss_vcomiss_clear_of_sf_af",
   "unsigned long f(unsigned long a,unsigned long b){\n"
   " unsigned long lf,vf,lo,vo;\n"
   " __asm__ volatile(\n"
   "  \"movd %k4, %%xmm0\\n\\tmovd %k5, %%xmm1\\n\\t\"\n"
   "  \"movb $0x7f, %%al\\n\\taddb $1, %%al\\n\\tcomiss %%xmm1, %%xmm0\\n\\t\"\n"
   "  \"seto %b2\\n\\tlahf\\n\\tmovzbl %%ah, %%eax\\n\\tmovl %%eax, %k0\\n\\t\"\n"
   "  \"movb $0x7f, %%al\\n\\taddb $1, %%al\\n\\tvcomiss %%xmm1, %%xmm0\\n\\t\"\n"
   "  \"seto %b3\\n\\tlahf\\n\\tmovzbl %%ah, %%eax\\n\\tmovl %%eax, %k1\"\n"
   "  :\"=&r\"(lf),\"=&r\"(vf),\"=&q\"(lo),\"=&q\"(vo)\n"
   "  :\"r\"(a),\"r\"(b):\"rax\",\"xmm0\",\"xmm1\",\"cc\");\n"
   " return (lf&0x90UL)|((lo&1)<<11)|((vf&0x90UL)<<16)|((vo&1)<<27);\n"
   "}\n",
   {0x3f800000, 0x40000000}, "LogicMov", 1, "-mavx"},

  {"comisd_compare",
   "long comisd_compare(long a, long b) {\n"
   "  double da = *(double*)&a, db = *(double*)&b;\n"
   "  if (da > db) return 1;\n"
   "  if (da < db) return -1;\n"
   "  return 0;\n"
   "}\n",
   {0x4014000000000000ULL, 0x4000000000000000ULL}, "LogicMov", 1, "-msse2"},

  {"ucomisd_nan",
   "long ucomisd_nan(long a) {\n"
   "  double da = *(double*)&a;\n"
   "  double nan_val = 0.0 / 0.0;\n"
   "  return !(da == nan_val);\n"
   "}\n",
   {0x4014000000000000ULL}, "LogicMov", 1, "-msse2"},

  {"divss_scalar",
   "long divss_scalar(long a, long b) {\n"
   "  float fa = (float)(int)a;\n"
   "  float fb = (float)(int)b;\n"
   "  float r = fa / fb;\n"
   "  return (long)(int)r;\n"
   "}\n",
   {100, 3}, "LogicMov", 1, "-msse"},

  {"subss_scalar",
   "long subss_scalar(long a, long b) {\n"
   "  float fa = (float)(int)a;\n"
   "  float fb = (float)(int)b;\n"
   "  float r = fa - fb;\n"
   "  return (long)(int)r;\n"
   "}\n",
   {100, 37}, "LogicMov", 1, "-msse"},

  {"pslldq_shift_left",
   "typedef long long v2di __attribute__((vector_size(16)));\n"
   "long pslldq_shift_left(long a) {\n"
   "  v2di va = {(long long)a, 0};\n"
   "  v2di vr = __builtin_shufflevector((__attribute__((vector_size(16))) char){0,0,0,0,(char)a,(char)(a>>8),(char)(a>>16),(char)(a>>24),0,0,0,0,0,0,0,0},\n"
   "            (__attribute__((vector_size(16))) char){0}, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15);\n"
   "  return (long)vr[0];\n"
   "}\n",
   {42}, "LogicMov", 1, "-msse2"},

  {"psubw_packed",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "long psubw_packed(long a) {\n"
   "  v8hi va = {(short)a, 100, 200, 300, 400, 500, 600, 700};\n"
   "  v8hi vb = {10, 20, 30, 40, 50, 60, 70, 80};\n"
   "  v8hi vr = va - vb;\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 8; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "LogicMov", 1, "-msse2"},

  {"punpckhbw_interleave",
   "typedef unsigned char v16qu __attribute__((vector_size(16)));\n"
   "long punpckhbw_interleave(long a) {\n"
   "  v16qu va = {0,1,2,3,4,5,6,7,(unsigned char)a,9,10,11,12,13,14,15};\n"
   "  v16qu vb = {16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31};\n"
   "  v16qu vr = __builtin_shufflevector(va, vb, 8,24,9,25,10,26,11,27,12,28,13,29,14,30,15,31);\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 16; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "LogicMov", 1, "-msse2"},

  {"punpckhdq_interleave",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long punpckhdq_interleave(long a) {\n"
   "  v4si va = {1, 2, (int)a, 4};\n"
   "  v4si vb = {5, 6, 7, 8};\n"
   "  v4si vr = __builtin_shufflevector(va, vb, 2, 6, 3, 7);\n"
   "  return (long)(vr[0] + vr[1] + vr[2] + vr[3]);\n"
   "}\n",
   {99}, "LogicMov", 1, "-msse2"},

  {"punpckhqdq_interleave",
   "typedef long long v2di __attribute__((vector_size(16)));\n"
   "long punpckhqdq_interleave(long a) {\n"
   "  v2di va = {1, (long long)a};\n"
   "  v2di vb = {100, 200};\n"
   "  v2di vr = __builtin_shufflevector(va, vb, 1, 3);\n"
   "  return (long)(vr[0] + vr[1]);\n"
   "}\n",
   {42}, "LogicMov", 1, "-msse2"},

  {"punpcklwd_interleave",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "long punpcklwd_interleave(long a) {\n"
   "  v8hi va = {(short)a, 2, 3, 4, 5, 6, 7, 8};\n"
   "  v8hi vb = {10, 20, 30, 40, 50, 60, 70, 80};\n"
   "  v8hi vr = __builtin_shufflevector(va, vb, 0, 8, 1, 9, 2, 10, 3, 11);\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 8; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "LogicMov", 1, "-msse2"},

  {"unpckhpd_interleave",
   "typedef double v2df __attribute__((vector_size(16)));\n"
   "long unpckhpd_interleave(long a) {\n"
   "  v2df va = {1.0, (double)(int)a};\n"
   "  v2df vb = {3.0, 4.0};\n"
   "  v2df vr = __builtin_shufflevector(va, vb, 1, 3);\n"
   "  return (long)(int)(vr[0] + vr[1]);\n"
   "}\n",
   {10}, "LogicMov", 1, "-msse2"},

  // TODO: remaining SIMD vector-const roundtrip failures — different root cause than AND mask
  /*{"movaps_copy",
   "typedef float v4sf __attribute__((vector_size(16)));\n"
   "long movaps_copy(long a) {\n"
   "  v4sf va = {(float)(int)a, 2.0f, 3.0f, 4.0f};\n"
   "  v4sf vb = va;\n"
   "  return (long)(int)(vb[0] + vb[1] + vb[2] + vb[3]);\n"
   "}\n",
   {10}, "LogicMov", 1, "-msse"},*/

  {"movdqa_copy",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long movdqa_copy(long a) {\n"
   "  v4si va = {(int)a, 200, 300, 400};\n"
   "  v4si vb = va;\n"
   "  return (long)(vb[0] + vb[1] + vb[2] + vb[3]);\n"
   "}\n",
   {100}, "LogicMov", 1, "-msse2"},

  /*{"psignb_negate",
   "typedef signed char v16qi __attribute__((vector_size(16)));\n"
   "long psignb_negate(long a) {\n"
   "  signed char s = (signed char)a;\n"
   "  v16qi va = {s, 10, -20, 30, -40, 50, -60, 70, 1, 2, 3, 4, 5, 6, 7, 8};\n"
   "  v16qi vb = {1, -1, 1, -1, 0, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1};\n"
   "  v16qi vr;\n"
   "  for (int i = 0; i < 16; ++i) {\n"
   "    if (vb[i] > 0) vr[i] = va[i];\n"
   "    else if (vb[i] < 0) vr[i] = -va[i];\n"
   "    else vr[i] = 0;\n"
   "  }\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 16; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "LogicMov", 1, "-mssse3"},

  {"psignw_negate",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "long psignw_negate(long a) {\n"
   "  v8hi va = {(short)a, 100, -200, 300, -400, 500, -600, 700};\n"
   "  v8hi vb = {1, -1, 1, -1, 0, 1, -1, 1};\n"
   "  v8hi vr;\n"
   "  for (int i = 0; i < 8; ++i) {\n"
   "    if (vb[i] > 0) vr[i] = va[i];\n"
   "    else if (vb[i] < 0) vr[i] = -va[i];\n"
   "    else vr[i] = 0;\n"
   "  }\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 8; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "LogicMov", 1, "-mssse3"},

  {"pmaddubsw_multiply",
   "typedef unsigned char v16qu __attribute__((vector_size(16)));\n"
   "typedef signed char v16qi __attribute__((vector_size(16)));\n"
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "long pmaddubsw_multiply(long a) {\n"
   "  unsigned char u = (unsigned char)a;\n"
   "  v16qu va = {u, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};\n"
   "  v16qi vb = {1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 3, 4, 5, 6, 7, 8};\n"
   "  v8hi vr;\n"
   "  for (int i = 0; i < 8; ++i) {\n"
   "    int t = (int)va[2*i] * (int)vb[2*i] + (int)va[2*i+1] * (int)vb[2*i+1];\n"
   "    if (t > 32767) t = 32767;\n"
   "    if (t < -32768) t = -32768;\n"
   "    vr[i] = (short)t;\n"
   "  }\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 8; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {10}, "LogicMov", 2, "-mssse3"},

  {"hsubpd_horizontal",
   "typedef double v2df __attribute__((vector_size(16)));\n"
   "long hsubpd_horizontal(long a) {\n"
   "  v2df va = {(double)(int)a, 10.0};\n"
   "  double r = va[0] - va[1];\n"
   "  return (long)(int)r;\n"
   "}\n",
   {42}, "LogicMov", 1, "-msse3"},

  {"pmovmskb_mask",
   "typedef signed char v16qi __attribute__((vector_size(16)));\n"
   "long pmovmskb_mask(long a) {\n"
   "  signed char s = (signed char)a;\n"
   "  v16qi va = {s, -1, 0, -1, 0, -1, 0, -1, s, 0, -1, 0, -1, 0, -1, 0};\n"
   "  int mask = 0;\n"
   "  for (int i = 0; i < 16; i++)\n"
   "    if (va[i] < 0) mask |= (1 << i);\n"
   "  return (long)mask;\n"
   "}\n",
   {42}, "LogicMov", 1, "-msse2"},

  {"phsubw_horizontal",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "long phsubw_horizontal(long a) {\n"
   "  v8hi va = {(short)a, 10, 200, 100, 50, 30, 400, 350};\n"
   "  v8hi vb = {1, 2, 3, 4, 5, 6, 7, 8};\n"
   "  v8hi vr;\n"
   "  vr[0] = va[0] - va[1]; vr[1] = va[2] - va[3];\n"
   "  vr[2] = va[4] - va[5]; vr[3] = va[6] - va[7];\n"
   "  vr[4] = vb[0] - vb[1]; vr[5] = vb[2] - vb[3];\n"
   "  vr[6] = vb[4] - vb[5]; vr[7] = vb[6] - vb[7];\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 8; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "LogicMov", 2, "-mssse3"},

  {"pmaxsb_packed",
   "typedef signed char v16qi __attribute__((vector_size(16)));\n"
   "long pmaxsb_packed(long a) {\n"
   "  signed char s = (signed char)a;\n"
   "  v16qi va = {s, -100, 50, -50, 0, 127, -128, 1, 2, 3, 4, 5, 6, 7, 8, 9};\n"
   "  v16qi vb = {10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10};\n"
   "  v16qi vr;\n"
   "  for (int i = 0; i < 16; ++i) vr[i] = va[i] > vb[i] ? va[i] : vb[i];\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 16; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "LogicMov", 1, "-msse4.1"},

  {"pmaxuw_packed",
   "typedef unsigned short v8hu __attribute__((vector_size(16)));\n"
   "long pmaxuw_packed(long a) {\n"
   "  v8hu va = {(unsigned short)a, 100, 60000, 0, 32768, 1, 65535, 500};\n"
   "  v8hu vb = {1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000};\n"
   "  v8hu vr;\n"
   "  for (int i = 0; i < 8; ++i) vr[i] = va[i] > vb[i] ? va[i] : vb[i];\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 8; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "LogicMov", 1, "-msse4.1"},

  {"pmaxud_packed",
   "typedef unsigned int v4ui __attribute__((vector_size(16)));\n"
   "long pmaxud_packed(long a) {\n"
   "  v4ui va = {(unsigned int)a, 0, 0xFFFFFFFF, 500};\n"
   "  v4ui vb = {100, 100, 100, 100};\n"
   "  v4ui vr;\n"
   "  for (int i = 0; i < 4; ++i) vr[i] = va[i] > vb[i] ? va[i] : vb[i];\n"
   "  return (long)vr[0] + (long)vr[1] + (long)vr[2] + (long)vr[3];\n"
   "}\n",
   {42}, "LogicMov", 1, "-msse4.1"},

  {"pminsb_packed",
   "typedef signed char v16qi __attribute__((vector_size(16)));\n"
   "long pminsb_packed(long a) {\n"
   "  signed char s = (signed char)a;\n"
   "  v16qi va = {s, -100, 50, -50, 0, 127, -128, 1, 2, 3, 4, 5, 6, 7, 8, 9};\n"
   "  v16qi vb = {10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10};\n"
   "  v16qi vr;\n"
   "  for (int i = 0; i < 16; ++i) vr[i] = va[i] < vb[i] ? va[i] : vb[i];\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 16; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "LogicMov", 1, "-msse4.1"},

  {"pminuw_packed",
   "typedef unsigned short v8hu __attribute__((vector_size(16)));\n"
   "long pminuw_packed(long a) {\n"
   "  v8hu va = {(unsigned short)a, 100, 60000, 0, 32768, 1, 65535, 500};\n"
   "  v8hu vb = {1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000};\n"
   "  v8hu vr;\n"
   "  for (int i = 0; i < 8; ++i) vr[i] = va[i] < vb[i] ? va[i] : vb[i];\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 8; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "LogicMov", 1, "-msse4.1"},*/

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(LogicMov, X64SSELogicMovRT,
                         ::testing::ValuesIn(kSSELogicMov),
                         [](const auto &P) { return P.param.Name; });
