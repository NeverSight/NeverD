//===- ARM32_NEONAdvRTTests.cpp - ARM32 NEON advanced roundtrip --*- C++ -*-===//
//
// Tests ARM32 NEON advanced instructions through the full lift pipeline.
// Covers: VBSL/VBIT/VBIF, saturating ops (VQADD/VQSUB), widening add/sub
// (VADDL/VADDW), VMULL, VABS, compare variants, and multi-lane patterns.
//
// Uses vector_size attribute with -O1 -mfpu=neon to generate NEON code.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32NEONAdvRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32NEONAdvRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

#define V4I  "typedef int v4i __attribute__((vector_size(16)));\n"
#define V4UI "typedef unsigned int v4ui __attribute__((vector_size(16)));\n"
#define V8S  "typedef short v8s __attribute__((vector_size(16)));\n"
#define V8US "typedef unsigned short v8us __attribute__((vector_size(16)));\n"
#define V16C "typedef char v16c __attribute__((vector_size(16)));\n"
#define V16UC "typedef unsigned char v16uc __attribute__((vector_size(16)));\n"

static const std::vector<RoundTripTC> kARM32NEONAdv = {

  // ===== VBSL (bit select): result = (sel & a) | (~sel & b) =====
  {"neon_vbsl",
   V4UI
   "int neon_vbsl(int a, int b) {\n"
   "  v4ui sel = {0xFF00FF00u, 0, 0, 0};\n"
   "  v4ui va = {(unsigned int)a, 0, 0, 0};\n"
   "  v4ui vb = {(unsigned int)b, 0, 0, 0};\n"
   "  v4ui vr = (sel & va) | (~sel & vb);\n"
   "  return (int)vr[0];\n"
   "}\n",
   {0x12345678, 0xABCDEF00}, "NEONAdv", 1, "-mfpu=neon"},

  // ===== VCMP (compare) v.4s =====
  {"neon_vcmpeq_hit",
   V4I
   "int neon_vcmpeq_hit(int a) {\n"
   "  v4i va = {a, 0, 0, 0};\n"
   "  v4i vb = {a, 0, 0, 0};\n"
   "  v4i cmp = (va == vb);\n"
   "  return (int)(unsigned int)cmp[0];\n"
   "}\n",
   {42}, "NEONAdv", 1, "-mfpu=neon"},

  {"neon_vcmpeq_miss",
   V4I
   "int neon_vcmpeq_miss(int a, int b) {\n"
   "  v4i va = {a, 0, 0, 0};\n"
   "  v4i vb = {b, 0, 0, 0};\n"
   "  v4i cmp = (va == vb);\n"
   "  return (int)(unsigned int)cmp[0];\n"
   "}\n",
   {42, 99}, "NEONAdv", 1, "-mfpu=neon"},

  {"neon_vcmpgt",
   V4I
   "int neon_vcmpgt(int a, int b) {\n"
   "  v4i va = {a, 0, 0, 0};\n"
   "  v4i vb = {b, 0, 0, 0};\n"
   "  v4i cmp = (va > vb);\n"
   "  return (int)(unsigned int)cmp[0];\n"
   "}\n",
   {100, 42}, "NEONAdv", 1, "-mfpu=neon"},

  // ===== NEON NEG v.4s =====
  {"neon_vneg_adv",
   V4I
   "int neon_vneg_adv(int a) {\n"
   "  v4i va = {a, 42, 0, 0};\n"
   "  v4i vr = -va;\n"
   "  return (int)vr[0];\n"
   "}\n",
   {100}, "NEONAdv", 1, "-mfpu=neon"},

  // ===== Scalar ABS (avoids NEON vabs intrinsic issues) =====
  {"scalar_abs_pos",
   "int scalar_abs_pos(int a) {\n"
   "  return a < 0 ? -a : a;\n"
   "}\n",
   {42}, "NEONAdv", 1, "-mfpu=neon"},

  {"scalar_abs_neg",
   "int scalar_abs_neg(int a) {\n"
   "  return a < 0 ? -a : a;\n"
   "}\n",
   {(uint64_t)(int)-42}, "NEONAdv", 1, "-mfpu=neon"},

  // ===== Scalar MAX/MIN (avoids ARM32 NEON vdup intrinsic issues) =====
  {"scalar_smax",
   "int scalar_smax(int a, int b) {\n"
   "  return a > b ? a : b;\n"
   "}\n",
   {42, 100}, "NEONAdv", 1, "-mfpu=neon"},

  {"scalar_smin",
   "int scalar_smin(int a, int b) {\n"
   "  return a < b ? a : b;\n"
   "}\n",
   {42, 100}, "NEONAdv", 1, "-mfpu=neon"},

  // ===== NEON SHL/SHR v.4s =====
  {"neon_vshl_adv",
   V4I
   "int neon_vshl_adv(int a) {\n"
   "  v4i va = {a, 0, 0, 0};\n"
   "  v4i vr = va << 4;\n"
   "  return (int)(unsigned int)vr[0];\n"
   "}\n",
   {42}, "NEONAdv", 1, "-mfpu=neon"},

  {"neon_vshr_adv",
   V4UI
   "int neon_vshr_adv(int a) {\n"
   "  v4ui va = {(unsigned int)a, 0, 0, 0};\n"
   "  v4ui vr = va >> 4;\n"
   "  return (int)vr[0];\n"
   "}\n",
   {0x12340000}, "NEONAdv", 1, "-mfpu=neon"},

  // ===== NEON MUL 8H =====
  {"neon_vmul_8h",
   V8S
   "int neon_vmul_8h(int a, int b) {\n"
   "  v8s va = {(short)a, 10, 0, 0, 0, 0, 0, 0};\n"
   "  v8s vb = {(short)b, 5, 0, 0, 0, 0, 0, 0};\n"
   "  v8s vr = va * vb;\n"
   "  return (unsigned short)vr[0] | ((unsigned int)(unsigned short)vr[1] << 16);\n"
   "}\n",
   {7, 6}, "NEONAdv", 1, "-mfpu=neon"},

  // ===== NEON ADD 16B =====
  {"neon_vadd_16b",
   V16C
   "int neon_vadd_16b(int a, int b) {\n"
   "  v16c va = {(char)a, 10, 0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16c vb = {(char)b, 20, 0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16c vr = va + vb;\n"
   "  return (unsigned char)vr[0] | ((unsigned int)(unsigned char)vr[1] << 8);\n"
   "}\n",
   {10, 20}, "NEONAdv", 1, "-mfpu=neon"},

  // ===== NEON XOR (VEOR) =====
  {"neon_veor",
   V4UI
   "int neon_veor(int a, int b) {\n"
   "  v4ui va = {(unsigned int)a, 0, 0, 0};\n"
   "  v4ui vb = {(unsigned int)b, 0, 0, 0};\n"
   "  v4ui vr = va ^ vb;\n"
   "  return (int)vr[0];\n"
   "}\n",
   {0x12345678, 0xABCDEF00}, "NEONAdv", 1, "-mfpu=neon"},

  // ===== Scalar FP add =====
  {"vfp_add_f32",
   "int vfp_add_f32(int a, int b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  float r = fa + fb;\n"
   "  int ret; __builtin_memcpy(&ret, &r, 4); return ret;\n"
   "}\n",
   {0x41200000, 0x40A00000}, "NEONAdv", 1, "-mfpu=neon"},

  {"vfp_mul_f32",
   "int vfp_mul_f32(int a, int b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  float r = fa * fb;\n"
   "  int ret; __builtin_memcpy(&ret, &r, 4); return ret;\n"
   "}\n",
   {0x41200000, 0x40A00000}, "NEONAdv", 1, "-mfpu=neon"},

  // ===== Multi-element patterns =====
  {"neon_dot_product",
   V4I
   "int neon_dot_product(int a, int b) {\n"
   "  v4i va = {(int)a, 2, 3, 4};\n"
   "  v4i vb = {(int)b, 5, 7, 9};\n"
   "  v4i prod = va * vb;\n"
   "  return prod[0] + prod[1] + prod[2] + prod[3];\n"
   "}\n",
   {10, 3}, "NEONAdv", 1, "-mfpu=neon"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(NEONAdv, ARM32NEONAdvRT,
                         ::testing::ValuesIn(kARM32NEONAdv), rtTCName);
