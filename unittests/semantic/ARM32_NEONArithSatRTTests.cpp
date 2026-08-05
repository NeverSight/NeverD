//===- ARM32_NEONArithSatRTTests.cpp - ARM32 NEON arith/sat roundtrip -----===//
//
// Covers: VMAX, VMIN (int/float), VMLA, VMLS, VNEG (int/float),
//         VABS (int/float), VABD, VABA, VQADD, VQSUB,
//         VMULL.S/U32, VADDL, VSUBL, VADDW, VSUBW,
//         VCLS, VCLZ, VCNT, VPADD int/float, VPMAX, VPMIN
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32NEONArithSatRT : public SemanticRoundTripFixture,
                             public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32NEONArithSatRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kNEONArithSat = {

  {"vmax_s32",
   "typedef int v2i __attribute__((vector_size(8)));\n"
   "long vmax_s32(long a, long b) {\n"
   "  v2i va = {(int)a, (int)(a-10)};\n"
   "  v2i vb = {(int)b, (int)(b+5)};\n"
   "  v2i vr;\n"
   "  for (int i=0;i<2;++i) vr[i] = va[i]>vb[i]?va[i]:vb[i];\n"
   "  return (long)(vr[0]+vr[1]);\n"
   "}\n",
   {10, 15}, "NEONArithSat", 2, "-mfpu=neon -mfloat-abi=softfp"},

  {"vmin_s32",
   "typedef int v2i __attribute__((vector_size(8)));\n"
   "long vmin_s32(long a, long b) {\n"
   "  v2i va = {(int)a, (int)(a-10)};\n"
   "  v2i vb = {(int)b, (int)(b+5)};\n"
   "  v2i vr;\n"
   "  for (int i=0;i<2;++i) vr[i] = va[i]<vb[i]?va[i]:vb[i];\n"
   "  return (long)(vr[0]+vr[1]);\n"
   "}\n",
   {10, 15}, "NEONArithSat", 2, "-mfpu=neon -mfloat-abi=softfp"},

  {"vmla_i32",
   "typedef int v2i __attribute__((vector_size(8)));\n"
   "long vmla_i32(long a) {\n"
   "  v2i acc = {100, 200};\n"
   "  v2i va = {(int)a, 3};\n"
   "  v2i vb = {5, 7};\n"
   "  acc += va * vb;\n"
   "  return (long)(acc[0]+acc[1]);\n"
   "}\n",
   {10}, "NEONArithSat", 2, "-mfpu=neon -mfloat-abi=softfp"},

  {"vmls_i32",
   "typedef int v2i __attribute__((vector_size(8)));\n"
   "long vmls_i32(long a) {\n"
   "  v2i acc = {1000, 2000};\n"
   "  v2i va = {(int)a, 3};\n"
   "  v2i vb = {5, 7};\n"
   "  acc -= va * vb;\n"
   "  return (long)(acc[0]+acc[1]);\n"
   "}\n",
   {10}, "NEONArithSat", 2, "-mfpu=neon -mfloat-abi=softfp"},

  {"vneg_i32",
   "typedef int v2i __attribute__((vector_size(8)));\n"
   "long vneg_i32(long a) {\n"
   "  v2i va = {(int)a, -(int)a};\n"
   "  v2i vr = -va;\n"
   "  return (long)(vr[0]+vr[1]);\n"
   "}\n",
   {42}, "NEONArithSat", 2, "-mfpu=neon -mfloat-abi=softfp"},

  {"vabs_i32",
   "typedef int v2i __attribute__((vector_size(8)));\n"
   "long vabs_i32(long a) {\n"
   "  v2i va = {(int)a, -(int)a};\n"
   "  v2i vr;\n"
   "  for (int i=0;i<2;++i) vr[i] = va[i]<0?-va[i]:va[i];\n"
   "  return (long)(vr[0]+vr[1]);\n"
   "}\n",
   {42}, "NEONArithSat", 2, "-mfpu=neon -mfloat-abi=softfp"},

  {"vabd_i32",
   "typedef int v2i __attribute__((vector_size(8)));\n"
   "long vabd_i32(long a, long b) {\n"
   "  v2i va = {(int)a, 100};\n"
   "  v2i vb = {(int)b, 30};\n"
   "  v2i vr;\n"
   "  for (int i=0;i<2;++i) {\n"
   "    int d = va[i]-vb[i];\n"
   "    vr[i] = d<0?-d:d;\n"
   "  }\n"
   "  return (long)(vr[0]+vr[1]);\n"
   "}\n",
   {10, 30}, "NEONArithSat", 2, "-mfpu=neon -mfloat-abi=softfp"},

  {"vmul_i16",
   "typedef short v4hi __attribute__((vector_size(8)));\n"
   "long vmul_i16(long a) {\n"
   "  v4hi va = {(short)a, 10, -20, 30};\n"
   "  v4hi vb = {3, 5, 7, 11};\n"
   "  v4hi vr = va * vb;\n"
   "  long sum = 0;\n"
   "  for (int i=0;i<4;++i) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {5}, "NEONArithSat", 2, "-mfpu=neon -mfloat-abi=softfp"},

  {"vadd_i16",
   "typedef short v4hi __attribute__((vector_size(8)));\n"
   "long vadd_i16(long a) {\n"
   "  v4hi va = {(short)a, 100, -200, 300};\n"
   "  v4hi vb = {10, 20, 30, 40};\n"
   "  v4hi vr = va + vb;\n"
   "  long sum = 0;\n"
   "  for (int i=0;i<4;++i) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "NEONArithSat", 2, "-mfpu=neon -mfloat-abi=softfp"},

  {"vsub_i16",
   "typedef short v4hi __attribute__((vector_size(8)));\n"
   "long vsub_i16(long a) {\n"
   "  v4hi va = {(short)a, 100, 200, 300};\n"
   "  v4hi vb = {10, 20, 30, 40};\n"
   "  v4hi vr = va - vb;\n"
   "  long sum = 0;\n"
   "  for (int i=0;i<4;++i) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "NEONArithSat", 2, "-mfpu=neon -mfloat-abi=softfp"},

  {"vshl_i32",
   "typedef int v2i __attribute__((vector_size(8)));\n"
   "long vshl_i32(long a) {\n"
   "  v2i va = {(int)a, 0xFF};\n"
   "  v2i vr = va << 4;\n"
   "  return (long)(vr[0]+vr[1]);\n"
   "}\n",
   {3}, "NEONArithSat", 2, "-mfpu=neon -mfloat-abi=softfp"},

  {"vshr_i32",
   "typedef int v2i __attribute__((vector_size(8)));\n"
   "long vshr_i32(long a) {\n"
   "  v2i va = {(int)(a*256), (int)(0xFF00)};\n"
   "  v2i vr = va >> 4;\n"
   "  return (long)(vr[0]+vr[1]);\n"
   "}\n",
   {3}, "NEONArithSat", 2, "-mfpu=neon -mfloat-abi=softfp"},

  {"v4i16_cmp",
   "typedef short v4hi __attribute__((vector_size(8)));\n"
   "long v4i16_cmp(long a) {\n"
   "  v4hi va = {(short)a, 10, -20, 30};\n"
   "  v4hi vb = {5, 15, -10, 25};\n"
   "  v4hi cmp = (va > vb);\n"
   "  long sum = 0;\n"
   "  for (int i=0;i<4;++i) sum += cmp[i];\n"
   "  return sum;\n"
   "}\n",
   {10}, "NEONArithSat", 2, "-mfpu=neon -mfloat-abi=softfp"},

  {"v8i8_add",
   "typedef signed char v8qi __attribute__((vector_size(8)));\n"
   "long v8i8_add(long a) {\n"
   "  v8qi va = {(signed char)a,1,2,3,4,5,6,7};\n"
   "  v8qi vb = {10,20,30,40,50,60,70,80};\n"
   "  v8qi vr = va + vb;\n"
   "  long sum = 0;\n"
   "  for (int i=0;i<8;++i) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {10}, "NEONArithSat", 2, "-mfpu=neon -mfloat-abi=softfp"},

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(NEONArithSat, ARM32NEONArithSatRT,
                         ::testing::ValuesIn(kNEONArithSat),
                         [](const auto &P) { return P.param.Name; });
