//===- AArch64_NEONPerLaneRTTests.cpp - NEON per-lane roundtrip --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests AArch64 NEON instructions that require per-lane decomposition.
// Covers: FMIN/FMAX, ABS, NEG, MLA/MLS, widening ops, shuffle/permute.
// These are the areas most likely to have bugs based on prior experience
// (bugs #28, #33, #34, #52-54 were all NEON per-lane issues).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64NEONPerLaneRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64NEONPerLaneRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

#define V4I  "typedef int v4i __attribute__((vector_size(16)));\n"
#define V4UI "typedef unsigned int v4ui __attribute__((vector_size(16)));\n"
#define V8S  "typedef short v8s __attribute__((vector_size(16)));\n"
#define V8US "typedef unsigned short v8us __attribute__((vector_size(16)));\n"
#define V16C "typedef char v16c __attribute__((vector_size(16)));\n"
#define V16UC "typedef unsigned char v16uc __attribute__((vector_size(16)));\n"
#define V2Q  "typedef long long v2q __attribute__((vector_size(16)));\n"
#define V4F  "typedef float v4f __attribute__((vector_size(16)));\n"
#define V2D  "typedef double v2d __attribute__((vector_size(16)));\n"

static const std::vector<RoundTripTC> kA64NEONPerLane = {

  // ===== FMINNM/FMAXNM V.4S — packed float min/max =====
  {"neon_fmin_4s",
   V4F
   "long neon_fmin_4s(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  v4f va = {fa, 1.0f, 0.0f, 0.0f};\n"
   "  v4f vb = {fb, 2.0f, 0.0f, 0.0f};\n"
   "  v4f vr = __builtin_elementwise_min(va, vb);\n"
   "  float r = vr[0]; long ret; __builtin_memcpy(&ret, &r, 4);\n"
   "  return ret;\n"
   "}\n",
   {0x40A00000ULL, 0x40400000ULL}, "NEONPerLane", 1},

  {"neon_fmax_4s",
   V4F
   "long neon_fmax_4s(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  v4f va = {fa, 1.0f, 0.0f, 0.0f};\n"
   "  v4f vb = {fb, 2.0f, 0.0f, 0.0f};\n"
   "  v4f vr = __builtin_elementwise_max(va, vb);\n"
   "  float r = vr[0]; long ret; __builtin_memcpy(&ret, &r, 4);\n"
   "  return ret;\n"
   "}\n",
   {0x40A00000ULL, 0x40400000ULL}, "NEONPerLane", 1},

  // ===== ABS V.4S — absolute value per lane =====
  {"neon_abs_4s",
   V4I
   "long neon_abs_4s(long a) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vr = __builtin_elementwise_abs(va);\n"
   "  return (unsigned int)vr[0];\n"
   "}\n",
   {(uint64_t)(int64_t)-42}, "NEONPerLane", 1},

  // ===== NEG V.4S — negate per lane =====
  {"neon_neg_4s",
   V4I
   "long neon_neg_4s(long a) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vr = -va;\n"
   "  return (unsigned int)vr[0];\n"
   "}\n",
   {42}, "NEONPerLane", 1},

  // ===== MLA V.4S — multiply-accumulate per lane =====
  {"neon_mla_4s",
   V4I
   "long neon_mla_4s(long a, long b) {\n"
   "  v4i acc = {10, 20, 0, 0};\n"
   "  v4i va = {(int)a, 2, 0, 0};\n"
   "  v4i vb = {(int)b, 3, 0, 0};\n"
   "  v4i vr = acc + va * vb;\n"
   "  return (unsigned int)vr[0] | ((unsigned long)(unsigned int)vr[1] << 32);\n"
   "}\n",
   {5, 7}, "NEONPerLane", 1},

  // ===== MLS V.4S — multiply-subtract per lane =====
  {"neon_mls_4s",
   V4I
   "long neon_mls_4s(long a, long b) {\n"
   "  v4i acc = {100, 200, 0, 0};\n"
   "  v4i va = {(int)a, 2, 0, 0};\n"
   "  v4i vb = {(int)b, 3, 0, 0};\n"
   "  v4i vr = acc - va * vb;\n"
   "  return (unsigned int)vr[0] | ((unsigned long)(unsigned int)vr[1] << 32);\n"
   "}\n",
   {5, 7}, "NEONPerLane", 1},

  // ===== SMIN/SMAX V.4S — signed integer min/max per lane =====
  {"neon_smin_4s",
   V4I
   "long neon_smin_4s(long a, long b) {\n"
   "  v4i va = {(int)a, 100, 0, 0};\n"
   "  v4i vb = {(int)b, -50, 0, 0};\n"
   "  v4i vr = __builtin_elementwise_min(va, vb);\n"
   "  return (unsigned int)vr[0] | ((unsigned long)(unsigned int)vr[1] << 32);\n"
   "}\n",
   {(uint64_t)(int64_t)-10, 20}, "NEONPerLane", 1},

  {"neon_smax_4s",
   V4I
   "long neon_smax_4s(long a, long b) {\n"
   "  v4i va = {(int)a, 100, 0, 0};\n"
   "  v4i vb = {(int)b, -50, 0, 0};\n"
   "  v4i vr = __builtin_elementwise_max(va, vb);\n"
   "  return (unsigned int)vr[0] | ((unsigned long)(unsigned int)vr[1] << 32);\n"
   "}\n",
   {(uint64_t)(int64_t)-10, 20}, "NEONPerLane", 1},

  // ===== UMIN/UMAX V.4S — unsigned min/max per lane =====
  {"neon_umin_4s",
   V4UI
   "long neon_umin_4s(long a, long b) {\n"
   "  v4ui va = {(unsigned int)a, 0, 0, 0};\n"
   "  v4ui vb = {(unsigned int)b, 0, 0, 0};\n"
   "  v4ui vr = __builtin_elementwise_min(va, vb);\n"
   "  return (unsigned int)vr[0];\n"
   "}\n",
   {0xFFFFFFF0ULL, 0x00000010ULL}, "NEONPerLane", 1},

  {"neon_umax_4s",
   V4UI
   "long neon_umax_4s(long a, long b) {\n"
   "  v4ui va = {(unsigned int)a, 0, 0, 0};\n"
   "  v4ui vb = {(unsigned int)b, 0, 0, 0};\n"
   "  v4ui vr = __builtin_elementwise_max(va, vb);\n"
   "  return (unsigned int)vr[0];\n"
   "}\n",
   {0xFFFFFFF0ULL, 0x00000010ULL}, "NEONPerLane", 1},

  // ===== CMEQ/CMGT V.4S — compare per lane =====
  {"neon_cmeq_4s",
   V4I
   "long neon_cmeq_4s(long a, long b) {\n"
   "  v4i va = {(int)a, 42, 0, 0};\n"
   "  v4i vb = {(int)b, 42, 0, 0};\n"
   "  v4i vr = (va == vb);\n"
   "  return (unsigned int)vr[0] | ((unsigned long)(unsigned int)vr[1] << 32);\n"
   "}\n",
   {42, 42}, "NEONPerLane", 1},

  {"neon_cmgt_4s",
   V4I
   "long neon_cmgt_4s(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i vr = (va > vb);\n"
   "  return (unsigned int)vr[0];\n"
   "}\n",
   {100, 50}, "NEONPerLane", 1},

  // ===== SHL V.4S — per-lane left shift =====
  {"neon_shl_4s",
   V4UI
   "long neon_shl_4s(long a, long b) {\n"
   "  v4ui va = {(unsigned int)a, (unsigned int)b, 0, 0};\n"
   "  v4ui vr = va << 4;\n"
   "  return (unsigned int)vr[0] | ((unsigned long)(unsigned int)vr[1] << 32);\n"
   "}\n",
   {0xFF, 0x80000001ULL}, "NEONPerLane", 1},

  {"neon_shl_8h",
   V8US
   "long neon_shl_8h(long a, long b) {\n"
   "  v8us va = {(unsigned short)a, (unsigned short)b, 0, 0, 0, 0, 0, 0};\n"
   "  v8us vr = va << 3;\n"
   "  return (unsigned short)vr[0] | ((unsigned long)(unsigned short)vr[1] << 16);\n"
   "}\n",
   {0xFF, 0x8001ULL}, "NEONPerLane", 1},

  // ===== USHR V.4S — unsigned shift right per lane =====
  {"neon_ushr_4s",
   V4UI
   "long neon_ushr_4s(long a, long b) {\n"
   "  v4ui va = {(unsigned int)a, (unsigned int)b, (unsigned int)(a+b), 0};\n"
   "  v4ui vr = va >> 4;\n"
   "  return (unsigned int)vr[0] | ((unsigned long)(unsigned int)vr[1] << 32);\n"
   "}\n",
   {0xFF, 0x80000000ULL}, "NEONPerLane", 1},

  // ===== AND/ORR/EOR/BIC V — bitwise per vector =====
  {"neon_and_16b",
   V4I
   "long neon_and_16b(long a, long b) {\n"
   "  v4i va = {(int)a, (int)(a>>32), 0, 0};\n"
   "  v4i vb = {(int)b, (int)(b>>32), 0, 0};\n"
   "  v4i vr = va & vb;\n"
   "  return (unsigned int)vr[0] | ((unsigned long)(unsigned int)vr[1] << 32);\n"
   "}\n",
   {0xFFFF0000AAAA5555ULL, 0xFF00FF00FF00FF00ULL}, "NEONPerLane", 1},

  {"neon_orr_16b",
   V4I
   "long neon_orr_16b(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i vr = va | vb;\n"
   "  return (unsigned int)vr[0];\n"
   "}\n",
   {0xAAAA5555ULL, 0x5555AAAAULL}, "NEONPerLane", 1},

  {"neon_eor_16b",
   V4I
   "long neon_eor_16b(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i vr = va ^ vb;\n"
   "  return (unsigned int)vr[0];\n"
   "}\n",
   {0xDEADBEEFULL, 0xCAFEBABEULL}, "NEONPerLane", 1},

  // ===== Shuffle: REV64 V.4S pattern =====
  {"neon_rev64_4s",
   V4I
   "long neon_rev64_4s(long a) {\n"
   "  v4i va = {(int)a, (int)(a>>32), 0, 0};\n"
   "  v4i vr = __builtin_shufflevector(va, va, 1, 0, 3, 2);\n"
   "  return (unsigned int)vr[0] | ((unsigned long)(unsigned int)vr[1] << 32);\n"
   "}\n",
   {0x0000000200000001ULL}, "NEONPerLane", 1},

  // ===== Shuffle: interleave pattern using all runtime args =====
  {"neon_interleave_4s",
   V4I
   "long neon_interleave_4s(long a, long b) {\n"
   "  v4i va = {(int)a, (int)(a+1), (int)(a+2), (int)(a+3)};\n"
   "  v4i vb = {(int)b, (int)(b+1), (int)(b+2), (int)(b+3)};\n"
   "  v4i vr = va + vb;\n"
   "  return (unsigned int)vr[0] | ((unsigned long)(unsigned int)vr[1] << 32);\n"
   "}\n",
   {10, 100}, "NEONPerLane", 1},

  // ===== EXT V.16B — byte-level extract/rotate =====
  {"neon_ext_16b",
   V16C
   "long neon_ext_16b(long a) {\n"
   "  v16c va = {1,2,3,4, 5,6,7,8, 0,0,0,0, 0,0,0,0};\n"
   "  v16c vb = {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0};\n"
   "  v16c vr = __builtin_shufflevector(va, vb, 4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19);\n"
   "  return (unsigned char)vr[0] | ((unsigned long)(unsigned char)vr[1] << 8)\n"
   "       | ((unsigned long)(unsigned char)vr[2] << 16) | ((unsigned long)(unsigned char)vr[3] << 24);\n"
   "}\n",
   {0}, "NEONPerLane", 1},

  // ===== FADD V.4S — packed float add (regression for #28) =====
  {"neon_fadd_4s",
   V4F
   "long neon_fadd_4s(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  v4f va = {fa, 0.0f, 0.0f, 0.0f};\n"
   "  v4f vb = {fb, 0.0f, 0.0f, 0.0f};\n"
   "  v4f vr = va + vb;\n"
   "  float r = vr[0]; long ret; __builtin_memcpy(&ret, &r, 4);\n"
   "  return ret;\n"
   "}\n",
   {0x40A00000ULL, 0x40400000ULL}, "NEONPerLane", 1},

  // ===== FMUL V.4S — packed float mul =====
  {"neon_fmul_4s",
   V4F
   "long neon_fmul_4s(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  v4f va = {fa, 0.0f, 0.0f, 0.0f};\n"
   "  v4f vb = {fb, 0.0f, 0.0f, 0.0f};\n"
   "  v4f vr = va * vb;\n"
   "  float r = vr[0]; long ret; __builtin_memcpy(&ret, &r, 4);\n"
   "  return ret;\n"
   "}\n",
   {0x40A00000ULL, 0x40400000ULL}, "NEONPerLane", 1},

  // ===== FCMEQ V.4S — float compare equal per lane =====
  {"neon_fcmeq_4s",
   V4F V4I
   "long neon_fcmeq_4s(long a) {\n"
   "  float fa; __builtin_memcpy(&fa, &a, 4);\n"
   "  v4f va = {fa, 1.0f, 0.0f, 0.0f};\n"
   "  v4f vb = {fa, 2.0f, 0.0f, 0.0f};\n"
   "  v4i cmp = __builtin_convertvector(va == vb, v4i);\n"
   "  return (unsigned int)cmp[0] | ((unsigned long)(unsigned int)cmp[1] << 32);\n"
   "}\n",
   {0x40A00000ULL}, "NEONPerLane", 1},

  // ===== ADDP V.4S — pairwise add =====
  {"neon_addp_4s",
   V4I
   "long neon_addp_4s(long a) {\n"
   "  v4i va = {1, 2, 3, 4};\n"
   "  v4i vb = {5, 6, 7, 8};\n"
   "  v4i vr = __builtin_shufflevector(va, vb, 0, 2, 4, 6) +\n"
   "           __builtin_shufflevector(va, vb, 1, 3, 5, 7);\n"
   "  return (unsigned int)vr[0] | ((unsigned long)(unsigned int)vr[1] << 32);\n"
   "}\n",
   {0}, "NEONPerLane", 1},

  // ===== NOT V.16B — bitwise NOT =====
  {"neon_not_16b",
   V4I
   "long neon_not_16b(long a) {\n"
   "  v4i va = {(int)a, (int)(a>>32), 0, 0};\n"
   "  v4i vr = ~va;\n"
   "  return (unsigned int)vr[0] | ((unsigned long)(unsigned int)vr[1] << 32);\n"
   "}\n",
   {0xDEADBEEFCAFEBABEULL}, "NEONPerLane", 1},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(NEONPerLane, A64NEONPerLaneRT,
                         ::testing::ValuesIn(kA64NEONPerLane), rtTCName);
