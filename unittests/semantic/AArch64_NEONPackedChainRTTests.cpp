//===- AArch64_NEONPackedChainRTTests.cpp - NEON packed chain roundtrip ---===//
//
// Covers chained NEON vector operations: ADD+MUL, SHL+ADD, AND+OR+EOR chains,
// SADDL+SSUBL widening chains, FCVTZS+ADD int-from-float chains,
// MLA+accumulate chains, packed compare+select chains.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64NEONPackedChainRT
    : public SemanticRoundTripFixture,
      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64NEONPackedChainRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kA64NEONPackedChain = {

  {"add_mul_v4i32",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long add_mul_v4i32(long a, long b) {\n"
   "  v4i va = {(int)a, (int)(a+1), (int)(a+2), (int)(a+3)};\n"
   "  v4i vb = {(int)b, (int)(b+1), (int)(b+2), (int)(b+3)};\n"
   "  v4i sum = va + vb;\n"
   "  v4i prod = sum * va;\n"
   "  return (long)(prod[0] + prod[1] + prod[2] + prod[3]);\n"
   "}\n",
   {3, 5}, "NEONChain", 1, ""},

  {"sub_shl_v4i32",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long sub_shl_v4i32(long a) {\n"
   "  v4i va = {(int)a, (int)(a+1), (int)(a+2), (int)(a+3)};\n"
   "  v4i shifted = va << 2;\n"
   "  v4i diff = shifted - va;\n"
   "  return (long)diff[0];\n"
   "}\n",
   {10}, "NEONChain", 1, ""},

  {"logic_chain_v4i32",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long logic_chain_v4i32(long a, long b) {\n"
   "  v4i va = {(int)a, (int)(a^0xFF), (int)(a|0xF0), (int)(a&0x0F)};\n"
   "  v4i vb = {(int)b, (int)(b^0xFF), (int)(b|0xF0), (int)(b&0x0F)};\n"
   "  v4i r = (va & vb) | (va ^ vb);\n"
   "  return (long)(r[0] + r[1] + r[2] + r[3]);\n"
   "}\n",
   {0x12345678, 0xABCDEF01}, "NEONChain", 1, ""},

  {"add_v8i16_chain",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "long add_v8i16_chain(long a) {\n"
   "  v8hi va = {(short)a, (short)(a+1), (short)(a+2), (short)(a+3),\n"
   "             (short)(a+4), (short)(a+5), (short)(a+6), (short)(a+7)};\n"
   "  v8hi shifted = va >> 1;\n"
   "  v8hi sum = shifted + va;\n"
   "  int total = 0;\n"
   "  for (int i = 0; i < 8; ++i) total += sum[i];\n"
   "  return (long)total;\n"
   "}\n",
   {10}, "NEONChain", 1, ""},

  {"sub_v16i8_chain",
   "typedef unsigned char v16qi __attribute__((vector_size(16)));\n"
   "long sub_v16i8_chain(long a, long b) {\n"
   "  unsigned char ba = (unsigned char)a, bb = (unsigned char)b;\n"
   "  v16qi va = {ba, (unsigned char)(ba+1), (unsigned char)(ba+2), (unsigned char)(ba+3),\n"
   "              ba, (unsigned char)(ba+1), (unsigned char)(ba+2), (unsigned char)(ba+3),\n"
   "              ba, (unsigned char)(ba+1), (unsigned char)(ba+2), (unsigned char)(ba+3),\n"
   "              ba, (unsigned char)(ba+1), (unsigned char)(ba+2), (unsigned char)(ba+3)};\n"
   "  v16qi vb = {bb, bb, bb, bb, bb, bb, bb, bb, bb, bb, bb, bb, bb, bb, bb, bb};\n"
   "  v16qi diff = va - vb;\n"
   "  return (long)diff[0];\n"
   "}\n",
   {100, 50}, "NEONChain", 1, ""},

  {"fadd_fmul_v4f32",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long fadd_fmul_v4f32(long a, long b) {\n"
   "  v4f va = {(float)a, (float)(a+1), (float)(a+2), (float)(a+3)};\n"
   "  v4f vb = {(float)b, (float)(b+1), (float)(b+2), (float)(b+3)};\n"
   "  v4f sum = va + vb;\n"
   "  v4f prod = sum * va;\n"
   "  v4i ri = __builtin_convertvector(prod, v4i);\n"
   "  return (long)(ri[0] + ri[1] + ri[2] + ri[3]);\n"
   "}\n",
   {3, 7}, "NEONChain", 1, ""},

  {"fsub_v2f64",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long fsub_v2f64(long a, long b) {\n"
   "  v2d va = {(double)a, (double)(a+5)};\n"
   "  v2d vb = {(double)b, (double)(b+3)};\n"
   "  v2d diff = va - vb;\n"
   "  return (long)diff[0] + (long)diff[1];\n"
   "}\n",
   {20, 10}, "NEONChain", 1, ""},

  {"mul_accumulate_v4i32",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long mul_accumulate_v4i32(long a, long b) {\n"
   "  v4i va = {(int)a, (int)(a+1), (int)(a+2), (int)(a+3)};\n"
   "  v4i vb = {(int)b, (int)(b+1), (int)(b+2), (int)(b+3)};\n"
   "  v4i acc = va * vb + va;\n"
   "  return (long)acc[0];\n"
   "}\n",
   {2, 3}, "NEONChain", 1, ""},

  {"neg_abs_v4i32",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long neg_abs_v4i32(long a) {\n"
   "  v4i va = {(int)a, (int)(-a), (int)(a-5), (int)(5-a)};\n"
   "  v4i neg = -va;\n"
   "  v4i mask = neg >> 31;\n"
   "  v4i ab = (neg ^ mask) - mask;\n"
   "  return (long)(ab[0] + ab[1] + ab[2] + ab[3]);\n"
   "}\n",
   {3}, "NEONChain", 1, ""},

  // --- Bug #136 targeted: extract individual non-zero lanes ---
  {"mul_extract_lane1",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long mul_extract_lane1(long a, long b) {\n"
   "  v4i va = {(int)a, (int)(a+1), (int)(a+2), (int)(a+3)};\n"
   "  v4i vb = {(int)b, (int)(b+1), (int)(b+2), (int)(b+3)};\n"
   "  v4i prod = va * vb;\n"
   "  return (long)prod[1];\n"
   "}\n",
   {3, 5}, "NEONChain", 1, ""},

  {"add_extract_lane2",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long add_extract_lane2(long a, long b) {\n"
   "  v4i va = {(int)a, (int)(a+10), (int)(a+20), (int)(a+30)};\n"
   "  v4i vb = {(int)b, (int)(b+10), (int)(b+20), (int)(b+30)};\n"
   "  v4i sum = va + vb;\n"
   "  return (long)sum[2];\n"
   "}\n",
   {7, 11}, "NEONChain", 1, ""},

  {"shl_sub_extract_lane3",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long shl_sub_extract_lane3(long a) {\n"
   "  v4i va = {(int)a, (int)(a+1), (int)(a+2), (int)(a+3)};\n"
   "  v4i shifted = va << 3;\n"
   "  v4i diff = shifted - va;\n"
   "  return (long)diff[3];\n"
   "}\n",
   {5}, "NEONChain", 1, ""},

  {"mul_add_chain_lane2",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long mul_add_chain_lane2(long a, long b) {\n"
   "  v4i va = {(int)a, (int)(a+1), (int)(a+2), (int)(a+3)};\n"
   "  v4i vb = {(int)b, (int)(b+1), (int)(b+2), (int)(b+3)};\n"
   "  v4i prod = va * vb;\n"
   "  v4i acc = prod + va;\n"
   "  return (long)acc[2];\n"
   "}\n",
   {2, 3}, "NEONChain", 1, ""},

  {"logic_extract_lane1",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long logic_extract_lane1(long a, long b) {\n"
   "  v4i va = {(int)a, (int)(a^0xFF), (int)(a|0xF0), (int)(a&0x0F)};\n"
   "  v4i vb = {(int)b, (int)(b^0xFF), (int)(b|0xF0), (int)(b&0x0F)};\n"
   "  v4i r = (va & vb) | (va ^ vb);\n"
   "  return (long)r[1];\n"
   "}\n",
   {0x12345678, 0xABCDEF01}, "NEONChain", 1, ""},

  {"mul_extract_lane3_v4i32",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long mul_extract_lane3_v4i32(long a, long b) {\n"
   "  v4i va = {(int)a, (int)(a+1), (int)(a+2), (int)(a+3)};\n"
   "  v4i vb = {(int)b, (int)(b+1), (int)(b+2), (int)(b+3)};\n"
   "  v4i prod = va * vb;\n"
   "  return (long)prod[3];\n"
   "}\n",
   {5, 10}, "NEONChain", 1, ""},

  {"sub_extract_all_lanes",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long sub_extract_all_lanes(long a, long b) {\n"
   "  v4i va = {(int)a, (int)(a*2), (int)(a*3), (int)(a*4)};\n"
   "  v4i vb = {(int)b, (int)(b*2), (int)(b*3), (int)(b*4)};\n"
   "  v4i diff = va - vb;\n"
   "  long r = 0;\n"
   "  r += (long)diff[0] << 0;\n"
   "  r += (long)diff[1] << 8;\n"
   "  r += (long)diff[2] << 16;\n"
   "  r += (long)diff[3] << 24;\n"
   "  return r;\n"
   "}\n",
   {100, 30}, "NEONChain", 1, ""},

  {"shl_add_extract_lane2_v8i16",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "long shl_add_extract_lane2_v8i16(long a) {\n"
   "  v8hi va = {(short)a, (short)(a+1), (short)(a+2), (short)(a+3),\n"
   "             (short)(a+4), (short)(a+5), (short)(a+6), (short)(a+7)};\n"
   "  v8hi shifted = va << 1;\n"
   "  v8hi sum = shifted + va;\n"
   "  return (long)sum[2] + (long)sum[5];\n"
   "}\n",
   {10}, "NEONChain", 1, ""},

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(NEONChain, AArch64NEONPackedChainRT,
                         ::testing::ValuesIn(kA64NEONPackedChain),
                         [](const auto &P) { return P.param.Name; });
