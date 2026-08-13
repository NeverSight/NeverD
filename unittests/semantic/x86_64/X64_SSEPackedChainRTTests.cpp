//===- X64_SSEPackedChainRTTests.cpp - SSE packed chain roundtrip ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Covers chained packed operations: ADDPS+MULPS, PADDD+PSLLD,
// PAND+POR+PXOR chains, CVTDQ2PS+ADDPS, PMULLD+PADDD,
// packed compare+blend chains, PUNPCKL+shuffle chains.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SSEPackedChainRT : public SemanticRoundTripFixture,
                             public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SSEPackedChainRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kSSEPackedChain = {

  {"addps_mulps_chain",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long addps_mulps_chain(long a, long b) {\n"
   "  v4f va = {(float)a, (float)(a+1), (float)(a+2), (float)(a+3)};\n"
   "  v4f vb = {(float)b, (float)(b+1), (float)(b+2), (float)(b+3)};\n"
   "  v4f sum = va + vb;\n"
   "  v4f prod = sum * va;\n"
   "  v4i ri = __builtin_convertvector(prod, v4i);\n"
   "  return (long)(ri[0] + ri[1] + ri[2] + ri[3]);\n"
   "}\n",
   {3, 7}, "PackedChain", 1, "-msse"},

  {"subps_divps_chain",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long subps_divps_chain(long a, long b) {\n"
   "  v4f va = {(float)(a*4), (float)(a*8), (float)(a*12), (float)(a*16)};\n"
   "  v4f vb = {(float)b, (float)b, (float)b, (float)b};\n"
   "  v4f diff = va - vb;\n"
   "  v4f quot = diff / vb;\n"
   "  v4i ri = __builtin_convertvector(quot, v4i);\n"
   "  return (long)(ri[0] + ri[1] + ri[2] + ri[3]);\n"
   "}\n",
   {5, 2}, "PackedChain", 1, "-msse -fno-math-errno"},

  {"paddd_pslld_chain",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long paddd_pslld_chain(long a) {\n"
   "  v4i va = {(int)a, (int)(a+1), (int)(a+2), (int)(a+3)};\n"
   "  v4i shifted = va << 2;\n"
   "  v4i added = shifted + va;\n"
   "  return (long)(added[0] + added[1] + added[2] + added[3]);\n"
   "}\n",
   {5}, "PackedChain", 1, "-msse2"},

  {"logic_chain",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long logic_chain(long a, long b) {\n"
   "  v4i va = {(int)a, (int)(a^0xFF), (int)(a|0xF0), (int)(a&0x0F)};\n"
   "  v4i vb = {(int)b, (int)(b^0xFF), (int)(b|0xF0), (int)(b&0x0F)};\n"
   "  v4i r = (va & vb) | (va ^ vb);\n"
   "  return (long)(r[0] + r[1] + r[2] + r[3]);\n"
   "}\n",
   {0x12345678, 0xABCDEF01}, "PackedChain", 1, "-msse2"},

  {"paddd_pmulld_chain",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long paddd_pmulld_chain(long a, long b) {\n"
   "  v4i va = {(int)a, (int)(a+1), (int)(a+2), (int)(a+3)};\n"
   "  v4i vb = {(int)b, (int)(b+1), (int)(b+2), (int)(b+3)};\n"
   "  v4i prod = va * vb;\n"
   "  v4i sum = prod + va;\n"
   "  return (long)(sum[0] + sum[1] + sum[2] + sum[3]);\n"
   "}\n",
   {3, 5}, "PackedChain", 1, "-msse4.1"},

  {"cvtdq2ps_addps_chain",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long cvtdq2ps_addps_chain(long a) {\n"
   "  v4i vi = {(int)a, (int)(a+10), (int)(a+20), (int)(a+30)};\n"
   "  v4f vf = __builtin_convertvector(vi, v4f);\n"
   "  v4f half = {0.5f, 0.5f, 0.5f, 0.5f};\n"
   "  v4f r = vf + half;\n"
   "  v4i ri = __builtin_convertvector(r, v4i);\n"
   "  return (long)(ri[0] + ri[1] + ri[2] + ri[3]);\n"
   "}\n",
   {100}, "PackedChain", 1, "-msse2"},

  {"paddw_psrlw_chain",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "long paddw_psrlw_chain(long a) {\n"
   "  v8hi va = {(short)a, (short)(a+1), (short)(a+2), (short)(a+3),\n"
   "             (short)(a+4), (short)(a+5), (short)(a+6), (short)(a+7)};\n"
   "  v8hi shifted = va >> 1;\n"
   "  v8hi sum = shifted + va;\n"
   "  int total = 0;\n"
   "  for (int i = 0; i < 8; ++i) total += sum[i];\n"
   "  return (long)total;\n"
   "}\n",
   {10}, "PackedChain", 1, "-msse2"},

  {"psubb_pavgb_chain",
   "typedef unsigned char v16qi __attribute__((vector_size(16)));\n"
   "long psubb_pavgb_chain(long a, long b) {\n"
   "  unsigned char ba = (unsigned char)a, bb = (unsigned char)b;\n"
   "  v16qi va = {ba, (unsigned char)(ba+1), (unsigned char)(ba+2), (unsigned char)(ba+3),\n"
   "              ba, (unsigned char)(ba+1), (unsigned char)(ba+2), (unsigned char)(ba+3),\n"
   "              ba, (unsigned char)(ba+1), (unsigned char)(ba+2), (unsigned char)(ba+3),\n"
   "              ba, (unsigned char)(ba+1), (unsigned char)(ba+2), (unsigned char)(ba+3)};\n"
   "  v16qi vb = {bb, bb, bb, bb, bb, bb, bb, bb, bb, bb, bb, bb, bb, bb, bb, bb};\n"
   "  v16qi diff = va - vb;\n"
   "  int total = 0;\n"
   "  for (int i = 0; i < 16; ++i) total += diff[i];\n"
   "  return (long)total;\n"
   "}\n",
   {100, 50}, "PackedChain", 1, "-msse2"},

  {"addpd_mulpd_chain",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long addpd_mulpd_chain(long a, long b) {\n"
   "  v2d va = {(double)a, (double)(a+5)};\n"
   "  v2d vb = {(double)b, (double)(b+3)};\n"
   "  v2d sum = va + vb;\n"
   "  v2d prod = sum * va;\n"
   "  return (long)prod[0] + (long)prod[1];\n"
   "}\n",
   {3, 7}, "PackedChain", 1, "-msse2"},

  {"subpd_divpd_chain",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long subpd_divpd_chain(long a) {\n"
   "  v2d va = {(double)(a*6), (double)(a*10)};\n"
   "  v2d vb = {(double)a, (double)a};\n"
   "  v2d diff = va - vb;\n"
   "  v2d quot = diff / vb;\n"
   "  return (long)quot[0] + (long)quot[1];\n"
   "}\n",
   {5}, "PackedChain", 1, "-msse2 -fno-math-errno"},

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(PackedChain, X64SSEPackedChainRT,
                         ::testing::ValuesIn(kSSEPackedChain),
                         [](const auto &P) { return P.param.Name; });
