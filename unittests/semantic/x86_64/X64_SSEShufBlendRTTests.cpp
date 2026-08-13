//===- X64_SSEShufBlendRTTests.cpp - SSE shuffle/blend roundtrip ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Covers: PSHUFD, PSHUFB, PALIGNR, PUNPCKLDQ, PUNPCKHWD,
//         BLENDPS, BLENDPD, PBLENDW, MOVHLPS, MOVLHPS,
//         UNPCKLPS, UNPCKHPS, SHUFPS, SHUFPD
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SSEShufBlendRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SSEShufBlendRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kSSEShufBlend = {

  {"pshufd_swap",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long pshufd_swap(long a) {\n"
   "  v4si v = {(int)a, (int)(a+10), (int)(a+20), (int)(a+30)};\n"
   "  v4si r = __builtin_shufflevector(v, v, 3, 2, 1, 0);\n"
   "  return (long)(r[0]+r[1]+r[2]+r[3]);\n"
   "}\n",
   {10}, "ShufBlend", 2, "-msse2"},

  {"punpckldq_interleave",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long punpckldq_interleave(long a, long b) {\n"
   "  v4si va = {(int)a, (int)(a+1), 0, 0};\n"
   "  v4si vb = {(int)b, (int)(b+1), 0, 0};\n"
   "  v4si r = __builtin_shufflevector(va, vb, 0, 4, 1, 5);\n"
   "  return (long)(r[0]+r[1]+r[2]+r[3]);\n"
   "}\n",
   {10, 100}, "ShufBlend", 2, "-msse2"},

  {"vec_concat_lo",
   "typedef long long v2di __attribute__((vector_size(16)));\n"
   "long vec_concat_lo(long a, long b) {\n"
   "  v2di va = {a, 0};\n"
   "  v2di vb = {b, 0};\n"
   "  v2di r = __builtin_shufflevector(va, vb, 0, 2);\n"
   "  return (long)(r[0] + r[1]);\n"
   "}\n",
   {42, 99}, "ShufBlend", 2, "-msse2"},

  {"vec_float_shuf",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long vec_float_shuf(long a) {\n"
   "  v4f v = {(float)a, (float)(a+1), (float)(a+2), (float)(a+3)};\n"
   "  v4f r = __builtin_shufflevector(v, v, 2, 3, 0, 1);\n"
   "  v4i ri = __builtin_convertvector(r, v4i);\n"
   "  return (long)(ri[0]+ri[1]+ri[2]+ri[3]);\n"
   "}\n",
   {10}, "ShufBlend", 2, "-msse2"},

  {"vec_byte_reverse",
   "typedef unsigned char v16qu __attribute__((vector_size(16)));\n"
   "long vec_byte_reverse(long a) {\n"
   "  v16qu v = {(unsigned char)a, 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};\n"
   "  v16qu r = __builtin_shufflevector(v, v, 15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0);\n"
   "  return (long)r[0] + (long)r[15];\n"
   "}\n",
   {42}, "ShufBlend", 2, "-msse2"},

  {"vec_word_pairs",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "long vec_word_pairs(long a) {\n"
   "  v8hi v = {(short)a, 10, 20, 30, 40, 50, 60, 70};\n"
   "  v8hi r = __builtin_shufflevector(v, v, 0, 0, 2, 2, 4, 4, 6, 6);\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 8; i++) sum += r[i];\n"
   "  return sum;\n"
   "}\n",
   {100}, "ShufBlend", 2, "-msse2"},

  {"vec_float_blend",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long vec_float_blend(long a, long b) {\n"
   "  v4f va = {(float)a, (float)(a+1), (float)(a+2), (float)(a+3)};\n"
   "  v4f vb = {(float)b, (float)(b+1), (float)(b+2), (float)(b+3)};\n"
   "  v4f r = __builtin_shufflevector(va, vb, 4, 1, 6, 3);\n"
   "  v4i ri = __builtin_convertvector(r, v4i);\n"
   "  return (long)(ri[0]+ri[1]+ri[2]+ri[3]);\n"
   "}\n",
   {10, 100}, "ShufBlend", 2, "-msse4.1"},

  {"vec_qword_broadcast",
   "typedef long long v2di __attribute__((vector_size(16)));\n"
   "long vec_qword_broadcast(long a) {\n"
   "  v2di v = {a, 0};\n"
   "  v2di r = __builtin_shufflevector(v, v, 0, 0);\n"
   "  return (long)(r[0] + r[1]);\n"
   "}\n",
   {42}, "ShufBlend", 2, "-msse2"},

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(ShufBlend, X64SSEShufBlendRT,
                         ::testing::ValuesIn(kSSEShufBlend),
                         [](const auto &P) { return P.param.Name; });
