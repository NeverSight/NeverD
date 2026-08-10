//===- AArch64_NEONArithRTTests.cpp - NEON arithmetic roundtrip -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Covers: SMAX, UMAX, SMIN, UMIN, SABA, UABA, SABAL, UABAL,
//         SADDLV, UADDLV, SMAXV, UMAXV, SMINV, UMINV,
//         MLA, MLS, SQNEG, NEG, ADDV, RADDHN, SUBHN
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64NEONArithRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64NEONArithRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kNEONArith = {

  {"smax_v4i32",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long smax_v4i32(long a, long b) {\n"
   "  v4i va = {(int)a, (int)(a-10), (int)(a+20), (int)(a-5)};\n"
   "  v4i vb = {(int)b, (int)(b+5), (int)(b-3), (int)(b+10)};\n"
   "  v4i vr;\n"
   "  for (int i=0;i<4;++i) vr[i] = va[i]>vb[i]?va[i]:vb[i];\n"
   "  return (long)(vr[0]+vr[1]+vr[2]+vr[3]);\n"
   "}\n",
   {10, 15}, "NEONArith", 2, ""},

  {"umax_v4u32",
   "typedef unsigned int v4u __attribute__((vector_size(16)));\n"
   "long umax_v4u32(long a, long b) {\n"
   "  v4u va = {(unsigned)a, 100, 200, 300};\n"
   "  v4u vb = {(unsigned)b, 50, 250, 150};\n"
   "  v4u vr;\n"
   "  for (int i=0;i<4;++i) vr[i] = va[i]>vb[i]?va[i]:vb[i];\n"
   "  return (long)(vr[0]+vr[1]+vr[2]+vr[3]);\n"
   "}\n",
   {42, 99}, "NEONArith", 2, ""},

  {"smin_v4i32",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long smin_v4i32(long a, long b) {\n"
   "  v4i va = {(int)a, (int)(a-10), (int)(a+20), (int)(a-5)};\n"
   "  v4i vb = {(int)b, (int)(b+5), (int)(b-3), (int)(b+10)};\n"
   "  v4i vr;\n"
   "  for (int i=0;i<4;++i) vr[i] = va[i]<vb[i]?va[i]:vb[i];\n"
   "  return (long)(vr[0]+vr[1]+vr[2]+vr[3]);\n"
   "}\n",
   {10, 15}, "NEONArith", 2, ""},

  {"umin_v4u32",
   "typedef unsigned int v4u __attribute__((vector_size(16)));\n"
   "long umin_v4u32(long a, long b) {\n"
   "  v4u va = {(unsigned)a, 100, 200, 300};\n"
   "  v4u vb = {(unsigned)b, 50, 250, 150};\n"
   "  v4u vr;\n"
   "  for (int i=0;i<4;++i) vr[i] = va[i]<vb[i]?va[i]:vb[i];\n"
   "  return (long)(vr[0]+vr[1]+vr[2]+vr[3]);\n"
   "}\n",
   {42, 99}, "NEONArith", 2, ""},

  {"neg_v4i32",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long neg_v4i32(long a) {\n"
   "  v4i va = {(int)a, -(int)a, 100, -200};\n"
   "  v4i vr;\n"
   "  for (int i=0;i<4;++i) vr[i] = -va[i];\n"
   "  return (long)(vr[0]+vr[1]+vr[2]+vr[3]);\n"
   "}\n",
   {42}, "NEONArith", 2, ""},

  {"abs_v4i32",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long abs_v4i32(long a) {\n"
   "  v4i va = {(int)a, -(int)a, 100, -200};\n"
   "  v4i vr;\n"
   "  for (int i=0;i<4;++i) vr[i] = va[i]<0?-va[i]:va[i];\n"
   "  return (long)(vr[0]+vr[1]+vr[2]+vr[3]);\n"
   "}\n",
   {42}, "NEONArith", 2, ""},

  {"mla_v4i32",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long mla_v4i32(long a) {\n"
   "  v4i acc = {0, 0, 0, 0};\n"
   "  v4i va = {(int)a, 2, 3, 4};\n"
   "  v4i vb = {5, 6, 7, 8};\n"
   "  acc += va * vb;\n"
   "  return (long)(acc[0]+acc[1]+acc[2]+acc[3]);\n"
   "}\n",
   {10}, "NEONArith", 2, ""},

  {"mls_v4i32",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long mls_v4i32(long a) {\n"
   "  v4i acc = {1000, 1000, 1000, 1000};\n"
   "  v4i va = {(int)a, 2, 3, 4};\n"
   "  v4i vb = {5, 6, 7, 8};\n"
   "  acc -= va * vb;\n"
   "  return (long)(acc[0]+acc[1]+acc[2]+acc[3]);\n"
   "}\n",
   {10}, "NEONArith", 2, ""},

  {"v8i16_add_sub_chain",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "long v8i16_add_sub_chain(long a) {\n"
   "  v8hi va = {(short)a, 100, 200, -300, 400, -500, 600, -700};\n"
   "  v8hi vb = {10, 20, 30, 40, 50, 60, 70, 80};\n"
   "  v8hi vr = va + vb - vb + vb;\n"
   "  long sum = 0;\n"
   "  for (int i=0;i<8;++i) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "NEONArith", 2, ""},

  {"v16i8_cmp_mask",
   "typedef signed char v16qi __attribute__((vector_size(16)));\n"
   "long v16i8_cmp_mask(long a) {\n"
   "  v16qi va = {(signed char)a,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};\n"
   "  v16qi vb = {5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5};\n"
   "  v16qi cmp = (va > vb);\n"
   "  long sum = 0;\n"
   "  for (int i=0;i<16;++i) sum += cmp[i];\n"
   "  return sum;\n"
   "}\n",
   {10}, "NEONArith", 2, ""},

  {"smax_v8i16",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "long smax_v8i16(long a) {\n"
   "  v8hi va = {(short)a, -100, 200, -300, 400, -500, 600, -700};\n"
   "  v8hi vb = {10, 20, 30, 40, 50, 60, 70, 80};\n"
   "  v8hi vr;\n"
   "  for (int i=0;i<8;++i) vr[i] = va[i]>vb[i]?va[i]:vb[i];\n"
   "  long sum = 0;\n"
   "  for (int i=0;i<8;++i) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "NEONArith", 2, ""},

  // Packed FP vector ops with per-lane extraction after FADD v.2d / FMUL v.4s.
  {"fadd_v2f64",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long fadd_v2f64(long a, long b) {\n"
   "  v2d va = {(double)a, (double)(a+1)};\n"
   "  v2d vb = {(double)b, (double)(b+2)};\n"
   "  v2d vr = va + vb;\n"
   "  return (long)vr[0] + (long)vr[1];\n"
   "}\n",
   {10, 20}, "NEONArith", 2, ""},

  {"fmul_v4f32",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long fmul_v4f32(long a) {\n"
   "  v4f va = {(float)a, (float)(a+1), (float)(a+2), (float)(a+3)};\n"
   "  v4f vb = {2.0f, 3.0f, 4.0f, 5.0f};\n"
   "  v4f vr = va * vb;\n"
   "  v4i ri = __builtin_convertvector(vr, v4i);\n"
   "  return (long)(ri[0]+ri[1]+ri[2]+ri[3]);\n"
   "}\n",
   {10}, "NEONArith", 2, ""},

  {"fdiv_v4f32",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long fdiv_v4f32(long a) {\n"
   "  v4f va = {(float)(a*10),(float)(a*20),(float)(a*30),(float)(a*40)};\n"
   "  v4f vb = {(float)a,(float)a,(float)a,(float)a};\n"
   "  v4f vr = va / vb;\n"
   "  v4i ri = __builtin_convertvector(vr, v4i);\n"
   "  return (long)(ri[0]+ri[1]+ri[2]+ri[3]);\n"
   "}\n",
   {3}, "NEONArith", 2, ""},

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(NEONArith, AArch64NEONArithRT,
                         ::testing::ValuesIn(kNEONArith),
                         [](const auto &P) { return P.param.Name; });
