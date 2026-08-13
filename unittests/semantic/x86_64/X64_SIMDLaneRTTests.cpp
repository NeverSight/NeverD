//===- X64_SIMDLaneRTTests.cpp - Per-lane SIMD correctness tests -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip tests targeting SIMD instructions that require per-lane semantics.
// Each test exercises an SSE intrinsic through the full pipeline:
//   clang → original .o → Unicorn → expected value
//   neverd lift → recompiled .o → Unicorn → actual value
//   assert expected == actual
//
// These are high-risk: the lifter previously used full-width (i128) operations
// instead of per-lane decomposition for many packed instructions.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SIMDLaneRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SIMDLaneRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

#define V4I  "typedef int v4i __attribute__((vector_size(16)));\n"
#define V8S  "typedef short v8s __attribute__((vector_size(16)));\n"
#define V16C "typedef char v16c __attribute__((vector_size(16)));\n"
#define V4UI "typedef unsigned int v4ui __attribute__((vector_size(16)));\n"
#define V16UC "typedef unsigned char v16uc __attribute__((vector_size(16)));\n"
#define V8US "typedef unsigned short v8us __attribute__((vector_size(16)));\n"
#define V4F  "typedef float v4f __attribute__((vector_size(16)));\n"

// ============================================================================
// PMULLD (packed dword multiply low) — carry must NOT cross lane boundaries
// ============================================================================
static const std::vector<RoundTripTC> kPMULLD = {
  {"pmulld_basic",
   V4I
   "long pmulld_basic(long a, long b) {\n"
   "  v4i va = {(int)a, (int)(a>>32), 1, 0};\n"
   "  v4i vb = {(int)b, (int)(b>>32), 1, 0};\n"
   "  v4i vr = va * vb;\n"
   "  return (long)(unsigned)vr[0] | ((long)(unsigned)vr[1] << 32);\n"
   "}\n",
   {(3ULL | (7ULL << 32)), (5ULL | (11ULL << 32))}, "PMULLD", 1, "-msse4.1 -mno-avx"},

  {"pmulld_overflow",
   V4I
   "long pmulld_overflow(long a, long b) {\n"
   "  v4i va = {(int)a, 0x7FFFFFFF, 0, 0};\n"
   "  v4i vb = {(int)b, 2, 0, 0};\n"
   "  v4i vr = va * vb;\n"
   "  return (long)(unsigned)vr[0] | ((long)(unsigned)vr[1] << 32);\n"
   "}\n",
   {100000, 200000}, "PMULLD", 1, "-msse4.1 -mno-avx"},

  {"pmullw_basic",
   V8S
   "long pmullw_basic(long a, long b) {\n"
   "  v8s va = {(short)a, (short)(a>>16), 0,0,0,0,0,0};\n"
   "  v8s vb = {(short)b, (short)(b>>16), 0,0,0,0,0,0};\n"
   "  v8s vr = va * vb;\n"
   "  return (long)(unsigned short)vr[0] | ((long)(unsigned short)vr[1] << 16);\n"
   "}\n",
   {(100 | (200ULL << 16)), (300 | (400ULL << 16))}, "PMULLD", 1, "-mno-avx"},
};

// ============================================================================
// PMINSD / PMAXSD / PMINUB / PMAXUB — per-lane min/max via vector ops
// Uses vector comparison + bitwise select pattern to force SIMD min/max.
// ============================================================================
static const std::vector<RoundTripTC> kPMINMAX = {
  {"pminsd_vec",
   V4I
   "long pminsd_vec(long a, long b) {\n"
   "  v4i va = {(int)a, (int)(a>>32), 0, 0};\n"
   "  v4i vb = {(int)b, (int)(b>>32), 0, 0};\n"
   "  v4i mask = va < vb;\n"
   "  v4i vr = (va & mask) | (vb & ~mask);\n"
   "  return (long)(unsigned)vr[0] | ((long)(unsigned)vr[1] << 32);\n"
   "}\n",
   {(50ULL | (100ULL << 32)), (30ULL | (200ULL << 32))}, "PMINMAX", 2, "-msse4.1 -mno-avx"},

  {"pmaxsd_vec",
   V4I
   "long pmaxsd_vec(long a, long b) {\n"
   "  v4i va = {(int)a, (int)(a>>32), 0, 0};\n"
   "  v4i vb = {(int)b, (int)(b>>32), 0, 0};\n"
   "  v4i mask = va > vb;\n"
   "  v4i vr = (va & mask) | (vb & ~mask);\n"
   "  return (long)(unsigned)vr[0] | ((long)(unsigned)vr[1] << 32);\n"
   "}\n",
   {(50ULL | (100ULL << 32)), (30ULL | (200ULL << 32))}, "PMINMAX", 2, "-msse4.1 -mno-avx"},

  {"pminsw_vec",
   V8S
   "long pminsw_vec(long a, long b) {\n"
   "  v8s va = {(short)a, (short)(a>>16), 0,0,0,0,0,0};\n"
   "  v8s vb = {(short)b, (short)(b>>16), 0,0,0,0,0,0};\n"
   "  v8s mask = va < vb;\n"
   "  v8s vr = (va & mask) | (vb & ~mask);\n"
   "  return (long)(unsigned short)vr[0] | ((long)(unsigned short)vr[1] << 16);\n"
   "}\n",
   {((uint64_t)(uint16_t)-100 | (1000ULL << 16)), (200ULL | ((uint64_t)(uint16_t)-500 << 16))}, "PMINMAX", 2, "-mno-avx"},

  {"pmaxsw_vec",
   V8S
   "long pmaxsw_vec(long a, long b) {\n"
   "  v8s va = {(short)a, (short)(a>>16), 0,0,0,0,0,0};\n"
   "  v8s vb = {(short)b, (short)(b>>16), 0,0,0,0,0,0};\n"
   "  v8s mask = va > vb;\n"
   "  v8s vr = (va & mask) | (vb & ~mask);\n"
   "  return (long)(unsigned short)vr[0] | ((long)(unsigned short)vr[1] << 16);\n"
   "}\n",
   {((uint64_t)(uint16_t)-100 | (1000ULL << 16)), (200ULL | ((uint64_t)(uint16_t)-500 << 16))}, "PMINMAX", 2, "-mno-avx"},
};

