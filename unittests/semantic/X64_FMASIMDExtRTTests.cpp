//===- X64_FMASIMDExtRTTests.cpp - FMA/SIMD widening roundtrip ---*- C++ -*-===//
//
// Tests x86_64 FMA and SIMD widening/horizontal/complex operations through
// the full lift → recompile → Unicorn pipeline.
//
// High-risk targets: FMA 3-operand ops, PMADDWD/PMULHW/PHADDD cross-lane ops.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64FMART : public SemanticRoundTripFixture,
                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64FMART, Verify) { roundTripX64(GetParam()); }

// clang-format off

// ============================================================================
// FMA scalar double: VFMADD, VFMSUB, VFNMADD, VFNMSUB
// ============================================================================
static const std::vector<RoundTripTC> kX64FMA = {
  // Use mul+add C expressions (generates mulsd+addsd, no VEX FMA needed)
  {"fma_muladd_sd",
   "long fma_muladd_sd(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da,&a,8); __builtin_memcpy(&db,&b,8);\n"
   "  double r = da * db + 1.0;\n"
   "  long rv; __builtin_memcpy(&rv,&r,8); return rv;\n"
   "}\n",
   {0x4014000000000000ULL, 0x4008000000000000ULL}, "FMA", 1},

  {"fma_mulsub_sd",
   "long fma_mulsub_sd(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da,&a,8); __builtin_memcpy(&db,&b,8);\n"
   "  double r = da * db - 1.0;\n"
   "  long rv; __builtin_memcpy(&rv,&r,8); return rv;\n"
   "}\n",
   {0x4000000000000000ULL, 0x4008000000000000ULL}, "FMA", 1},

  {"fma_negmuladd_sd",
   "long fma_negmuladd_sd(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da,&a,8); __builtin_memcpy(&db,&b,8);\n"
   "  double r = -(da * db) + 1.0;\n"
   "  long rv; __builtin_memcpy(&rv,&r,8); return rv;\n"
   "}\n",
   {0x4000000000000000ULL, 0x4008000000000000ULL}, "FMA", 1},

  {"fma_negmulsub_sd",
   "long fma_negmulsub_sd(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da,&a,8); __builtin_memcpy(&db,&b,8);\n"
   "  double r = -(da * db) - 1.0;\n"
   "  long rv; __builtin_memcpy(&rv,&r,8); return rv;\n"
   "}\n",
   {0x4000000000000000ULL, 0x4008000000000000ULL}, "FMA", 1},

  {"fma_muladd_int_via_fp",
   "long fma_muladd_int_via_fp(long a, long b) {\n"
   "  double da = (double)(long)a, db = (double)(long)b;\n"
   "  double r = da * db + 1.0;\n"
   "  return (long)r;\n"
   "}\n",
   {5, 3}, "FMA", 1},

  {"fma_mulsub_int_via_fp",
   "long fma_mulsub_int_via_fp(long a, long b) {\n"
   "  double da = (double)(long)a, db = (double)(long)b;\n"
   "  double r = da * db - 10.0;\n"
   "  return (long)r;\n"
   "}\n",
   {7, 3}, "FMA", 1},

  // Packed mul+add via vector types (SSE, no VEX)
  {"fma_muladd_pd",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long fma_muladd_pd(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da,&a,8); __builtin_memcpy(&db,&b,8);\n"
   "  v2d va = {da, 1.0}, vb = {db, 2.0}, vc = {1.0, 1.0};\n"
   "  v2d vr = va * vb + vc;\n"
   "  double r0 = vr[0];\n"
   "  long rv; __builtin_memcpy(&rv,&r0,8); return rv;\n"
   "}\n",
   {0x4000000000000000ULL, 0x4008000000000000ULL}, "FMA", 1},

  {"fma_muladd_pd_lane",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long fma_muladd_pd_lane(long a, long b) {\n"
   "  double da = (double)(long)a, db = (double)(long)b;\n"
   "  v2d va = {da, 1.0}, vb = {db, 2.0}, vc = {1.0, 10.0};\n"
   "  v2d vr = va * vb + vc;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {3, 4}, "FMA", 1},
};

// ============================================================================
// SIMD widening multiply: PMADDWD (word mul + pair-add → dword)
// ============================================================================
static const std::vector<RoundTripTC> kX64SIMDWiden = {
  {"pmaddwd_simple",
   "typedef short v8s __attribute__((vector_size(16)));\n"
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long pmaddwd_simple(long a, long b) {\n"
   "  v8s va = {(short)a, (short)(a>>16), 0,0,0,0,0,0};\n"
   "  v8s vb = {(short)b, (short)(b>>16), 0,0,0,0,0,0};\n"
   "  v4i vr = __builtin_ia32_pmaddwd128(va, vb);\n"
   "  return (long)vr[0];\n"
   "}\n",
   {(3 | (4ULL << 16)), (5 | (6ULL << 16))}, "SIMDWiden", 1, "-mno-avx"},

  {"pmulhw_simple",
   "typedef short v8s __attribute__((vector_size(16)));\n"
   "long pmulhw_simple(long a, long b) {\n"
   "  v8s va = {(short)a, 0,0,0,0,0,0,0};\n"
   "  v8s vb = {(short)b, 0,0,0,0,0,0,0};\n"
   "  v8s vr = __builtin_ia32_pmulhw128(va, vb);\n"
   "  return (long)(unsigned short)vr[0];\n"
   "}\n",
   {1000, 2000}, "SIMDWiden", 1, "-mno-avx"},

  {"pmulhuw_simple",
   "typedef short v8s __attribute__((vector_size(16)));\n"
   "long pmulhuw_simple(long a, long b) {\n"
   "  v8s va = {(short)a, 0,0,0,0,0,0,0};\n"
   "  v8s vb = {(short)b, 0,0,0,0,0,0,0};\n"
   "  v8s vr = __builtin_ia32_pmulhuw128(va, vb);\n"
   "  return (long)(unsigned short)vr[0];\n"
   "}\n",
   {30000, 40000}, "SIMDWiden", 1, "-mno-avx"},

  // PMULDQ: signed dword → signed qword multiply (SSE4.1)
  {"pmuldq_simple",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "typedef long long v2q __attribute__((vector_size(16)));\n"
   "long pmuldq_simple(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v2q vr = __builtin_ia32_pmuldq128(va, vb);\n"
   "  return (long)vr[0];\n"
   "}\n",
   {100000, 200000}, "SIMDWiden", 1, "-msse4.1 -mno-avx"},
};

// ============================================================================
// SIMD horizontal: PHADDD, PHSUBW, DPPS
// ============================================================================
static const std::vector<RoundTripTC> kX64SIMDHoriz = {
  {"phaddd_simple",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long phaddd_simple(long a, long b) {\n"
   "  v4i va = {(int)a, (int)(a>>32), 0, 0};\n"
   "  v4i vb = {(int)b, (int)(b>>32), 0, 0};\n"
   "  v4i vr = __builtin_ia32_phaddd128(va, vb);\n"
   "  return (long)(unsigned)vr[0];\n"
   "}\n",
   {(3ULL | (7ULL << 32)), (11ULL | (13ULL << 32))}, "SIMDHoriz", 1, "-mssse3 -mno-avx"},

  {"phaddw_simple",
   "typedef short v8s __attribute__((vector_size(16)));\n"
   "long phaddw_simple(long a, long b) {\n"
   "  v8s va = {(short)a, (short)(a>>16), 0,0,0,0,0,0};\n"
   "  v8s vb = {(short)b, (short)(b>>16), 0,0,0,0,0,0};\n"
   "  v8s vr = __builtin_ia32_phaddw128(va, vb);\n"
   "  return (long)(unsigned short)vr[0];\n"
   "}\n",
   {(10 | (20ULL << 16)), (30 | (40ULL << 16))}, "SIMDHoriz", 1, "-mssse3 -mno-avx"},

  {"phsubd_simple",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long phsubd_simple(long a, long b) {\n"
   "  v4i va = {(int)a, (int)(a>>32), 0, 0};\n"
   "  v4i vb = {(int)b, (int)(b>>32), 0, 0};\n"
   "  v4i vr = __builtin_ia32_phsubd128(va, vb);\n"
   "  return (long)(unsigned)vr[0];\n"
   "}\n",
   {(100ULL | (30ULL << 32)), (50ULL | (20ULL << 32))}, "SIMDHoriz", 1, "-mssse3 -mno-avx"},
};

// ============================================================================
// SIMD saturating arithmetic: PADDSB, PADDSW, PSUBUSB, PADDUSW
// ============================================================================
static const std::vector<RoundTripTC> kX64SIMDSat = {
  {"paddsb_sat",
   "typedef char v16c __attribute__((vector_size(16)));\n"
   "long paddsb_sat(long a, long b) {\n"
   "  v16c va = {(char)a, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16c vb = {(char)b, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16c vr = __builtin_elementwise_add_sat(va, vb);\n"
   "  return (long)(signed char)vr[0];\n"
   "}\n",
   {100, 100}, "SIMDSat", 1, "-mno-avx"},

  {"paddsw_sat",
   "typedef short v8s __attribute__((vector_size(16)));\n"
   "long paddsw_sat(long a, long b) {\n"
   "  v8s va = {(short)a, 0,0,0,0,0,0,0};\n"
   "  v8s vb = {(short)b, 0,0,0,0,0,0,0};\n"
   "  v8s vr = __builtin_elementwise_add_sat(va, vb);\n"
   "  return (long)(short)vr[0];\n"
   "}\n",
   {30000, 30000}, "SIMDSat", 1, "-mno-avx"},

  {"psubusb_sat",
   "typedef unsigned char v16uc __attribute__((vector_size(16)));\n"
   "long psubusb_sat(long a, long b) {\n"
   "  v16uc va = {(unsigned char)a, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16uc vb = {(unsigned char)b, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16uc vr = __builtin_elementwise_sub_sat(va, vb);\n"
   "  return (long)vr[0];\n"
   "}\n",
   {10, 200}, "SIMDSat", 1, "-mno-avx"},

  {"paddusw_sat",
   "typedef unsigned short v8us __attribute__((vector_size(16)));\n"
   "long paddusw_sat(long a, long b) {\n"
   "  v8us va = {(unsigned short)a, 0,0,0,0,0,0,0};\n"
   "  v8us vb = {(unsigned short)b, 0,0,0,0,0,0,0};\n"
   "  v8us vr = __builtin_elementwise_add_sat(va, vb);\n"
   "  return (long)vr[0];\n"
   "}\n",
   {60000, 60000}, "SIMDSat", 1, "-mno-avx"},
};

// ============================================================================
// SIMD shuffle/permute: PSHUFB, PALIGNR, PSADBW
// ============================================================================
static const std::vector<RoundTripTC> kX64SIMDShuffle = {
  {"pshufb_zero_extend",
   "typedef char v16c __attribute__((vector_size(16)));\n"
   "long pshufb_zero_extend(long a, long b) {\n"
   "  v16c data = {(char)a, (char)(a>>8), (char)(a>>16), (char)(a>>24),\n"
   "               0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16c mask = {0, 0, 1, 1, 2, 2, 3, 3, -1,-1,-1,-1,-1,-1,-1,-1};\n"
   "  v16c vr = __builtin_ia32_pshufb128(data, mask);\n"
   "  return (unsigned char)vr[0] | ((unsigned char)vr[2] << 8) |\n"
   "         ((unsigned char)vr[4] << 16) | ((unsigned long)(unsigned char)vr[6] << 24);\n"
   "}\n",
   {0x04030201ULL, 0}, "SIMDShuffle", 1, "-mssse3 -mno-avx"},

  {"psadbw_simple",
   "typedef char v16c __attribute__((vector_size(16)));\n"
   "typedef long long v2q __attribute__((vector_size(16)));\n"
   "long psadbw_simple(long a, long b) {\n"
   "  v16c va = {(char)a, (char)(a>>8), (char)(a>>16), (char)(a>>24),\n"
   "             0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16c vb = {(char)b, (char)(b>>8), (char)(b>>16), (char)(b>>24),\n"
   "             0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v2q vr = __builtin_ia32_psadbw128(va, vb);\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0x0A0A0A0AULL, 0x01010101ULL}, "SIMDShuffle", 1, "-mno-avx"},
};

// ============================================================================
// x86 IDIV/DIV edge cases (ensure no __divti3 library calls)
// ============================================================================
static const std::vector<RoundTripTC> kX64DivEdge = {
  {"idiv_neg_roundtrip",
   "long idiv_neg_roundtrip(long a, long b) {\n"
   "  if (b == 0) return 0;\n"
   "  return a / b;\n"
   "}\n",
   {(uint64_t)(int64_t)-100, 7}, "DivEdge", 1},

  {"udiv_large_roundtrip",
   "long udiv_large_roundtrip(long a, long b) {\n"
   "  unsigned long ua = (unsigned long)a;\n"
   "  unsigned long ub = (unsigned long)b;\n"
   "  if (ub == 0) return 0;\n"
   "  return (long)(ua / ub);\n"
   "}\n",
   {0xFFFFFFFF00000000ULL, 0x100}, "DivEdge", 1},

  {"imod_roundtrip",
   "long imod_roundtrip(long a, long b) {\n"
   "  if (b == 0) return 0;\n"
   "  return a % b;\n"
   "}\n",
   {(uint64_t)(int64_t)-100, 7}, "DivEdge", 1},

  {"div_by_const",
   "long div_by_const(long a, long b) {\n"
   "  return a / 7;\n"
   "}\n",
   {100, 0}, "DivEdge", 1},
};

// ============================================================================
// x86 sub-register aliasing stress (optimizer regression guard)
// ============================================================================
static const std::vector<RoundTripTC> kX64SubregStress = {
  {"subreg_al_write_rax_read",
   "long subreg_al_write_rax_read(long a, long b) {\n"
   "  unsigned char lo = (unsigned char)a;\n"
   "  lo += (unsigned char)b;\n"
   "  return (long)lo;\n"
   "}\n",
   {250, 10}, "SubregStress", 1},

  {"subreg_ax_write_eax_read",
   "long subreg_ax_write_eax_read(long a, long b) {\n"
   "  unsigned short lo = (unsigned short)a;\n"
   "  lo *= (unsigned short)b;\n"
   "  return (long)lo;\n"
   "}\n",
   {200, 300}, "SubregStress", 1},

  {"subreg_eax_write_rax_read",
   "long subreg_eax_write_rax_read(long a, long b) {\n"
   "  unsigned int lo = (unsigned int)a;\n"
   "  lo ^= (unsigned int)b;\n"
   "  return (long)lo;\n"
   "}\n",
   {0xDEADBEEFULL, 0xCAFEBABEULL}, "SubregStress", 1},

  {"subreg_chain_64_32_16_8",
   "long subreg_chain_64_32_16_8(long a, long b) {\n"
   "  unsigned int v32 = (unsigned int)a + (unsigned int)b;\n"
   "  unsigned short v16 = (unsigned short)v32;\n"
   "  unsigned char v8 = (unsigned char)v16;\n"
   "  return (long)v8 + (long)v16 + (long)v32;\n"
   "}\n",
   {0x12345678ULL, 0xABCD0000ULL}, "SubregStress", 1},

  {"subreg_signext_chain",
   "long subreg_signext_chain(long a, long b) {\n"
   "  signed char s8 = (signed char)a;\n"
   "  short s16 = (short)s8;\n"
   "  int s32 = (int)s16;\n"
   "  long s64 = (long)s32;\n"
   "  return s64;\n"
   "}\n",
   {0xFF, 0}, "SubregStress", 1},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(FMA, X64FMART, ::testing::ValuesIn(kX64FMA), rtTCName);
INSTANTIATE_TEST_SUITE_P(SIMDWiden, X64FMART, ::testing::ValuesIn(kX64SIMDWiden), rtTCName);
INSTANTIATE_TEST_SUITE_P(SIMDHoriz, X64FMART, ::testing::ValuesIn(kX64SIMDHoriz), rtTCName);
INSTANTIATE_TEST_SUITE_P(SIMDSat, X64FMART, ::testing::ValuesIn(kX64SIMDSat), rtTCName);
INSTANTIATE_TEST_SUITE_P(SIMDShuffle, X64FMART, ::testing::ValuesIn(kX64SIMDShuffle), rtTCName);
INSTANTIATE_TEST_SUITE_P(DivEdge, X64FMART, ::testing::ValuesIn(kX64DivEdge), rtTCName);
INSTANTIATE_TEST_SUITE_P(SubregStress, X64FMART, ::testing::ValuesIn(kX64SubregStress), rtTCName);
