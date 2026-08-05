//===- ARM32_NEONRoundTripTests.cpp - ARM NEON roundtrip tests -*- C++ -*-===//
//
// Tests ARM32 NEON/VFP instructions through the full lift pipeline.
// Uses max 2 args to avoid ABI non-consecutive-register detection bug.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32NEONRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32NEONRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kARM32NEON = {
  // ========== VADD/VSUB/VMUL 32-bit int ==========
  {"arm_vadd4i",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "int arm_vadd4i(int a, int b) {\n"
   "  v4i va = {a, 0, 0, 0}; v4i vb = {b, 0, 0, 0};\n"
   "  v4i vr = va + vb;\n"
   "  return vr[0];\n"
   "}\n",
   {100, 42}, "ARM32NEONRT", 1},

  {"arm_vsub4i",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "int arm_vsub4i(int a, int b) {\n"
   "  v4i va = {a, 0, 0, 0}; v4i vb = {b, 0, 0, 0};\n"
   "  v4i vr = va - vb;\n"
   "  return vr[0];\n"
   "}\n",
   {100, 42}, "ARM32NEONRT", 1},

  {"arm_vmul4i",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "int arm_vmul4i(int a, int b) {\n"
   "  v4i va = {a, 0, 0, 0}; v4i vb = {b, 0, 0, 0};\n"
   "  v4i vr = va * vb;\n"
   "  return vr[0];\n"
   "}\n",
   {7, 6}, "ARM32NEONRT", 1},

  // ========== 16-bit ==========
  {"arm_vadd8h",
   "typedef short v8h __attribute__((vector_size(16)));\n"
   "int arm_vadd8h(int a, int b) {\n"
   "  v8h va = {(short)a, 0,0,0,0,0,0,0};\n"
   "  v8h vb = {(short)b, 0,0,0,0,0,0,0};\n"
   "  v8h vr = va + vb;\n"
   "  return (int)(unsigned short)vr[0];\n"
   "}\n",
   {100, 42}, "ARM32NEONRT", 1},

  // ========== 8-bit ==========
  {"arm_vadd16b",
   "typedef char v16b __attribute__((vector_size(16)));\n"
   "int arm_vadd16b(int a, int b) {\n"
   "  v16b va = {(char)a, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0};\n"
   "  v16b vb = {(char)b, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0};\n"
   "  v16b vr = va + vb;\n"
   "  return (int)(unsigned char)vr[0];\n"
   "}\n",
   {100, 42}, "ARM32NEONRT", 1},

  // ========== Bitwise (VAND/VORR/VEOR) ==========
  {"arm_vand",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "int arm_vand(int a, int b) {\n"
   "  v4i va = {a, 0, 0, 0}; v4i vb = {b, 0, 0, 0};\n"
   "  v4i vr = va & vb;\n"
   "  return vr[0];\n"
   "}\n",
   {0xFF00FF00ULL, 0x0FF00FF0ULL}, "ARM32NEONRT", 1},

  {"arm_vorr",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "int arm_vorr(int a, int b) {\n"
   "  v4i va = {a, 0, 0, 0}; v4i vb = {b, 0, 0, 0};\n"
   "  v4i vr = va | vb;\n"
   "  return vr[0];\n"
   "}\n",
   {0xF0F0F0F0ULL, 0x0F0F0F0FULL}, "ARM32NEONRT", 1},

  {"arm_veor",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "int arm_veor(int a, int b) {\n"
   "  v4i va = {a, 0, 0, 0}; v4i vb = {b, 0, 0, 0};\n"
   "  v4i vr = va ^ vb;\n"
   "  return vr[0];\n"
   "}\n",
   {0xFF00FF00ULL, 0x0FF00FF0ULL}, "ARM32NEONRT", 1},

  // ========== Shift (VSHL/VSHR) ==========
  {"arm_vshl4i",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "int arm_vshl4i(int a) {\n"
   "  v4i va = {a, 0, 0, 0};\n"
   "  v4i vr = va << 8;\n"
   "  return vr[0];\n"
   "}\n",
   {0xDEADBEEFULL}, "ARM32NEONRT", 1},

  {"arm_vshr4i",
   "typedef unsigned int v4ui __attribute__((vector_size(16)));\n"
   "int arm_vshr4i(int a) {\n"
   "  v4ui va = {(unsigned int)a, 0, 0, 0};\n"
   "  v4ui vr = va >> 8;\n"
   "  return (int)vr[0];\n"
   "}\n",
   {0xDEADBEEFULL}, "ARM32NEONRT", 1},

  // ========== Compare (VCEQ/VCGT) ==========
  {"arm_vceq4i",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "int arm_vceq4i(int a, int b) {\n"
   "  v4i va = {a, 0, 0, 0}; v4i vb = {b, 0, 0, 0};\n"
   "  v4i vr = (va == vb);\n"
   "  return vr[0];\n"
   "}\n",
   {42, 42}, "ARM32NEONRT", 1},

  {"arm_vcgt4i",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "int arm_vcgt4i(int a, int b) {\n"
   "  v4i va = {a, 0, 0, 0}; v4i vb = {b, 0, 0, 0};\n"
   "  v4i vr = (va > vb);\n"
   "  return vr[0];\n"
   "}\n",
   {100, 42}, "ARM32NEONRT", 1},

  // ========== Negate ==========
  {"arm_vneg4i",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "int arm_vneg4i(int a) {\n"
   "  v4i va = {a, 0, 0, 0};\n"
   "  v4i vr = -va;\n"
   "  return vr[0];\n"
   "}\n",
   {42}, "ARM32NEONRT", 1},

  // ========== VFP float (VADD.F32/VMUL.F32) ==========
  {"arm_vfp_adds",
   "int arm_vfp_adds(int a, int b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  float result = fa + fb;\n"
   "  int r; __builtin_memcpy(&r, &result, 4); return r;\n"
   "}\n",
   {0x40A00000ULL, 0x40400000ULL}, "ARM32NEONRT", 1},

  {"arm_vfp_muls",
   "int arm_vfp_muls(int a, int b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  float result = fa * fb;\n"
   "  int r; __builtin_memcpy(&r, &result, 4); return r;\n"
   "}\n",
   {0x40A00000ULL, 0x40000000ULL}, "ARM32NEONRT", 1},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(ARM32NEONRT, ARM32NEONRT,
                         ::testing::ValuesIn(kARM32NEON), rtTCName);