// ============================================================================
// MINPS/MAXPS/MINSS/MAXSS — float min/max (NOT just COPY!)
// ============================================================================
static const std::vector<RoundTripTC> kFPMINMAX = {
  {"minss_basic",
   "long minss_basic(long a, long b) {\n"
   "  float fa, fb;\n"
   "  unsigned ia = (unsigned)a, ib = (unsigned)b;\n"
   "  __builtin_memcpy(&fa, &ia, 4); __builtin_memcpy(&fb, &ib, 4);\n"
   "  float r = fa < fb ? fa : fb;\n"
   "  unsigned ir; __builtin_memcpy(&ir, &r, 4);\n"
   "  return (long)ir;\n"
   "}\n",
   {0x40A00000ULL, 0x40400000ULL}, "FPMINMAX", 1, "-mno-avx"},

  {"maxss_basic",
   "long maxss_basic(long a, long b) {\n"
   "  float fa, fb;\n"
   "  unsigned ia = (unsigned)a, ib = (unsigned)b;\n"
   "  __builtin_memcpy(&fa, &ia, 4); __builtin_memcpy(&fb, &ib, 4);\n"
   "  float r = fa > fb ? fa : fb;\n"
   "  unsigned ir; __builtin_memcpy(&ir, &r, 4);\n"
   "  return (long)ir;\n"
   "}\n",
   {0x40400000ULL, 0x40A00000ULL}, "FPMINMAX", 1, "-mno-avx"},

  {"minsd_basic",
   "long minsd_basic(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  double r = da < db ? da : db;\n"
   "  long rv; __builtin_memcpy(&rv, &r, 8);\n"
   "  return rv;\n"
   "}\n",
   {0x4014000000000000ULL, 0x4008000000000000ULL}, "FPMINMAX", 1, "-mno-avx"},

  {"maxsd_basic",
   "long maxsd_basic(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  double r = da > db ? da : db;\n"
   "  long rv; __builtin_memcpy(&rv, &r, 8);\n"
   "  return rv;\n"
   "}\n",
   {0x4008000000000000ULL, 0x4014000000000000ULL}, "FPMINMAX", 1, "-mno-avx"},
};

// ============================================================================
// Saturating arithmetic — via C scalar clamping (tests scalar lift path)
// ============================================================================
static const std::vector<RoundTripTC> kSatArith = {
  {"sat_add_s8",
   "long sat_add_s8(long a, long b) {\n"
   "  int sum = (int)(signed char)a + (int)(signed char)b;\n"
   "  if (sum > 127) sum = 127;\n"
   "  if (sum < -128) sum = -128;\n"
   "  return (long)(unsigned char)(signed char)sum;\n"
   "}\n",
   {100, 100}, "SatArith", 2},

  {"sat_add_u16",
   "long sat_add_u16(long a, long b) {\n"
   "  unsigned sum = (unsigned)(unsigned short)a + (unsigned)(unsigned short)b;\n"
   "  if (sum > 65535) sum = 65535;\n"
   "  return (long)(unsigned short)sum;\n"
   "}\n",
   {60000, 60000}, "SatArith", 2},

  {"sat_sub_u8",
   "long sat_sub_u8(long a, long b) {\n"
   "  int diff = (int)(unsigned char)a - (int)(unsigned char)b;\n"
   "  if (diff < 0) diff = 0;\n"
   "  return (long)(unsigned char)diff;\n"
   "}\n",
   {10, 200}, "SatArith", 2},
};

