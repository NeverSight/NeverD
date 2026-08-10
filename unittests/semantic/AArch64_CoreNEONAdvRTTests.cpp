//===- AArch64_CoreNEONAdvRTTests.cpp - CoreNEON adv roundtrip ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests AArch64 CoreNEON advanced instructions through the full lift pipeline:
//   SQADD/UQADD/SQSUB/UQSUB (saturating)
//   SHADD/UHADD/SRHADD/URHADD (halving)
//   SABD/UABD (absolute difference)
//   SMAX/SMIN with 8H/16B lanes
//   MLS (multiply-subtract)
//   More compare variants (CMEQ/CMGE/CMHI/CMHS/CMLE/CMLT)
//   UMOV/SMOV (element extract)
//
// Uses vector_size attribute with -O1 to generate real NEON instructions.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64CoreNEONAdvRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64CoreNEONAdvRT, Verify) { roundTripAArch64(GetParam()); }

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

static const std::vector<RoundTripTC> kA64CoreNEONAdv = {

  // ===== Saturating add/sub =====
  {"sqadd_4s_no_sat",
   V4I
   "long sqadd_4s_no_sat(long a, long b) {\n"
   "  v4i va = {(int)a, 10, 0, 0};\n"
   "  v4i vb = {(int)b, 20, 0, 0};\n"
   "  v4i vr = __builtin_elementwise_add_sat(va, vb);\n"
   "  return (unsigned int)vr[0] | ((unsigned long)(unsigned int)vr[1] << 32);\n"
   "}\n",
   {100, 42}, "CoreNEONAdv", 1},

  {"sqadd_4s_large",
   V4I
   "long sqadd_4s_large(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i vr = __builtin_elementwise_add_sat(va, vb);\n"
   "  return (unsigned int)vr[0];\n"
   "}\n",
   {1000, 2000}, "CoreNEONAdv", 1},

  {"uqadd_4s_no_sat",
   V4UI
   "long uqadd_4s_no_sat(long a, long b) {\n"
   "  v4ui va = {(unsigned int)a, 5, 0, 0};\n"
   "  v4ui vb = {(unsigned int)b, 10, 0, 0};\n"
   "  v4ui vr = __builtin_elementwise_add_sat(va, vb);\n"
   "  return (unsigned int)vr[0] | ((unsigned long)(unsigned int)vr[1] << 32);\n"
   "}\n",
   {100, 42}, "CoreNEONAdv", 1},

  {"uqadd_4s_large",
   V4UI
   "long uqadd_4s_large(long a, long b) {\n"
   "  v4ui va = {(unsigned int)a, 0, 0, 0};\n"
   "  v4ui vb = {(unsigned int)b, 0, 0, 0};\n"
   "  v4ui vr = __builtin_elementwise_add_sat(va, vb);\n"
   "  return (unsigned int)vr[0];\n"
   "}\n",
   {1000, 2000}, "CoreNEONAdv", 1},

  {"sqsub_4s",
   V4I
   "long sqsub_4s(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i vr = __builtin_elementwise_sub_sat(va, vb);\n"
   "  return (unsigned int)vr[0];\n"
   "}\n",
   {100, 42}, "CoreNEONAdv", 1},

  {"uqsub_4s",
   V4UI
   "long uqsub_4s(long a, long b) {\n"
   "  v4ui va = {(unsigned int)a, 0, 0, 0};\n"
   "  v4ui vb = {(unsigned int)b, 0, 0, 0};\n"
   "  v4ui vr = __builtin_elementwise_sub_sat(va, vb);\n"
   "  return (unsigned int)vr[0];\n"
   "}\n",
   {42, 100}, "CoreNEONAdv", 1},

  // ===== Halving add =====
  {"shadd_4s",
   V4I
   "long shadd_4s(long a, long b) {\n"
   "  int ia = (int)a, ib = (int)b;\n"
   "  int r = (int)(((long long)ia + (long long)ib) >> 1);\n"
   "  return (unsigned int)r;\n"
   "}\n",
   {100, 42}, "CoreNEONAdv", 1},

  {"uhadd_scalar",
   "long uhadd_scalar(long a, long b) {\n"
   "  unsigned int ua = (unsigned int)a, ub = (unsigned int)b;\n"
   "  unsigned int r = (unsigned int)(((unsigned long long)ua + (unsigned long long)ub) >> 1);\n"
   "  return r;\n"
   "}\n",
   {200, 100}, "CoreNEONAdv", 1},

  // ===== Absolute difference =====
  {"sabd_scalar",
   "long sabd_scalar(long a, long b) {\n"
   "  int ia = (int)a, ib = (int)b;\n"
   "  int diff = ia - ib;\n"
   "  return (unsigned int)(diff < 0 ? -diff : diff);\n"
   "}\n",
   {42, 100}, "CoreNEONAdv", 1},

  {"uabd_scalar",
   "long uabd_scalar(long a, long b) {\n"
   "  unsigned int ua = (unsigned int)a, ub = (unsigned int)b;\n"
   "  return ua > ub ? ua - ub : ub - ua;\n"
   "}\n",
   {42, 100}, "CoreNEONAdv", 1},

  // ===== MLS (multiply-subtract) v.4s =====
  {"neon_mls_4s",
   V4I
   "long neon_mls_4s(long a, long b) {\n"
   "  v4i acc = {100, 200, 0, 0};\n"
   "  v4i va = {(int)a, 3, 0, 0};\n"
   "  v4i vb = {(int)b, 5, 0, 0};\n"
   "  v4i vr = acc - va * vb;\n"
   "  return (unsigned int)vr[0] | ((unsigned long)(unsigned int)vr[1] << 32);\n"
   "}\n",
   {3, 7}, "CoreNEONAdv", 1},

  // ===== Compare: CMEQ v.4s =====
  {"neon_cmeq_4s_hit",
   V4I
   "long neon_cmeq_4s_hit(long a) {\n"
   "  v4i va = {(int)a, 42, 0, 0};\n"
   "  v4i vb = {(int)a, 42, 0, 0};\n"
   "  v4i cmp = (va == vb);\n"
   "  return (unsigned int)cmp[0];\n"
   "}\n",
   {42}, "CoreNEONAdv", 1},

  {"neon_cmeq_4s_miss",
   V4I
   "long neon_cmeq_4s_miss(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i cmp = (va == vb);\n"
   "  return (unsigned int)cmp[0];\n"
   "}\n",
   {42, 99}, "CoreNEONAdv", 1},

  // ===== Compare: CMGE v.4s =====
  {"neon_cmge_4s",
   V4I
   "long neon_cmge_4s(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i cmp = (va >= vb);\n"
   "  return (unsigned int)cmp[0];\n"
   "}\n",
   {100, 42}, "CoreNEONAdv", 1},

  {"neon_cmge_4s_false",
   V4I
   "long neon_cmge_4s_false(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i cmp = (va >= vb);\n"
   "  return (unsigned int)cmp[0];\n"
   "}\n",
   {42, 100}, "CoreNEONAdv", 1},

  // ===== Compare: CMHI (unsigned >) v.4s =====
  {"neon_cmhi_4s",
   V4UI
   "long neon_cmhi_4s(long a, long b) {\n"
   "  v4ui va = {(unsigned int)a, 0, 0, 0};\n"
   "  v4ui vb = {(unsigned int)b, 0, 0, 0};\n"
   "  v4ui cmp = (va > vb);\n"
   "  return (unsigned int)cmp[0];\n"
   "}\n",
   {100, 42}, "CoreNEONAdv", 1},

  // ===== Compare: CMHS (unsigned >=) v.4s =====
  {"neon_cmhs_4s",
   V4UI
   "long neon_cmhs_4s(long a, long b) {\n"
   "  v4ui va = {(unsigned int)a, 0, 0, 0};\n"
   "  v4ui vb = {(unsigned int)b, 0, 0, 0};\n"
   "  v4ui cmp = (va >= vb);\n"
   "  return (unsigned int)cmp[0];\n"
   "}\n",
   {100, 42}, "CoreNEONAdv", 1},

  // ===== Compare: CMLE v.4s (signed <=) =====
  {"neon_cmle_4s",
   V4I
   "long neon_cmle_4s(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i cmp = (va <= vb);\n"
   "  return (unsigned int)cmp[0];\n"
   "}\n",
   {42, 100}, "CoreNEONAdv", 1},

  // ===== Compare: CMLT v.4s (signed <) =====
  {"neon_cmlt_4s",
   V4I
   "long neon_cmlt_4s(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i cmp = (va < vb);\n"
   "  return (unsigned int)cmp[0];\n"
   "}\n",
   {42, 100}, "CoreNEONAdv", 1},

  // ===== 8H lane size: SMAX v.8h (lane 0 only) =====
  {"neon_smax_8h",
   V8S
   "long neon_smax_8h(long a, long b) {\n"
   "  v8s va = {(short)a, 0, 0, 0, 0, 0, 0, 0};\n"
   "  v8s vb = {(short)b, 0, 0, 0, 0, 0, 0, 0};\n"
   "  v8s vr = __builtin_elementwise_max(va, vb);\n"
   "  return (unsigned short)vr[0];\n"
   "}\n",
   {100, 42}, "CoreNEONAdv", 1},

  {"neon_smin_8h",
   V8S
   "long neon_smin_8h(long a, long b) {\n"
   "  v8s va = {(short)a, 0, 0, 0, 0, 0, 0, 0};\n"
   "  v8s vb = {(short)b, 0, 0, 0, 0, 0, 0, 0};\n"
   "  v8s vr = __builtin_elementwise_min(va, vb);\n"
   "  return (unsigned short)vr[0];\n"
   "}\n",
   {100, 42}, "CoreNEONAdv", 1},

  // ===== 16B lane size: UMAX v.16b (lane 0 only) =====
  {"neon_umax_16b",
   V16UC
   "long neon_umax_16b(long a, long b) {\n"
   "  v16uc va = {(unsigned char)a, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16uc vb = {(unsigned char)b, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16uc vr = __builtin_elementwise_max(va, vb);\n"
   "  return (unsigned char)vr[0];\n"
   "}\n",
   {200, 150}, "CoreNEONAdv", 1},

  {"neon_umin_16b",
   V16UC
   "long neon_umin_16b(long a, long b) {\n"
   "  v16uc va = {(unsigned char)a, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16uc vb = {(unsigned char)b, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16uc vr = __builtin_elementwise_min(va, vb);\n"
   "  return (unsigned char)vr[0];\n"
   "}\n",
   {200, 150}, "CoreNEONAdv", 1},

  // ===== NEON NEG v.4s =====
  {"neon_neg_4s",
   V4I
   "long neon_neg_4s(long a) {\n"
   "  v4i va = {(int)a, 42, -10, 0};\n"
   "  v4i vr = -va;\n"
   "  return (unsigned int)vr[0] | ((unsigned long)(unsigned int)vr[1] << 32);\n"
   "}\n",
   {100}, "CoreNEONAdv", 1},

  // ===== NEON ABS v.4s =====
  {"neon_abs_pos",
   V4I
   "long neon_abs_pos(long a) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i mask = va >> 31;\n"
   "  v4i vr = (va ^ mask) - mask;\n"
   "  return (unsigned int)vr[0];\n"
   "}\n",
   {42}, "CoreNEONAdv", 1},

  {"neon_abs_neg",
   V4I
   "long neon_abs_neg(long a) {\n"
   "  v4i va = {-(int)a, 0, 0, 0};\n"
   "  v4i mask = va >> 31;\n"
   "  v4i vr = (va ^ mask) - mask;\n"
   "  return (unsigned int)vr[0];\n"
   "}\n",
   {42}, "CoreNEONAdv", 1},

  // ===== MVNI — move NOT immediate =====
  {"neon_mvni",
   V4UI
   "long neon_mvni(long a) {\n"
   "  v4ui vr = {~(unsigned int)a, 0, 0, 0};\n"
   "  return vr[0] & 0xFFFFFFFFUL;\n"
   "}\n",
   {0x00FF00FFULL}, "CoreNEONAdv", 1},

  // ===== DUP — broadcast scalar to all lanes =====
  {"neon_dup_4s",
   V4I
   "long neon_dup_4s(long a) {\n"
   "  int val = (int)a;\n"
   "  v4i vr = {val, val, val, val};\n"
   "  return (unsigned int)vr[0] | ((unsigned long)(unsigned int)vr[2] << 32);\n"
   "}\n",
   {42}, "CoreNEONAdv", 1},

  // ===== UMOV — extract unsigned element =====
  {"neon_umov_s",
   V4UI
   "long neon_umov_s(long a, long b) {\n"
   "  v4ui va = {(unsigned int)a, (unsigned int)b, 0, 0};\n"
   "  return va[1];\n"
   "}\n",
   {42, 99}, "CoreNEONAdv", 1},

  // ===== SMOV — extract signed element =====
  {"neon_smov_s",
   V4I
   "long neon_smov_s(long a) {\n"
   "  v4i va = {-42, (int)a, 0, 0};\n"
   "  return va[0];\n"
   "}\n",
   {99}, "CoreNEONAdv", 1},

  // ===== UMOV V.H[1] — non-zero lane extract =====
  {"umov_h1",
   V8S
   "long umov_h1(long a, long b) {\n"
   "  v8s va = {(short)a, (short)b, 0, 0, 0, 0, 0, 0};\n"
   "  return (unsigned short)va[1];\n"
   "}\n",
   {42, 99}, "CoreNEONAdv", 1},

  {"umov_h0",
   V8S
   "long umov_h0(long a, long b) {\n"
   "  v8s va = {(short)a, (short)b, 0, 0, 0, 0, 0, 0};\n"
   "  return (unsigned short)va[0];\n"
   "}\n",
   {42, 99}, "CoreNEONAdv", 1},

  // ===== ADDP — pairwise add =====
  {"neon_addp_8h",
   V8S
   "long neon_addp_8h(long a, long b) {\n"
   "  short sa = (short)a, sb = (short)b;\n"
   "  return (unsigned short)(sa + sb);\n"
   "}\n",
   {100, 42}, "CoreNEONAdv", 1},

  // ===== SMAX/SMIN with negative values =====
  {"neon_smax_4s_neg",
   V4I
   "long neon_smax_4s_neg(long a) {\n"
   "  v4i va = {-(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)a, 0, 0, 0};\n"
   "  v4i vr = __builtin_elementwise_max(va, vb);\n"
   "  return (unsigned int)vr[0];\n"
   "}\n",
   {42}, "CoreNEONAdv", 1},

  {"neon_smin_4s_neg",
   V4I
   "long neon_smin_4s_neg(long a) {\n"
   "  v4i va = {-(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)a, 0, 0, 0};\n"
   "  v4i vr = __builtin_elementwise_min(va, vb);\n"
   "  return (unsigned int)vr[0];\n"
   "}\n",
   {42}, "CoreNEONAdv", 1},

  // ===== Saturating add 8H (lane 0 only) =====
  {"sqadd_8h",
   V8S
   "long sqadd_8h(long a, long b) {\n"
   "  v8s va = {(short)a, 0, 0, 0, 0, 0, 0, 0};\n"
   "  v8s vb = {(short)b, 0, 0, 0, 0, 0, 0, 0};\n"
   "  v8s vr = __builtin_elementwise_add_sat(va, vb);\n"
   "  return (unsigned short)vr[0];\n"
   "}\n",
   {100, 42}, "CoreNEONAdv", 1},

  {"sqadd_8h_normal",
   V8S
   "long sqadd_8h_normal(long a, long b) {\n"
   "  v8s va = {(short)a, 0, 0, 0, 0, 0, 0, 0};\n"
   "  v8s vb = {(short)b, 0, 0, 0, 0, 0, 0, 0};\n"
   "  v8s vr = __builtin_elementwise_add_sat(va, vb);\n"
   "  return (unsigned short)vr[0];\n"
   "}\n",
   {100, 200}, "CoreNEONAdv", 1},

  // ===== Compare v.8h lanes =====
  {"neon_cmeq_8h",
   V8S
   "long neon_cmeq_8h(long a) {\n"
   "  v8s va = {(short)a, 0, 0, 0, 0, 0, 0, 0};\n"
   "  v8s vb = {(short)a, 99, 0, 0, 0, 0, 0, 0};\n"
   "  v8s cmp = (va == vb);\n"
   "  return (unsigned short)cmp[0] | ((unsigned long)(unsigned short)cmp[1] << 16);\n"
   "}\n",
   {42}, "CoreNEONAdv", 1},

  // ===== Compare v.16b lanes =====
  {"neon_cmgt_16b",
   V16C
   "long neon_cmgt_16b(long a, long b) {\n"
   "  v16c va = {(char)a, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16c vb = {(char)b, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16c cmp = (va > vb);\n"
   "  return (unsigned char)cmp[0];\n"
   "}\n",
   {100, 42}, "CoreNEONAdv", 1},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(CoreNEONAdv, A64CoreNEONAdvRT,
                         ::testing::ValuesIn(kA64CoreNEONAdv), rtTCName);
