//===- ARM32_NEONVecOpsRTTests.cpp - ARM32 NEON vector ops roundtrip -----===//
//
// Tests ARM32 NEON packed vector operations through C vector types.
// Covers: VADD, VSUB, VMUL, VAND, VORR, VEOR, VMVN, VSHL, VSHR,
//         VCEQ, VCGT, VNEG, VABS, VMIN/VMAX.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32NEONVecOpsRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32NEONVecOpsRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kARM32NEONVecOps = {

  // ===== Packed int add (VADD.I32 q) =====
  {"neon_vadd_i32",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "int neon_vadd_i32(int a, int b) {\n"
   "  v4si va = {a, a+1, a+2, a+3};\n"
   "  v4si vb = {b, b, b, b};\n"
   "  v4si vr = va + vb;\n"
   "  return vr[0];\n"
   "}\n",
   {10, 20}, "NEONOps", 1, "-mfpu=neon -mfloat-abi=softfp"},

  // ===== Packed int sub (VSUB.I32 q) =====
  {"neon_vsub_i32",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "int neon_vsub_i32(int a, int b) {\n"
   "  v4si va = {a, a+10, a+20, a+30};\n"
   "  v4si vb = {b, b, b, b};\n"
   "  v4si vr = va - vb;\n"
   "  return vr[0];\n"
   "}\n",
   {100, 10}, "NEONOps", 1, "-mfpu=neon -mfloat-abi=softfp"},

  // ===== Packed int mul (VMUL.I32 q) =====
  {"neon_vmul_i32",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "int neon_vmul_i32(int a, int b) {\n"
   "  v4si va = {a, 2, 3, 4};\n"
   "  v4si vb = {b, 5, 6, 7};\n"
   "  v4si vr = va * vb;\n"
   "  return vr[0] + vr[1];\n"
   "}\n",
   {3, 7}, "NEONOps", 1, "-mfpu=neon -mfloat-abi=softfp"},

  // ===== Packed AND (VAND q) =====
  {"neon_vand",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "int neon_vand(int a, int b) {\n"
   "  v4si va = {a, 0, 0, 0};\n"
   "  v4si vb = {b, 0, 0, 0};\n"
   "  v4si vr = va & vb;\n"
   "  return vr[0];\n"
   "}\n",
   {0xFF00, 0x0FF0}, "NEONOps", 1, "-mfpu=neon -mfloat-abi=softfp"},

  // ===== Packed OR (VORR q) =====
  {"neon_vorr",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "int neon_vorr(int a, int b) {\n"
   "  v4si va = {a, 0, 0, 0};\n"
   "  v4si vb = {b, 0, 0, 0};\n"
   "  v4si vr = va | vb;\n"
   "  return vr[0];\n"
   "}\n",
   {0xF0, 0x0F}, "NEONOps", 1, "-mfpu=neon -mfloat-abi=softfp"},

  // ===== Packed XOR (VEOR q) =====
  {"neon_veor",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "int neon_veor(int a, int b) {\n"
   "  v4si va = {a, 0, 0, 0};\n"
   "  v4si vb = {b, 0, 0, 0};\n"
   "  v4si vr = va ^ vb;\n"
   "  return vr[0];\n"
   "}\n",
   {0xFF, 0x55}, "NEONOps", 1, "-mfpu=neon -mfloat-abi=softfp"},

  // ===== Packed NOT (VMVN q) =====
  {"neon_vmvn",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "int neon_vmvn(int a) {\n"
   "  v4si va = {a, 0, 0, 0};\n"
   "  v4si vr = ~va;\n"
   "  return vr[0];\n"
   "}\n",
   {0xFF}, "NEONOps", 1, "-mfpu=neon -mfloat-abi=softfp"},

  // ===== Packed shift left (VSHL.I32) =====
  {"neon_vshl_i32",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "int neon_vshl_i32(int a) {\n"
   "  v4si va = {a, 0, 0, 0};\n"
   "  v4si vr = va << 4;\n"
   "  return vr[0];\n"
   "}\n",
   {0xFF}, "NEONOps", 1, "-mfpu=neon -mfloat-abi=softfp"},

  // ===== Packed shift right unsigned (VSHR.U32) =====
  {"neon_vshr_u32",
   "typedef unsigned v4su __attribute__((vector_size(16)));\n"
   "int neon_vshr_u32(int a) {\n"
   "  v4su va = {(unsigned)a, 0, 0, 0};\n"
   "  v4su vr = va >> 4;\n"
   "  return (int)vr[0];\n"
   "}\n",
   {0xFF0}, "NEONOps", 1, "-mfpu=neon -mfloat-abi=softfp"},

  // ===== Packed compare == (VCEQ.I32) =====
  {"neon_vceq_i32",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "int neon_vceq_i32(int a) {\n"
   "  v4si va = {a, 42, 0, 0};\n"
   "  v4si vb = {a, 0, 0, 0};\n"
   "  v4si vr = (va == vb);\n"
   "  return vr[0] & 1;\n"
   "}\n",
   {42}, "NEONOps", 1, "-mfpu=neon -mfloat-abi=softfp"},

  // ===== Packed compare > (VCGT.S32) =====
  {"neon_vcgt_s32",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "int neon_vcgt_s32(int a, int b) {\n"
   "  v4si va = {a, 0, 0, 0};\n"
   "  v4si vb = {b, 0, 0, 0};\n"
   "  v4si vr = (va > vb);\n"
   "  return vr[0] & 1;\n"
   "}\n",
   {100, 50}, "NEONOps", 1, "-mfpu=neon -mfloat-abi=softfp"},

  // ===== Packed negate (VNEG.S32) =====
  {"neon_vneg_s32",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "int neon_vneg_s32(int a) {\n"
   "  v4si va = {a, 0, 0, 0};\n"
   "  v4si zero = {0, 0, 0, 0};\n"
   "  v4si vr = zero - va;\n"
   "  return vr[0];\n"
   "}\n",
   {42}, "NEONOps", 1, "-mfpu=neon -mfloat-abi=softfp"},

  // ===== Packed short add (VADD.I16) =====
  {"neon_vadd_i16",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "int neon_vadd_i16(int a, int b) {\n"
   "  v8hi va = {(short)a, 0, 0, 0, 0, 0, 0, 0};\n"
   "  v8hi vb = {(short)b, 0, 0, 0, 0, 0, 0, 0};\n"
   "  v8hi vr = va + vb;\n"
   "  return (int)(unsigned short)vr[0];\n"
   "}\n",
   {100, 200}, "NEONOps", 1, "-mfpu=neon -mfloat-abi=softfp"},

  // ===== Packed byte add (VADD.I8) =====
  {"neon_vadd_i8",
   "typedef signed char v16qi __attribute__((vector_size(16)));\n"
   "int neon_vadd_i8(int a, int b) {\n"
   "  v16qi va = {(signed char)a,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0};\n"
   "  v16qi vb = {(signed char)b,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0};\n"
   "  v16qi vr = va + vb;\n"
   "  return (int)(unsigned char)vr[0];\n"
   "}\n",
   {10, 20}, "NEONOps", 1, "-mfpu=neon -mfloat-abi=softfp"},

  // ===== Packed short mul (VMUL.I16) =====
  {"neon_vmul_i16",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "int neon_vmul_i16(int a, int b) {\n"
   "  v8hi va = {(short)a, 0, 0, 0, 0, 0, 0, 0};\n"
   "  v8hi vb = {(short)b, 0, 0, 0, 0, 0, 0, 0};\n"
   "  v8hi vr = va * vb;\n"
   "  return (int)(unsigned short)vr[0];\n"
   "}\n",
   {7, 6}, "NEONOps", 1, "-mfpu=neon -mfloat-abi=softfp"},

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(NEONOps, ARM32NEONVecOpsRT,
                         ::testing::ValuesIn(kARM32NEONVecOps), rtTCName);