// ============================================================================
// PCMPEQD / PCMPGTD — per-lane comparison via vector comparison operators
// ============================================================================
static const std::vector<RoundTripTC> kPCMPLane = {
  {"pcmpeqd_vec",
   V4I
   "long pcmpeqd_vec(long a, long b) {\n"
   "  v4i va = {(int)a, 42, 0, 0};\n"
   "  v4i vb = {(int)b, 42, 0, 0};\n"
   "  v4i vr = (va == vb);\n"
   "  return (long)(unsigned)vr[0] | ((long)(unsigned)vr[1] << 32);\n"
   "}\n",
   {42, 99}, "PCMPLane", 2, "-msse2 -mno-avx"},

  {"pcmpgtd_vec",
   V4I
   "long pcmpgtd_vec(long a, long b) {\n"
   "  v4i va = {(int)a, 10, 0, 0};\n"
   "  v4i vb = {(int)b, 20, 0, 0};\n"
   "  v4i vr = (va > vb);\n"
   "  return (long)(unsigned)vr[0] | ((long)(unsigned)vr[1] << 32);\n"
   "}\n",
   {100, 50}, "PCMPLane", 2, "-msse2 -mno-avx"},
};

// ============================================================================
// PADDB/PADDW per-lane cross-validate (ensure no carry across lanes)
// ============================================================================
static const std::vector<RoundTripTC> kPADDLane = {
  {"paddb_no_carry_across",
   V16C
   "long paddb_no_carry_across(long a, long b) {\n"
   "  v16c va = {(char)0xFF, (char)0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16c vb = {(char)1, (char)0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16c vr = va + vb;\n"
   "  return (long)(unsigned char)vr[0] | ((long)(unsigned char)vr[1] << 8);\n"
   "}\n",
   {0, 0}, "PADDLane", 1, "-mno-avx"},

  {"paddw_no_carry_across",
   V8S
   "long paddw_no_carry_across(long a, long b) {\n"
   "  v8s va = {(short)0x7FFF, (short)0, 0,0,0,0,0,0};\n"
   "  v8s vb = {(short)0x7FFF, (short)0, 0,0,0,0,0,0};\n"
   "  v8s vr = va + vb;\n"
   "  return (long)(unsigned short)vr[0] | ((long)(unsigned short)vr[1] << 16);\n"
   "}\n",
   {0, 0}, "PADDLane", 1, "-mno-avx"},

  {"psubb_no_borrow_across",
   V16C
   "long psubb_no_borrow_across(long a, long b) {\n"
   "  v16c va = {(char)0, (char)5, 0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16c vb = {(char)1, (char)0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16c vr = va - vb;\n"
   "  return (long)(unsigned char)vr[0] | ((long)(unsigned char)vr[1] << 8);\n"
   "}\n",
   {0, 0}, "PADDLane", 1, "-mno-avx"},
};

// ============================================================================
// PAVGB — packed byte average: (a + b + 1) >> 1
// ============================================================================
static const std::vector<RoundTripTC> kPAVG = {
  {"pavgb_via_c",
   "long pavgb_via_c(long a, long b) {\n"
   "  unsigned char ua = (unsigned char)a, ub = (unsigned char)b;\n"
   "  unsigned char r = (unsigned char)(((unsigned)ua + (unsigned)ub + 1) >> 1);\n"
   "  return (long)r;\n"
   "}\n",
   {100, 200}, "PAVG", 1},
};

// ============================================================================
// PACKUSWB/PACKSSWB — pack with saturation
// ============================================================================
static const std::vector<RoundTripTC> kPACK = {
  {"packuswb_via_c",
   "long packuswb_via_c(long a, long b) {\n"
   "  short sa = (short)a;\n"
   "  unsigned char r = (sa > 255) ? 255 : (sa < 0) ? 0 : (unsigned char)sa;\n"
   "  return (long)r;\n"
   "}\n",
   {300, 0}, "PACK", 1},

  {"packsswb_via_c",
   "long packsswb_via_c(long a, long b) {\n"
   "  short sa = (short)a;\n"
   "  signed char r = (sa > 127) ? 127 : (sa < -128) ? -128 : (signed char)sa;\n"
   "  return (long)(unsigned char)r;\n"
   "}\n",
   {200, 0}, "PACK", 1},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(PMULLD, X64SIMDLaneRT, ::testing::ValuesIn(kPMULLD), rtTCName);
INSTANTIATE_TEST_SUITE_P(PMINMAX, X64SIMDLaneRT, ::testing::ValuesIn(kPMINMAX), rtTCName);
INSTANTIATE_TEST_SUITE_P(FPMINMAX, X64SIMDLaneRT, ::testing::ValuesIn(kFPMINMAX), rtTCName);
INSTANTIATE_TEST_SUITE_P(SatArith, X64SIMDLaneRT, ::testing::ValuesIn(kSatArith), rtTCName);
INSTANTIATE_TEST_SUITE_P(PCMPLane, X64SIMDLaneRT, ::testing::ValuesIn(kPCMPLane), rtTCName);
INSTANTIATE_TEST_SUITE_P(PADDLane, X64SIMDLaneRT, ::testing::ValuesIn(kPADDLane), rtTCName);
INSTANTIATE_TEST_SUITE_P(PAVG, X64SIMDLaneRT, ::testing::ValuesIn(kPAVG), rtTCName);
INSTANTIATE_TEST_SUITE_P(PACK, X64SIMDLaneRT, ::testing::ValuesIn(kPACK), rtTCName);
