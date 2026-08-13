//===- X64_SIMDPerLaneRTTests.cpp - SIMD per-lane roundtrip -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests x86_64 SIMD instructions that require per-lane decomposition through
// the full lift pipeline.  These are the instructions that historically had
// the most bugs (bugs #26-47 in the tracker).
//
// Uses vector_size attribute with -O1 to generate real SIMD instructions.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SIMDPerLaneRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SIMDPerLaneRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

#define V16C  "typedef char v16c __attribute__((vector_size(16)));\n"
#define V16UC "typedef unsigned char v16uc __attribute__((vector_size(16)));\n"
#define V8S   "typedef short v8s __attribute__((vector_size(16)));\n"
#define V8US  "typedef unsigned short v8us __attribute__((vector_size(16)));\n"
#define V4I   "typedef int v4i __attribute__((vector_size(16)));\n"
#define V4UI  "typedef unsigned int v4ui __attribute__((vector_size(16)));\n"
#define V2Q   "typedef long long v2q __attribute__((vector_size(16)));\n"
#define V4F   "typedef float v4f __attribute__((vector_size(16)));\n"
#define V2D   "typedef double v2d __attribute__((vector_size(16)));\n"

static const std::vector<RoundTripTC> kX64SIMDPerLane = {

  // ===== PCMPEQB — byte-lane equality compare, result is all-ones mask =====
  {"pcmpeqb_hit",
   V16C
   "long pcmpeqb_hit(long a, long b) {\n"
   "  v16c va = {(char)a,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16c vb = {(char)b,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16c cmp = (va == vb);\n"
   "  return (unsigned char)cmp[0];\n"
   "}\n",
   {42, 42}, "SIMDPerLane", 1},

  {"pcmpeqb_miss",
   V16C
   "long pcmpeqb_miss(long a, long b) {\n"
   "  v16c va = {(char)a,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16c vb = {(char)b,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16c cmp = (va == vb);\n"
   "  return (unsigned char)cmp[0];\n"
   "}\n",
   {42, 99}, "SIMDPerLane", 1},

  // ===== PCMPEQW — word-lane equality =====
  {"pcmpeqw",
   V8S
   "long pcmpeqw(long a, long b) {\n"
   "  v8s va = {(short)a,0,0,0,0,0,0,0};\n"
   "  v8s vb = {(short)b,0,0,0,0,0,0,0};\n"
   "  v8s cmp = (va == vb);\n"
   "  return (unsigned short)cmp[0];\n"
   "}\n",
   {1000, 1000}, "SIMDPerLane", 1},

  // ===== PCMPEQD — dword-lane equality =====
  {"pcmpeqd",
   V4I
   "long pcmpeqd(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i cmp = (va == vb);\n"
   "  return (unsigned int)cmp[0];\n"
   "}\n",
   {12345, 12345}, "SIMDPerLane", 1},

  // ===== PCMPGTB — byte-lane signed greater-than =====
  {"pcmpgtb",
   V16C
   "long pcmpgtb(long a, long b) {\n"
   "  v16c va = {(char)a,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16c vb = {(char)b,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16c cmp = (va > vb);\n"
   "  return (unsigned char)cmp[0];\n"
   "}\n",
   {50, 20}, "SIMDPerLane", 1},

  // ===== PCMPGTD — dword-lane signed greater-than =====
  {"pcmpgtd",
   V4I
   "long pcmpgtd(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i cmp = (va > vb);\n"
   "  return (unsigned int)cmp[0];\n"
   "}\n",
   {100, 50}, "SIMDPerLane", 1},

  // ===== PMINUB — unsigned byte min =====
  {"pminub",
   V16UC
   "long pminub(long a, long b) {\n"
   "  v16uc va = {(unsigned char)a, (unsigned char)(a>>8), 0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16uc vb = {(unsigned char)b, (unsigned char)(b>>8), 0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16uc vr = __builtin_elementwise_min(va, vb);\n"
   "  return (unsigned char)vr[0] | ((unsigned long)(unsigned char)vr[1] << 8);\n"
   "}\n",
   {0xFF03, 0x0105}, "SIMDPerLane", 1, "-msse4.1"},

  // ===== PMAXUB — unsigned byte max =====
  {"pmaxub",
   V16UC
   "long pmaxub(long a, long b) {\n"
   "  v16uc va = {(unsigned char)a, (unsigned char)(a>>8), 0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16uc vb = {(unsigned char)b, (unsigned char)(b>>8), 0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16uc vr = __builtin_elementwise_max(va, vb);\n"
   "  return (unsigned char)vr[0] | ((unsigned long)(unsigned char)vr[1] << 8);\n"
   "}\n",
   {0xFF03, 0x0105}, "SIMDPerLane", 1, "-msse4.1"},

  // ===== PMINSW — signed word min =====
  {"pminsw",
   V8S
   "long pminsw(long a, long b) {\n"
   "  v8s va = {(short)a, 0,0,0,0,0,0,0};\n"
   "  v8s vb = {(short)b, 0,0,0,0,0,0,0};\n"
   "  v8s vr = __builtin_elementwise_min(va, vb);\n"
   "  return (unsigned short)vr[0];\n"
   "}\n",
   {(uint64_t)(int16_t)-100, (uint64_t)(int16_t)50}, "SIMDPerLane", 1},

  // ===== PMAXSW — signed word max =====
  {"pmaxsw",
   V8S
   "long pmaxsw(long a, long b) {\n"
   "  v8s va = {(short)a, 0,0,0,0,0,0,0};\n"
   "  v8s vb = {(short)b, 0,0,0,0,0,0,0};\n"
   "  v8s vr = __builtin_elementwise_max(va, vb);\n"
   "  return (unsigned short)vr[0];\n"
   "}\n",
   {(uint64_t)(int16_t)-100, (uint64_t)(int16_t)50}, "SIMDPerLane", 1},

  // ===== PMINSD — SSE4.1 signed dword min =====
  {"pminsd",
   V4I
   "long pminsd(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i vr = __builtin_elementwise_min(va, vb);\n"
   "  return (unsigned int)vr[0];\n"
   "}\n",
   {(uint64_t)(int32_t)-500, (uint64_t)(int32_t)200}, "SIMDPerLane", 1, "-msse4.1"},

  // ===== PMAXSD — SSE4.1 signed dword max =====
  {"pmaxsd",
   V4I
   "long pmaxsd(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i vr = __builtin_elementwise_max(va, vb);\n"
   "  return (unsigned int)vr[0];\n"
   "}\n",
   {(uint64_t)(int32_t)-500, (uint64_t)(int32_t)200}, "SIMDPerLane", 1, "-msse4.1"},

  // ===== PMINUD — SSE4.1 unsigned dword min =====
  {"pminud",
   V4UI
   "long pminud(long a, long b) {\n"
   "  v4ui va = {(unsigned int)a, 0, 0, 0};\n"
   "  v4ui vb = {(unsigned int)b, 0, 0, 0};\n"
   "  v4ui vr = __builtin_elementwise_min(va, vb);\n"
   "  return (unsigned int)vr[0];\n"
   "}\n",
   {0xFFFFFFF0ULL, 0x00000010ULL}, "SIMDPerLane", 1, "-msse4.1"},

  // ===== PMULLD — dword-lane multiply low (bug #41 regression) =====
  {"pmulld_regress",
   V4I
   "long pmulld_regress(long a, long b) {\n"
   "  v4i va = {(int)a, (int)(a>>32), 0, 0};\n"
   "  v4i vb = {(int)b, (int)(b>>32), 0, 0};\n"
   "  v4i vr = va * vb;\n"
   "  return (unsigned int)vr[0] | ((unsigned long)(unsigned int)vr[1] << 32);\n"
   "}\n",
   {0x0000000700000003ULL, 0x0000000B00000005ULL}, "SIMDPerLane", 1, "-msse4.1"},

  // ===== PMULLW — word-lane multiply (bug #41 regression) =====
  {"pmullw_regress",
   V8S
   "long pmullw_regress(long a, long b) {\n"
   "  v8s va = {(short)a, (short)(a>>16), 0,0,0,0,0,0};\n"
   "  v8s vb = {(short)b, (short)(b>>16), 0,0,0,0,0,0};\n"
   "  v8s vr = va * vb;\n"
   "  return (unsigned short)vr[0] | ((unsigned long)(unsigned short)vr[1] << 16);\n"
   "}\n",
   {(7 | (11ULL << 16)), (3 | (5ULL << 16))}, "SIMDPerLane", 1},

  // ===== Packed float add (ADDPS) =====
  {"addps_4lane",
   V4F
   "long addps_4lane(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  v4f va = {fa, 1.0f, 0.0f, 0.0f};\n"
   "  v4f vb = {fb, 2.0f, 0.0f, 0.0f};\n"
   "  v4f vr = va + vb;\n"
   "  float r = vr[0]; long ret; __builtin_memcpy(&ret, &r, 4);\n"
   "  return ret;\n"
   "}\n",
   {0x40A00000ULL, 0x40400000ULL}, "SIMDPerLane", 1},

  // ===== Packed float mul (MULPS) =====
  {"mulps_4lane",
   V4F
   "long mulps_4lane(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  v4f va = {fa, 0.0f, 0.0f, 0.0f};\n"
   "  v4f vb = {fb, 0.0f, 0.0f, 0.0f};\n"
   "  v4f vr = va * vb;\n"
   "  float r = vr[0]; long ret; __builtin_memcpy(&ret, &r, 4);\n"
   "  return ret;\n"
   "}\n",
   {0x40A00000ULL, 0x40400000ULL}, "SIMDPerLane", 1},

  // ===== Packed float compare (CMPPS) =====
  {"cmpps_eq",
   V4F V4I
   "long cmpps_eq(long a) {\n"
   "  float fa; __builtin_memcpy(&fa, &a, 4);\n"
   "  v4f va = {fa, 0.0f, 0.0f, 0.0f};\n"
   "  v4f vb = {fa, 1.0f, 0.0f, 0.0f};\n"
   "  v4i cmp = __builtin_convertvector(va == vb, v4i);\n"
   "  return (unsigned int)cmp[0];\n"
   "}\n",
   {0x40A00000ULL}, "SIMDPerLane", 1},

  // ===== Packed float min (MINPS) =====
  {"minps",
   V4F
   "long minps(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  v4f va = {fa, 0.0f, 0.0f, 0.0f};\n"
   "  v4f vb = {fb, 0.0f, 0.0f, 0.0f};\n"
   "  v4f vr = __builtin_elementwise_min(va, vb);\n"
   "  float r = vr[0]; long ret; __builtin_memcpy(&ret, &r, 4);\n"
   "  return ret;\n"
   "}\n",
   {0x40A00000ULL, 0x40400000ULL}, "SIMDPerLane", 1},

  // ===== Packed float max (MAXPS) =====
  {"maxps",
   V4F
   "long maxps(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &a, 4); __builtin_memcpy(&fb, &b, 4);\n"
   "  v4f va = {fa, 0.0f, 0.0f, 0.0f};\n"
   "  v4f vb = {fb, 0.0f, 0.0f, 0.0f};\n"
   "  v4f vr = __builtin_elementwise_max(va, vb);\n"
   "  float r = vr[0]; long ret; __builtin_memcpy(&ret, &r, 4);\n"
   "  return ret;\n"
   "}\n",
   {0x40A00000ULL, 0x40400000ULL}, "SIMDPerLane", 1},

  // ===== Shuffle: PSHUFD-like (compile with builtin) =====
  {"shuffle_dword",
   V4I
   "long shuffle_dword(long a, long b) {\n"
   "  v4i va = {(int)a, (int)(a>>32), (int)b, (int)(b>>32)};\n"
   "  v4i vr = __builtin_shufflevector(va, va, 2, 0, 3, 1);\n"
   "  return (unsigned int)vr[0] | ((unsigned long)(unsigned int)vr[1] << 32);\n"
   "}\n",
   {0x0000000200000001ULL, 0x0000000400000003ULL}, "SIMDPerLane", 1},

  // ===== Shuffle: PUNPCKLBW-like (interleave low bytes) =====
  {"unpack_low_bytes",
   V16C
   "long unpack_low_bytes(long a, long b) {\n"
   "  v16c va = {(char)a, (char)(a>>8), 0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16c vb = {(char)b, (char)(b>>8), 0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16c vr = __builtin_shufflevector(va, vb, 0, 16, 1, 17, 2,18,3,19,4,20,5,21,6,22,7,23);\n"
   "  return (unsigned char)vr[0] | ((unsigned long)(unsigned char)vr[1] << 8)\n"
   "       | ((unsigned long)(unsigned char)vr[2] << 16) | ((unsigned long)(unsigned char)vr[3] << 24);\n"
   "}\n",
   {0x0201, 0x0403}, "SIMDPerLane", 1},

  // ===== PUNPCKLDQ — interleave low dwords =====
  {"unpack_low_dwords",
   V4I
   "long unpack_low_dwords(long a, long b) {\n"
   "  v4i va = {(int)a, (int)(a>>32), 0, 0};\n"
   "  v4i vb = {(int)b, (int)(b>>32), 0, 0};\n"
   "  v4i vr = __builtin_shufflevector(va, vb, 0, 4, 1, 5);\n"
   "  return (unsigned int)vr[0] | ((unsigned long)(unsigned int)vr[1] << 32);\n"
   "}\n",
   {0x0000000200000001ULL, 0x0000000400000003ULL}, "SIMDPerLane", 1},

  // ===== CVTDQ2PS — int to float conversion =====
  {"cvtdq2ps_simple",
   V4I V4F
   "long cvtdq2ps_simple(long a) {\n"
   "  v4i vi = {(int)a, 0, 0, 0};\n"
   "  v4f vf = __builtin_convertvector(vi, v4f);\n"
   "  float r = vf[0]; long ret; __builtin_memcpy(&ret, &r, 4);\n"
   "  return ret;\n"
   "}\n",
   {42}, "SIMDPerLane", 1},

  // ===== CVTPS2DQ — float to int conversion =====
  {"cvtps2dq_simple",
   V4I V4F
   "long cvtps2dq_simple(long a) {\n"
   "  float fa; __builtin_memcpy(&fa, &a, 4);\n"
   "  v4f vf = {fa, 0.0f, 0.0f, 0.0f};\n"
   "  v4i vi = __builtin_convertvector(vf, v4i);\n"
   "  return (unsigned int)vi[0];\n"
   "}\n",
   {0x42280000ULL}, "SIMDPerLane", 1},

  // ===== Multi-lane PADDD with overflow isolation (regression for #26) =====
  {"paddd_4lane_overflow",
   V4I
   "long paddd_4lane_overflow(long a, long b) {\n"
   "  v4i va = {(int)0xFFFFFFFF, (int)0x7FFFFFFF, (int)a, (int)b};\n"
   "  v4i vb = {1, 1, (int)b, (int)a};\n"
   "  v4i vr = va + vb;\n"
   "  return (unsigned int)vr[0] | ((unsigned long)(unsigned int)vr[2] << 32);\n"
   "}\n",
   {100, 200}, "SIMDPerLane", 1},

  // ===== PSUBD 4-lane with borrow isolation =====
  {"psubd_4lane",
   V4I
   "long psubd_4lane(long a, long b) {\n"
   "  v4i va = {0, 100, (int)a, (int)b};\n"
   "  v4i vb = {1, 50, (int)b, (int)a};\n"
   "  v4i vr = va - vb;\n"
   "  return (unsigned int)vr[0] | ((unsigned long)(unsigned int)vr[1] << 32);\n"
   "}\n",
   {300, 100}, "SIMDPerLane", 1},

  // ===== PAND/POR/PXOR per-lane (should be whole-width but test anyway) =====
  {"pand_vec",
   V4I
   "long pand_vec(long a, long b) {\n"
   "  v4i va = {(int)a, (int)(a>>32), 0, 0};\n"
   "  v4i vb = {(int)b, (int)(b>>32), 0, 0};\n"
   "  v4i vr = va & vb;\n"
   "  return (unsigned int)vr[0] | ((unsigned long)(unsigned int)vr[1] << 32);\n"
   "}\n",
   {0xFFFF0000AAAA5555ULL, 0xFF00FF00FF00FF00ULL}, "SIMDPerLane", 1},

  {"por_vec",
   V4I
   "long por_vec(long a, long b) {\n"
   "  v4i va = {(int)a, (int)(a>>32), 0, 0};\n"
   "  v4i vb = {(int)b, (int)(b>>32), 0, 0};\n"
   "  v4i vr = va | vb;\n"
   "  return (unsigned int)vr[0] | ((unsigned long)(unsigned int)vr[1] << 32);\n"
   "}\n",
   {0xFFFF0000AAAA5555ULL, 0xFF00FF00FF00FF00ULL}, "SIMDPerLane", 1},

  {"pxor_vec",
   V4I
   "long pxor_vec(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i vr = va ^ vb;\n"
   "  return (unsigned int)vr[0];\n"
   "}\n",
   {0xDEADBEEF, 0xCAFEBABE}, "SIMDPerLane", 1},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(SIMDPerLane, X64SIMDPerLaneRT,
                         ::testing::ValuesIn(kX64SIMDPerLane), rtTCName);
