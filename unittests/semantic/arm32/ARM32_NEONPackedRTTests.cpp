//===- ARM32_NEONPackedRTTests.cpp - ARM32 NEON packed RT ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests ARM32 NEON packed vector operations and VFP through lift pipeline.
// Bug #14 (VFP register mapping) and #18-23 were in the ARM32 area.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32NEONPackRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32NEONPackRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kARM32NEONPack = {
  // ========== VFP float add ==========
  {"arm_fadd",
   "int arm_fadd(int a, int b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  float r = fa + fb;\n"
   "  int ret; __builtin_memcpy(&ret, &r, 4); return ret;\n"
   "}\n",
   {0x40A00000, 0x40400000}, "ARM32NEON"},  // 5.0f + 3.0f

  // ========== VFP float sub ==========
  {"arm_fsub",
   "int arm_fsub(int a, int b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  float r = fa - fb;\n"
   "  int ret; __builtin_memcpy(&ret, &r, 4); return ret;\n"
   "}\n",
   {0x40A00000, 0x40400000}, "ARM32NEON"},

  // ========== VFP float mul ==========
  {"arm_fmul",
   "int arm_fmul(int a, int b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  float r = fa * fb;\n"
   "  int ret; __builtin_memcpy(&ret, &r, 4); return ret;\n"
   "}\n",
   {0x40A00000, 0x40400000}, "ARM32NEON"},

  // ========== VFP float div ==========
  {"arm_fdiv",
   "int arm_fdiv(int a, int b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  float r = fa / fb;\n"
   "  int ret; __builtin_memcpy(&ret, &r, 4); return ret;\n"
   "}\n",
   {0x41200000, 0x40000000}, "ARM32NEON"},  // 10.0f / 2.0f

  // ========== VFP int<->float conversion ==========
  {"arm_i32_to_f32",
   "int arm_i32_to_f32(int a) {\n"
   "  float f = (float)a;\n"
   "  int ret; __builtin_memcpy(&ret, &f, 4); return ret;\n"
   "}\n",
   {42}, "ARM32NEON"},

  {"arm_f32_to_i32",
   "int arm_f32_to_i32(int a) {\n"
   "  float f; __builtin_memcpy(&f, &a, 4);\n"
   "  return (int)f;\n"
   "}\n",
   {0x42280000}, "ARM32NEON"},  // 42.0f

  {"arm_i32_to_f32_neg",
   "int arm_i32_to_f32_neg(int a) {\n"
   "  float f = (float)a;\n"
   "  int ret; __builtin_memcpy(&ret, &f, 4); return ret;\n"
   "}\n",
   {(uint64_t)(int32_t)-42}, "ARM32NEON"},

  // ========== VFP fneg/fabs ==========
  {"arm_fneg",
   "int arm_fneg(int a) {\n"
   "  float f; __builtin_memcpy(&f, &a, 4);\n"
   "  f = -f;\n"
   "  int ret; __builtin_memcpy(&ret, &f, 4); return ret;\n"
   "}\n",
   {0x42280000}, "ARM32NEON"},

  // ========== VFP float compare ==========
  {"arm_fcmp_lt",
   "int arm_fcmp_lt(int a, int b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  return fa < fb ? 1 : 0;\n"
   "}\n",
   {0x40400000, 0x40A00000}, "ARM32NEON"},  // 3.0f < 5.0f

  {"arm_fcmp_eq",
   "int arm_fcmp_eq(int a, int b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  return fa == fb ? 1 : 0;\n"
   "}\n",
   {0x42280000, 0x42280000}, "ARM32NEON"},

  {"arm_fcmp_gt",
   "int arm_fcmp_gt(int a, int b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  return fa > fb ? 1 : 0;\n"
   "}\n",
   {0x40A00000, 0x40400000}, "ARM32NEON"},

  // ========== FP multiply-accumulate (VMLA) ==========
  {"arm_fmadd",
   "int arm_fmadd(int a, int b, int c) {\n"
   "  float fa, fb, fc;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  __builtin_memcpy(&fc, &c, 4);\n"
   "  float r = fa * fb + fc;\n"
   "  int ret; __builtin_memcpy(&ret, &r, 4); return ret;\n"
   "}\n",
   {0x40A00000, 0x40400000, 0x41200000}, "ARM32NEON"},  // 5*3+10

  // ========== Integer packed add (VADD.I32) ==========
  {"arm_neon_add_i32",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "int arm_neon_add_i32(int a, int b) {\n"
   "  v4i va = {a, 0, 0, 0};\n"
   "  v4i vb = {b, 0, 0, 0};\n"
   "  v4i vr = va + vb;\n"
   "  return vr[0];\n"
   "}\n",
   {42, 100}, "ARM32NEON", 1},

  // ========== Integer packed sub (VSUB.I32) ==========
  {"arm_neon_sub_i32",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "int arm_neon_sub_i32(int a, int b) {\n"
   "  v4i va = {a, 0, 0, 0};\n"
   "  v4i vb = {b, 0, 0, 0};\n"
   "  v4i vr = va - vb;\n"
   "  return vr[0];\n"
   "}\n",
   {100, 42}, "ARM32NEON", 1},

  // ========== Bitwise vector ops ==========
  {"arm_neon_and",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "int arm_neon_and(int a, int b) {\n"
   "  v4i va = {a, 0, 0, 0};\n"
   "  v4i vb = {b, 0, 0, 0};\n"
   "  v4i vr = va & vb;\n"
   "  return vr[0];\n"
   "}\n",
   {0xFF00FF00, 0x0F0F0F0F}, "ARM32NEON", 1},

  {"arm_neon_orr",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "int arm_neon_orr(int a, int b) {\n"
   "  v4i va = {a, 0, 0, 0};\n"
   "  v4i vb = {b, 0, 0, 0};\n"
   "  v4i vr = va | vb;\n"
   "  return vr[0];\n"
   "}\n",
   {0xFF000000, 0x000000FF}, "ARM32NEON", 1},

  // ========== Shift (VSHL/VSHR) ==========
  {"arm_neon_shl",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "int arm_neon_shl(int a) {\n"
   "  v4i va = {a, 0, 0, 0};\n"
   "  v4i vr = va << 8;\n"
   "  return vr[0];\n"
   "}\n",
   {0x12345678}, "ARM32NEON", 1},

  {"arm_neon_shr",
   "typedef unsigned v4ui __attribute__((vector_size(16)));\n"
   "int arm_neon_shr(int a) {\n"
   "  v4ui va = {(unsigned)a, 0, 0, 0};\n"
   "  v4ui vr = va >> 8;\n"
   "  return (int)vr[0];\n"
   "}\n",
   {0x12345678}, "ARM32NEON", 1},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(ARM32NEON, ARM32NEONPackRT,
                         ::testing::ValuesIn(kARM32NEONPack), rtTCName);
