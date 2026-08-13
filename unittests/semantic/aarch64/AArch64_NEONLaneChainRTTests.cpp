//===- AArch64_NEONLaneChainRTTests.cpp - NEON lane + chain roundtrip -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "SemanticRoundTripFixture.h"

class AArch64NEONLaneChainRT
    : public SemanticRoundTripFixture,
      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64NEONLaneChainRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kA64NEONLaneChain = {

  {"movi_broadcast_v4s",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long movi_broadcast_v4s(long a) {\n"
   "  v4i va = {(int)a, (int)(a+1), (int)(a+2), (int)(a+3)};\n"
   "  v4i mask = {0xFF, 0xFF, 0xFF, 0xFF};\n"
   "  v4i r = va & mask;\n"
   "  return (long)(r[0] + r[1] + r[2] + r[3]);\n"
   "}\n",
   {300}, "NEONLane", 1, ""},

  {"movi_broadcast_v8h",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "long movi_broadcast_v8h(long a) {\n"
   "  v8hi va = {(short)a, (short)(a+1), (short)(a+2), (short)(a+3),\n"
   "             (short)(a+4), (short)(a+5), (short)(a+6), (short)(a+7)};\n"
   "  v8hi mask = {0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F};\n"
   "  v8hi r = va & mask;\n"
   "  int total = 0;\n"
   "  for (int i = 0; i < 8; ++i) total += r[i];\n"
   "  return (long)total;\n"
   "}\n",
   {20}, "NEONLane", 1, ""},

  {"mvni_complement_v4s",
   "typedef unsigned int v4u __attribute__((vector_size(16)));\n"
   "long mvni_complement_v4s(long a) {\n"
   "  v4u va = {(unsigned)a, (unsigned)(a+1), (unsigned)(a+2), (unsigned)(a+3)};\n"
   "  v4u r = ~va;\n"
   "  return (long)(r[0] & 0xFFFF);\n"
   "}\n",
   {0x1234}, "NEONLane", 1, ""},

  {"dup_scalar_v4i32",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long dup_scalar_v4i32(long a) {\n"
   "  v4i va = {(int)a, (int)a, (int)a, (int)a};\n"
   "  v4i vb = {1, 2, 3, 4};\n"
   "  v4i r = va + vb;\n"
   "  return (long)(r[0] + r[1] + r[2] + r[3]);\n"
   "}\n",
   {10}, "NEONLane", 1, ""},

  {"neg_v4i32_lane2",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long neg_v4i32_lane2(long a) {\n"
   "  v4i va = {(int)a, (int)(a+1), (int)(a+2), (int)(a+3)};\n"
   "  v4i r = -va;\n"
   "  return (long)r[2];\n"
   "}\n",
   {5}, "NEONLane", 1, ""},

  {"shl_v4i32_lane3",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long shl_v4i32_lane3(long a) {\n"
   "  v4i va = {(int)a, (int)(a+1), (int)(a+2), (int)(a+3)};\n"
   "  v4i r = va << 4;\n"
   "  return (long)r[3];\n"
   "}\n",
   {5}, "NEONLane", 1, ""},

  {"sshr_v4i32_lane1",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long sshr_v4i32_lane1(long a) {\n"
   "  v4i va = {(int)(-a), (int)(-(a+1)), (int)(-(a+2)), (int)(-(a+3))};\n"
   "  v4i r = va >> 2;\n"
   "  return (long)r[1];\n"
   "}\n",
   {100}, "NEONLane", 1, ""},

  {"fadd_v4f32_extract_lane2",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long fadd_v4f32_extract_lane2(long a, long b) {\n"
   "  v4f va = {(float)a, (float)(a+1), (float)(a+2), (float)(a+3)};\n"
   "  v4f vb = {(float)b, (float)(b+1), (float)(b+2), (float)(b+3)};\n"
   "  v4f r = va + vb;\n"
   "  v4i ri = __builtin_convertvector(r, v4i);\n"
   "  return (long)ri[2];\n"
   "}\n",
   {3, 7}, "NEONLane", 1, ""},

  {"fmul_v4f32_lane1",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long fmul_v4f32_lane1(long a, long b) {\n"
   "  v4f va = {(float)a, (float)(a+1), (float)(a+2), (float)(a+3)};\n"
   "  v4f vb = {(float)b, (float)(b+1), (float)(b+2), (float)(b+3)};\n"
   "  v4f r = va * vb;\n"
   "  v4i ri = __builtin_convertvector(r, v4i);\n"
   "  return (long)ri[1];\n"
   "}\n",
   {3, 5}, "NEONLane", 1, ""},

  {"cmeq_v4i32_lane0",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long cmeq_v4i32_lane0(long a, long b) {\n"
   "  v4i va = {(int)a, (int)(a+1), (int)(a+2), (int)b};\n"
   "  v4i vb = {(int)b, (int)(b+1), (int)(a+2), (int)(b+1)};\n"
   "  v4i cmp = (va == vb);\n"
   "  return (long)(cmp[0] + cmp[1] + cmp[2] + cmp[3]);\n"
   "}\n",
   {5, 5}, "NEONLane", 1, ""},

  {"abs_v4i32_lane3",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long abs_v4i32_lane3(long a) {\n"
   "  v4i va = {(int)a, (int)(-a), (int)(a-10), (int)(10-a)};\n"
   "  v4i mask = va >> 31;\n"
   "  v4i abs = (va ^ mask) - mask;\n"
   "  return (long)abs[3];\n"
   "}\n",
   {3}, "NEONLane", 1, ""},

  {"min_v4i32",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long min_v4i32(long a, long b) {\n"
   "  v4i va = {(int)a, (int)(a+10), (int)(a+20), (int)(a+30)};\n"
   "  v4i vb = {(int)(b+15), (int)(b+5), (int)(b+25), (int)(b+10)};\n"
   "  v4i mask = va < vb;\n"
   "  v4i mn = (va & mask) | (vb & ~mask);\n"
   "  return (long)(mn[0] + mn[1] + mn[2] + mn[3]);\n"
   "}\n",
   {5, 3}, "NEONLane", 1, ""},

};
// clang-format on

INSTANTIATE_TEST_SUITE_P(NEONLane, AArch64NEONLaneChainRT,
                         ::testing::ValuesIn(kA64NEONLaneChain),
                         [](const auto &P) { return P.param.Name; });
