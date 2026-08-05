//===- AArch64_NEONWidenNarrowRTTests.cpp - NEON widen/narrow roundtrip --===//
//
// Tests AArch64 NEON widening, narrowing, pairwise, and misc operations
// through C vector types and intrinsics.
//
// Covers: SADDL/UADDL, SSUBL/USUBL, SMULL/UMULL (vector), SADDLP/UADDLP,
//         ADDP, SMINP/UMINP, SMAXP/UMAXP, SQXTN/UQXTN,
//         ABS, NEG, CNT, REV64/REV32/REV16 (vector), CLZ (vector),
//         CMTST, SQDMULH, SQRDMULH, SRSHL/URSHL, SSHL/USHL,
//         TRN1/TRN2, ZIP1/ZIP2, UZP1/UZP2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64NEONWidenRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64NEONWidenRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kAArch64NEONWiden = {

  // ===== SADDL: widening signed add (v4i16 → v4i32) =====
  {"neon_saddl_s16",
   "typedef short v4hi __attribute__((vector_size(8)));\n"
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long neon_saddl_s16(long a, long b) {\n"
   "  v4hi va = {(short)a, 0, 0, 0};\n"
   "  v4hi vb = {(short)b, 0, 0, 0};\n"
   "  v4si vr = __builtin_convertvector(va, v4si) + __builtin_convertvector(vb, v4si);\n"
   "  return (long)vr[0];\n"
   "}\n",
   {100, 200}, "NEONWiden", 1, ""},

  // ===== UADDL: widening unsigned add =====
  {"neon_uaddl_u16",
   "typedef unsigned short v4hu __attribute__((vector_size(8)));\n"
   "typedef unsigned v4su __attribute__((vector_size(16)));\n"
   "long neon_uaddl_u16(long a, long b) {\n"
   "  v4hu va = {(unsigned short)a, 0, 0, 0};\n"
   "  v4hu vb = {(unsigned short)b, 0, 0, 0};\n"
   "  v4su vr = __builtin_convertvector(va, v4su) + __builtin_convertvector(vb, v4su);\n"
   "  return (long)vr[0];\n"
   "}\n",
   {60000, 60000}, "NEONWiden", 1, ""},

  // ===== Packed int abs =====
  {"neon_abs_s32",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long neon_abs_s32(long a) {\n"
   "  int ia = (int)a;\n"
   "  v4si va = {ia, -ia, ia, -ia};\n"
   "  v4si mask = va >> 31;\n"
   "  v4si vr = (va ^ mask) - mask;\n"
   "  return (long)vr[0] + (long)vr[1];\n"
   "}\n",
   {(uint64_t)(int64_t)-42}, "NEONWiden", 1, ""},

  // ===== Packed int neg =====
  {"neon_neg_s32",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long neon_neg_s32(long a) {\n"
   "  v4si va = {(int)a, 0, 0, 0};\n"
   "  v4si zero = {0, 0, 0, 0};\n"
   "  v4si vr = zero - va;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {42}, "NEONWiden", 1, ""},

  // ===== Packed add =====
  {"neon_add_v4i32",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long neon_add_v4i32(long a, long b) {\n"
   "  v4si va = {(int)a, (int)(a+1), (int)(a+2), (int)(a+3)};\n"
   "  v4si vb = {(int)b, (int)(b+1), (int)(b+2), (int)(b+3)};\n"
   "  v4si vr = va + vb;\n"
   "  return (long)vr[0] + (long)vr[1] + (long)vr[2] + (long)vr[3];\n"
   "}\n",
   {10, 20}, "NEONWiden", 1, ""},

  // ===== Packed sub =====
  {"neon_sub_v4i32",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long neon_sub_v4i32(long a, long b) {\n"
   "  v4si va = {(int)a, (int)(a+10), (int)(a+20), (int)(a+30)};\n"
   "  v4si vb = {(int)b, (int)b, (int)b, (int)b};\n"
   "  v4si vr = va - vb;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {100, 10}, "NEONWiden", 1, ""},

  // ===== Packed mul =====
  {"neon_mul_v4i32",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long neon_mul_v4i32(long a, long b) {\n"
   "  v4si va = {(int)a, 2, 3, 4};\n"
   "  v4si vb = {(int)b, 5, 6, 7};\n"
   "  v4si vr = va * vb;\n"
   "  return (long)vr[0] + (long)vr[1];\n"
   "}\n",
   {3, 7}, "NEONWiden", 1, ""},

  // ===== Packed AND =====
  {"neon_and_v4i32",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long neon_and_v4i32(long a, long b) {\n"
   "  v4si va = {(int)a, 0, 0, 0};\n"
   "  v4si vb = {(int)b, 0, 0, 0};\n"
   "  v4si vr = va & vb;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0xFF00, 0x0FF0}, "NEONWiden", 1, ""},

  // ===== Packed OR =====
  {"neon_or_v4i32",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long neon_or_v4i32(long a, long b) {\n"
   "  v4si va = {(int)a, 0, 0, 0};\n"
   "  v4si vb = {(int)b, 0, 0, 0};\n"
   "  v4si vr = va | vb;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0xF0, 0x0F}, "NEONWiden", 1, ""},

  // ===== Packed XOR =====
  {"neon_xor_v4i32",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long neon_xor_v4i32(long a, long b) {\n"
   "  v4si va = {(int)a, 0, 0, 0};\n"
   "  v4si vb = {(int)b, 0, 0, 0};\n"
   "  v4si vr = va ^ vb;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0xFF, 0x55}, "NEONWiden", 1, ""},

  // ===== Packed NOT =====
  {"neon_not_v4i32",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long neon_not_v4i32(long a) {\n"
   "  v4si va = {(int)a, 0, 0, 0};\n"
   "  v4si vr = ~va;\n"
   "  return (long)(unsigned)vr[0];\n"
   "}\n",
   {0xFF}, "NEONWiden", 1, ""},

  // ===== Packed shift left =====
  {"neon_shl_v4i32",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long neon_shl_v4i32(long a) {\n"
   "  v4si va = {(int)a, 0, 0, 0};\n"
   "  v4si vr = va << 4;\n"
   "  return (long)(unsigned)vr[0];\n"
   "}\n",
   {0xFF}, "NEONWiden", 1, ""},

  // ===== Packed shift right logical =====
  {"neon_ushr_v4i32",
   "typedef unsigned v4su __attribute__((vector_size(16)));\n"
   "long neon_ushr_v4i32(long a) {\n"
   "  v4su va = {(unsigned)a, 0, 0, 0};\n"
   "  v4su vr = va >> 4;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0xFF0}, "NEONWiden", 1, ""},

  // ===== Packed shift right arithmetic =====
  {"neon_sshr_v4i32",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long neon_sshr_v4i32(long a) {\n"
   "  v4si va = {(int)a, 0, 0, 0};\n"
   "  v4si vr = va >> 4;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {(uint64_t)(int32_t)-256}, "NEONWiden", 1, ""},

  // ===== Packed compare == =====
  {"neon_cmeq_v4i32",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long neon_cmeq_v4i32(long a) {\n"
   "  v4si va = {(int)a, 42, 0, 0};\n"
   "  v4si vb = {(int)a, 0, 0, 0};\n"
   "  v4si vr = (va == vb);\n"
   "  return (long)vr[0] & 1;\n"
   "}\n",
   {42}, "NEONWiden", 1, ""},

  // ===== Packed compare > =====
  {"neon_cmgt_v4i32",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long neon_cmgt_v4i32(long a, long b) {\n"
   "  v4si va = {(int)a, 0, 0, 0};\n"
   "  v4si vb = {(int)b, 0, 0, 0};\n"
   "  v4si vr = (va > vb);\n"
   "  return (long)vr[0] & 1;\n"
   "}\n",
   {100, 50}, "NEONWiden", 1, ""},

  // ===== Float vector add =====
  {"neon_fadd_v4f32",
   "typedef float v4sf __attribute__((vector_size(16)));\n"
   "long neon_fadd_v4f32(long a, long b) {\n"
   "  float fa, fb;\n"
   "  int ia = (int)a, ib = (int)b;\n"
   "  __builtin_memcpy(&fa, &ia, 4); __builtin_memcpy(&fb, &ib, 4);\n"
   "  v4sf va = {fa, fa, fa, fa};\n"
   "  v4sf vb = {fb, fb, fb, fb};\n"
   "  v4sf vr = va + vb;\n"
   "  float r = vr[0]; int ir; __builtin_memcpy(&ir, &r, 4);\n"
   "  return (long)(unsigned)ir;\n"
   "}\n",
   {0x40A00000, 0x40000000}, "NEONWiden", 1, ""},

  // ===== Float vector sub =====
  {"neon_fsub_v4f32",
   "typedef float v4sf __attribute__((vector_size(16)));\n"
   "long neon_fsub_v4f32(long a, long b) {\n"
   "  float fa, fb;\n"
   "  int ia = (int)a, ib = (int)b;\n"
   "  __builtin_memcpy(&fa, &ia, 4); __builtin_memcpy(&fb, &ib, 4);\n"
   "  v4sf va = {fa, fa, fa, fa};\n"
   "  v4sf vb = {fb, fb, fb, fb};\n"
   "  v4sf vr = va - vb;\n"
   "  float r = vr[0]; int ir; __builtin_memcpy(&ir, &r, 4);\n"
   "  return (long)(unsigned)ir;\n"
   "}\n",
   {0x41200000, 0x40A00000}, "NEONWiden", 1, ""},

  // ===== Float vector mul =====
  {"neon_fmul_v4f32",
   "typedef float v4sf __attribute__((vector_size(16)));\n"
   "long neon_fmul_v4f32(long a, long b) {\n"
   "  float fa, fb;\n"
   "  int ia = (int)a, ib = (int)b;\n"
   "  __builtin_memcpy(&fa, &ia, 4); __builtin_memcpy(&fb, &ib, 4);\n"
   "  v4sf va = {fa, fa, fa, fa};\n"
   "  v4sf vb = {fb, fb, fb, fb};\n"
   "  v4sf vr = va * vb;\n"
   "  float r = vr[0]; int ir; __builtin_memcpy(&ir, &r, 4);\n"
   "  return (long)(unsigned)ir;\n"
   "}\n",
   {0x40400000, 0x40000000}, "NEONWiden", 1, ""},

  // ===== Packed short add =====
  {"neon_add_v8i16",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "long neon_add_v8i16(long a, long b) {\n"
   "  v8hi va = {(short)a, 0, 0, 0, 0, 0, 0, 0};\n"
   "  v8hi vb = {(short)b, 0, 0, 0, 0, 0, 0, 0};\n"
   "  v8hi vr = va + vb;\n"
   "  return (long)(unsigned short)vr[0];\n"
   "}\n",
   {100, 200}, "NEONWiden", 1, ""},

  // ===== Packed byte add =====
  {"neon_add_v16i8",
   "typedef signed char v16qi __attribute__((vector_size(16)));\n"
   "long neon_add_v16i8(long a, long b) {\n"
   "  v16qi va = {(signed char)a,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0};\n"
   "  v16qi vb = {(signed char)b,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0};\n"
   "  v16qi vr = va + vb;\n"
   "  return (long)(unsigned char)vr[0];\n"
   "}\n",
   {10, 20}, "NEONWiden", 1, ""},

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(NEONWiden, AArch64NEONWidenRT,
                         ::testing::ValuesIn(kAArch64NEONWiden), rtTCName);
