//===- ARM32_NEONPerLaneRTTests.cpp - ARM32 NEON per-lane roundtrip -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests ARM32 NEON instructions through the full lift pipeline.
// Focus on per-lane operations and edge cases.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32NEONPerLaneRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32NEONPerLaneRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

#define V4I  "typedef int v4i __attribute__((vector_size(16)));\n"
#define V4UI "typedef unsigned int v4ui __attribute__((vector_size(16)));\n"
#define V8S  "typedef short v8s __attribute__((vector_size(16)));\n"
#define V8US "typedef unsigned short v8us __attribute__((vector_size(16)));\n"
#define V16C "typedef char v16c __attribute__((vector_size(16)));\n"
#define V16UC "typedef unsigned char v16uc __attribute__((vector_size(16)));\n"
#define V4F  "typedef float v4f __attribute__((vector_size(16)));\n"

static const std::vector<RoundTripTC> kARM32NEONPerLane = {

  // ===== VADD.I32 — per-lane 32-bit add =====
  {"neon_vadd_4i",
   V4I
   "int neon_vadd_4i(int a, int b) {\n"
   "  v4i va = {a, 10, 0, 0};\n"
   "  v4i vb = {b, 20, 0, 0};\n"
   "  v4i vr = va + vb;\n"
   "  return vr[0] + vr[1];\n"
   "}\n",
   {100, 42}, "NEONPerLane", 1, "-mfpu=neon"},

  // ===== VSUB.I32 — per-lane 32-bit sub =====
  {"neon_vsub_4i",
   V4I
   "int neon_vsub_4i(int a, int b) {\n"
   "  v4i va = {a, 100, 0, 0};\n"
   "  v4i vb = {b, 30, 0, 0};\n"
   "  v4i vr = va - vb;\n"
   "  return vr[0] + vr[1];\n"
   "}\n",
   {100, 42}, "NEONPerLane", 1, "-mfpu=neon"},

  // ===== VADD.I16 — per-lane 16-bit add =====
  {"neon_vadd_8h",
   V8S
   "int neon_vadd_8h(int a, int b) {\n"
   "  v8s va = {(short)a, 10, 0,0,0,0,0,0};\n"
   "  v8s vb = {(short)b, 5, 0,0,0,0,0,0};\n"
   "  v8s vr = va + vb;\n"
   "  return (int)vr[0] + (int)vr[1];\n"
   "}\n",
   {100, 42}, "NEONPerLane", 1, "-mfpu=neon"},

  // ===== VADD.I8 — per-lane 8-bit add =====
  {"neon_vadd_16b",
   V16UC
   "int neon_vadd_16b(int a, int b) {\n"
   "  v16uc va = {(unsigned char)a, 10, 0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16uc vb = {(unsigned char)b, 5, 0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16uc vr = va + vb;\n"
   "  return (int)vr[0] + (int)vr[1];\n"
   "}\n",
   {10, 20}, "NEONPerLane", 1, "-mfpu=neon"},

  // ===== VMUL.I32 — per-lane 32-bit multiply =====
  {"neon_vmul_4i",
   V4I
   "int neon_vmul_4i(int a, int b) {\n"
   "  v4i va = {a, 3, 0, 0};\n"
   "  v4i vb = {b, 7, 0, 0};\n"
   "  v4i vr = va * vb;\n"
   "  return vr[0] + vr[1];\n"
   "}\n",
   {6, 7}, "NEONPerLane", 1, "-mfpu=neon"},

  // ===== VAND/VORR/VEOR — bitwise per vector =====
  {"neon_vand",
   V4UI
   "int neon_vand(int a, int b) {\n"
   "  v4ui va = {(unsigned)a, 0xFF00FF, 0, 0};\n"
   "  v4ui vb = {(unsigned)b, 0x0F0F0F, 0, 0};\n"
   "  v4ui vr = va & vb;\n"
   "  return (int)(vr[0] + vr[1]);\n"
   "}\n",
   {0xFF00FF00u, 0x0F0F0F0Fu}, "NEONPerLane", 1, "-mfpu=neon"},

  {"neon_vorr",
   V4UI
   "int neon_vorr(int a, int b) {\n"
   "  v4ui va = {(unsigned)a, 0, 0, 0};\n"
   "  v4ui vb = {(unsigned)b, 0, 0, 0};\n"
   "  v4ui vr = va | vb;\n"
   "  return (int)vr[0];\n"
   "}\n",
   {0xAAAA5555u, 0x5555AAAAu}, "NEONPerLane", 1, "-mfpu=neon"},

  // ===== VSHL — per-lane left shift =====
  {"neon_vshl_4i",
   V4UI
   "int neon_vshl_4i(int a) {\n"
   "  v4ui va = {(unsigned)a, 1, 0, 0};\n"
   "  v4ui vr = va << 4;\n"
   "  return (int)(vr[0] + vr[1]);\n"
   "}\n",
   {0xFF}, "NEONPerLane", 1, "-mfpu=neon"},

  // ===== VSHR — per-lane right shift =====
  {"neon_vshr_4i",
   V4UI
   "int neon_vshr_4i(int a) {\n"
   "  v4ui va = {(unsigned)a, 0x100, 0, 0};\n"
   "  v4ui vr = va >> 4;\n"
   "  return (int)(vr[0] + vr[1]);\n"
   "}\n",
   {0xFF0}, "NEONPerLane", 1, "-mfpu=neon"},

  // ===== VCEQ — per-lane equality compare =====
  {"neon_vceq_hit",
   V4I
   "int neon_vceq_hit(int a, int b) {\n"
   "  v4i va = {a, 0, 0, 0};\n"
   "  v4i vb = {b, 0, 0, 0};\n"
   "  v4i cmp = (va == vb);\n"
   "  return (unsigned)cmp[0];\n"
   "}\n",
   {42, 42}, "NEONPerLane", 1, "-mfpu=neon"},

  {"neon_vceq_miss",
   V4I
   "int neon_vceq_miss(int a, int b) {\n"
   "  v4i va = {a, 0, 0, 0};\n"
   "  v4i vb = {b, 0, 0, 0};\n"
   "  v4i cmp = (va == vb);\n"
   "  return (unsigned)cmp[0];\n"
   "}\n",
   {42, 99}, "NEONPerLane", 1, "-mfpu=neon"},

  // ===== VCGT — per-lane signed greater than =====
  {"neon_vcgt",
   V4I
   "int neon_vcgt(int a, int b) {\n"
   "  v4i va = {a, 0, 0, 0};\n"
   "  v4i vb = {b, 0, 0, 0};\n"
   "  v4i cmp = (va > vb);\n"
   "  return (unsigned)cmp[0];\n"
   "}\n",
   {99, 42}, "NEONPerLane", 1, "-mfpu=neon"},

  // ===== VNEG — per-lane negate =====
  {"neon_vneg_4i",
   V4I
   "int neon_vneg_4i(int a) {\n"
   "  v4i va = {a, -42, 0, 0};\n"
   "  v4i vr = -va;\n"
   "  return vr[0] + vr[1];\n"
   "}\n",
   {100}, "NEONPerLane", 1, "-mfpu=neon"},

  // ===== NEON FP: scalar VFP add =====
  {"neon_vfp_scalar_add",
   "int neon_vfp_scalar_add(int a, int b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  float r = fa + fb;\n"
   "  int ret; __builtin_memcpy(&ret, &r, 4);\n"
   "  return ret;\n"
   "}\n",
   {0x40A00000u, 0x40400000u}, "NEONPerLane"},

  {"neon_vadd_4f",
   V4F
   "int neon_vadd_4f(int a, int b) {\n"
   "  v4f va = {(float)a, 2.0f, 3.0f, 4.0f};\n"
   "  v4f vb = {(float)b, 5.0f, 6.0f, 7.0f};\n"
   "  v4f vr = va + vb;\n"
   "  return (int)(vr[0] + vr[1] + vr[2] + vr[3]);\n"
   "}\n",
   {10, 20}, "NEONPerLane", 1, "-mfpu=neon"},

  {"neon_vmul_4f",
   V4F
   "int neon_vmul_4f(int a, int b) {\n"
   "  v4f va = {(float)a, 2.0f, 3.0f, 4.0f};\n"
   "  v4f vb = {(float)b, 5.0f, 6.0f, 7.0f};\n"
   "  v4f vr = va * vb;\n"
   "  return (int)(vr[0] + vr[1] + vr[2] + vr[3]);\n"
   "}\n",
   {3, 4}, "NEONPerLane", 1, "-mfpu=neon"},

  {"neon_vfp_scalar_mul",
   "int neon_vfp_scalar_mul(int a, int b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  float r = fa * fb;\n"
   "  int ret; __builtin_memcpy(&ret, &r, 4);\n"
   "  return ret;\n"
   "}\n",
   {0x40A00000u, 0x40400000u}, "NEONPerLane"},

  // ===== VMUL.I16 — per-lane 16-bit multiply =====
  {"neon_vmul_8h",
   V8S
   "int neon_vmul_8h(int a, int b) {\n"
   "  v8s va = {(short)a, 5, 0,0,0,0,0,0};\n"
   "  v8s vb = {(short)b, 3, 0,0,0,0,0,0};\n"
   "  v8s vr = va * vb;\n"
   "  return (int)vr[0] + (int)vr[1];\n"
   "}\n",
   {6, 7}, "NEONPerLane", 1, "-mfpu=neon"},

  // ===== Overflow edge: VADD.I32 with carry crossing =====
  {"neon_vadd_overflow",
   V4UI
   "int neon_vadd_overflow(int a) {\n"
   "  v4ui va = {(unsigned)a, 1, 0, 0};\n"
   "  v4ui vb = {1, 0xFFFFFFFF, 0, 0};\n"
   "  v4ui vr = va + vb;\n"
   "  return (int)(vr[0] + vr[1]);\n"
   "}\n",
   {0xFFFFFFFEu}, "NEONPerLane", 1, "-mfpu=neon"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(ARM32NEONPerLane, ARM32NEONPerLaneRT,
                         ::testing::ValuesIn(kARM32NEONPerLane), rtTCName);
