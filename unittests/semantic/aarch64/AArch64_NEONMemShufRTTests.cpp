//===- AArch64_NEONMemShufRTTests.cpp - NEON mem/shuffle roundtrip --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Covers: LDR/STR (vector), LDP/STP, DUP, SMOV, UMOV, ZIP1/2, UZP1/2,
//         TRN1/2, EXT, REV64/32/16, FABS/FNEG (vector), NOT, ORN, BIC
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64NEONMemShufRT : public SemanticRoundTripFixture,
                              public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64NEONMemShufRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kNEONMemShuf = {

  {"dup_scalar_v4i32",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long dup_scalar_v4i32(long a) {\n"
   "  int x = (int)a;\n"
   "  v4si v = {x, x, x, x};\n"
   "  return (long)(v[0]+v[1]+v[2]+v[3]);\n"
   "}\n",
   {42}, "NEONMemShuf", 2, ""},

  {"not_v16i8",
   "typedef unsigned char v16qu __attribute__((vector_size(16)));\n"
   "long not_v16i8(long a) {\n"
   "  v16qu v = {(unsigned char)a,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16qu r = ~v;\n"
   "  return (long)r[0];\n"
   "}\n",
   {0x55}, "NEONMemShuf", 2, ""},

  {"bic_v4i32",
   "typedef unsigned int v4u __attribute__((vector_size(16)));\n"
   "long bic_v4i32(long a, long b) {\n"
   "  v4u va = {(unsigned)a, (unsigned)a, (unsigned)a, (unsigned)a};\n"
   "  v4u vb = {(unsigned)b, (unsigned)b, (unsigned)b, (unsigned)b};\n"
   "  v4u r = va & ~vb;\n"
   "  return (long)r[0];\n"
   "}\n",
   {0xFF, 0x0F}, "NEONMemShuf", 2, ""},

  {"orn_v4i32",
   "typedef unsigned int v4u __attribute__((vector_size(16)));\n"
   "long orn_v4i32(long a, long b) {\n"
   "  v4u va = {(unsigned)a, 0, 0, 0};\n"
   "  v4u vb = {(unsigned)b, 0, 0, 0};\n"
   "  v4u r = va | ~vb;\n"
   "  return (long)(int)r[0];\n"
   "}\n",
   {0xFF, 0x0F}, "NEONMemShuf", 2, ""},

  {"vec_shuffle_v4i32",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long vec_shuffle_v4i32(long a) {\n"
   "  v4si v = {(int)a, (int)(a+10), (int)(a+20), (int)(a+30)};\n"
   "  v4si r = __builtin_shufflevector(v, v, 3, 2, 1, 0);\n"
   "  return (long)(r[0]+r[1]+r[2]+r[3]);\n"
   "}\n",
   {10}, "NEONMemShuf", 2, ""},

  {"vec_reverse_i16",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "long vec_reverse_i16(long a) {\n"
   "  v8hi v = {(short)a,2,3,4,5,6,7,8};\n"
   "  v8hi r = __builtin_shufflevector(v, v, 7,6,5,4,3,2,1,0);\n"
   "  return (long)(r[0]+r[7]);\n"
   "}\n",
   {42}, "NEONMemShuf", 2, ""},

  {"vec_interleave",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long vec_interleave(long a, long b) {\n"
   "  v4si va = {(int)a, (int)(a+1), 0, 0};\n"
   "  v4si vb = {(int)b, (int)(b+1), 0, 0};\n"
   "  v4si r = __builtin_shufflevector(va, vb, 0, 4, 1, 5);\n"
   "  return (long)(r[0]+r[1]+r[2]+r[3]);\n"
   "}\n",
   {10, 100}, "NEONMemShuf", 2, ""},

  {"vec_deinterleave",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long vec_deinterleave(long a) {\n"
   "  v4si v = {(int)a, 100, (int)(a+1), 200};\n"
   "  v4si r = __builtin_shufflevector(v, v, 0, 2, 1, 3);\n"
   "  return (long)(r[0]+r[1]);\n"
   "}\n",
   {10}, "NEONMemShuf", 2, ""},

  {"vec_broadcast_byte",
   "typedef unsigned char v16qu __attribute__((vector_size(16)));\n"
   "long vec_broadcast_byte(long a) {\n"
   "  unsigned char b = (unsigned char)a;\n"
   "  v16qu v = {b,b,b,b,b,b,b,b,b,b,b,b,b,b,b,b};\n"
   "  long sum = 0;\n"
   "  for (int i=0;i<16;++i) sum += v[i];\n"
   "  return sum;\n"
   "}\n",
   {7}, "NEONMemShuf", 2, ""},

  {"vec_extract_lane3",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long vec_extract_lane3(long a) {\n"
   "  v4si v = {(int)a, (int)(a*2), (int)(a*3), (int)(a*4)};\n"
   "  return (long)v[3];\n"
   "}\n",
   {25}, "NEONMemShuf", 2, ""},

  {"vec_insert_lane2",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long vec_insert_lane2(long a, long b) {\n"
   "  v4si v = {1, 2, 3, 4};\n"
   "  v[2] = (int)b;\n"
   "  return (long)(v[0]+v[1]+v[2]+v[3]);\n"
   "}\n",
   {0, 99}, "NEONMemShuf", 2, ""},

  {"volatile_load_store",
   "long volatile_load_store(long a) {\n"
   "  volatile long tmp = a * 3;\n"
   "  return tmp + 1;\n"
   "}\n",
   {42}, "NEONMemShuf", 2, ""},

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(NEONMemShuf, AArch64NEONMemShufRT,
                         ::testing::ValuesIn(kNEONMemShuf),
                         [](const auto &P) { return P.param.Name; });
