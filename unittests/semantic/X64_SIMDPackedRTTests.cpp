//===- X64_SIMDPackedRTTests.cpp - SSE packed op roundtrip ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests x86_64 SSE/SSE2 packed integer arithmetic through lift pipeline.
// Specifically targets per-lane correctness — past bugs (#26 PADDD, #27
// PCMPEQ) stem from full-width i128 operations instead of per-lane decomp.
//
// Uses vector_size attribute to generate real SIMD instructions.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SIMDPackRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SIMDPackRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kX64SIMDPack = {
  // ========== PADDB - byte-lane add with carry isolation ==========
  {"paddb_no_carry",
   "typedef char v16c __attribute__((vector_size(16)));\n"
   "long paddb_no_carry(long a, long b) {\n"
   "  v16c va = {(char)a, (char)(a>>8), 0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16c vb = {(char)b, (char)(b>>8), 0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16c vr = va + vb;\n"
   "  return (unsigned char)vr[0] | ((unsigned char)vr[1] << 8);\n"
   "}\n",
   {0x7F01, 0x0102}, "SIMDPacked", /*OptLevel=*/1},

  // ========== PADDW - word-lane add ==========
  {"paddw_simple",
   "typedef short v8s __attribute__((vector_size(16)));\n"
   "long paddw_simple(long a, long b) {\n"
   "  v8s va = {(short)a, (short)(a>>16), 0,0,0,0,0,0};\n"
   "  v8s vb = {(short)b, (short)(b>>16), 0,0,0,0,0,0};\n"
   "  v8s vr = va + vb;\n"
   "  return (unsigned short)vr[0] | ((unsigned long)(unsigned short)vr[1] << 16);\n"
   "}\n",
   {(3 | (4ULL << 16)), (5 | (6ULL << 16))}, "SIMDPacked", 1},

  // ========== PADDD - dword-lane add (regression for #26) ==========
  {"paddd_overflow",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long paddd_overflow(long a, long b) {\n"
   "  v4i va = {(int)a, (int)(a>>32), 0, 0};\n"
   "  v4i vb = {(int)b, (int)(b>>32), 0, 0};\n"
   "  v4i vr = va + vb;\n"
   "  return (unsigned)vr[0] | ((unsigned long)(unsigned)vr[1] << 32);\n"
   "}\n",
   {0xFFFFFFFF00000001ULL, 0x0000000100000002ULL}, "SIMDPacked", 1},

  // ========== PSUBB - byte-lane sub ==========
  {"psubb",
   "typedef char v16c __attribute__((vector_size(16)));\n"
   "long psubb(long a, long b) {\n"
   "  v16c va = {(char)a, (char)(a>>8), 0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16c vb = {(char)b, (char)(b>>8), 0,0,0,0,0,0,0,0,0,0,0,0,0,0};\n"
   "  v16c vr = va - vb;\n"
   "  return (unsigned char)vr[0] | ((unsigned char)vr[1] << 8);\n"
   "}\n",
   {0xFF05, 0x0102}, "SIMDPacked", 1},

  // ========== PSUBD - dword-lane sub ==========
  {"psubd",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long psubd(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i vr = va - vb;\n"
   "  return (long)(unsigned)vr[0];\n"
   "}\n",
   {100, 42}, "SIMDPacked", 1},

  // ========== PMULLW - word multiply low ==========
  {"pmullw",
   "typedef short v8s __attribute__((vector_size(16)));\n"
   "long pmullw(long a, long b) {\n"
   "  v8s va = {(short)a, 0, 0, 0, 0, 0, 0, 0};\n"
   "  v8s vb = {(short)b, 0, 0, 0, 0, 0, 0, 0};\n"
   "  v8s vr = va * vb;\n"
   "  return (unsigned short)vr[0];\n"
   "}\n",
   {100, 200}, "SIMDPacked", 1},

  // ========== PAND/POR/PXOR - bitwise lane-independent ==========
  {"pand_128",
   "typedef long long v2q __attribute__((vector_size(16)));\n"
   "long pand_128(long a, long b) {\n"
   "  v2q va = {a, 0};\n"
   "  v2q vb = {b, 0};\n"
   "  v2q vr = va & vb;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0xFF00FF00FF00FF00ULL, 0x0F0F0F0F0F0F0F0FULL}, "SIMDPacked", 1},

  {"por_128",
   "typedef long long v2q __attribute__((vector_size(16)));\n"
   "long por_128(long a, long b) {\n"
   "  v2q va = {a, 0};\n"
   "  v2q vb = {b, 0};\n"
   "  v2q vr = va | vb;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0xFF00000000000000ULL, 0x00000000000000FFULL}, "SIMDPacked", 1},

  {"pxor_128",
   "typedef long long v2q __attribute__((vector_size(16)));\n"
   "long pxor_128(long a, long b) {\n"
   "  v2q va = {a, 0};\n"
   "  v2q vb = {b, 0};\n"
   "  v2q vr = va ^ vb;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0xDEADBEEFCAFEBABEULL, 0xFFFFFFFFFFFFFFFFULL}, "SIMDPacked", 1},

  // ========== Scalar FP add/sub/mul/div in XMM ==========
  {"addsd_rt",
   "long addsd_rt(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  double r = da + db;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4008000000000000ULL, 0x4014000000000000ULL}, "SIMDPacked"},

  {"subsd_rt",
   "long subsd_rt(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  double r = da - db;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4014000000000000ULL, 0x4008000000000000ULL}, "SIMDPacked"},

  {"mulsd_rt",
   "long mulsd_rt(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  double r = da * db;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4008000000000000ULL, 0x4014000000000000ULL}, "SIMDPacked"},

  {"divsd_rt",
   "long divsd_rt(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da, &a, 8); __builtin_memcpy(&db, &b, 8);\n"
   "  double r = da / db;\n"
   "  long ret; __builtin_memcpy(&ret, &r, 8); return ret;\n"
   "}\n",
   {0x4024000000000000ULL, 0x4000000000000000ULL}, "SIMDPacked"},

  // ========== Float (32-bit) scalar ops ==========
  {"addss_rt",
   "long addss_rt(long a, long b) {\n"
   "  int ai = (int)a, bi = (int)b;\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &ai, 4); __builtin_memcpy(&fb, &bi, 4);\n"
   "  float r = fa + fb;\n"
   "  int ri; __builtin_memcpy(&ri, &r, 4);\n"
   "  return (long)(unsigned)ri;\n"
   "}\n",
   {0x40A00000ULL, 0x40400000ULL}, "SIMDPacked"},

  {"mulss_rt",
   "long mulss_rt(long a, long b) {\n"
   "  int ai = (int)a, bi = (int)b;\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa, &ai, 4); __builtin_memcpy(&fb, &bi, 4);\n"
   "  float r = fa * fb;\n"
   "  int ri; __builtin_memcpy(&ri, &r, 4);\n"
   "  return (long)(unsigned)ri;\n"
   "}\n",
   {0x40A00000ULL, 0x40400000ULL}, "SIMDPacked"},

  // ========== PCMPEQD-like pattern (regression for #27) ==========
  {"vec_cmpeq_i32",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long vec_cmpeq_i32(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i vr = (va == vb);\n"
   "  return (long)(unsigned)vr[0];\n"
   "}\n",
   {42, 42}, "SIMDPacked", 1},

  {"vec_cmpeq_i32_ne",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long vec_cmpeq_i32_ne(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i vr = (va == vb);\n"
   "  return (long)(unsigned)vr[0];\n"
   "}\n",
   {42, 100}, "SIMDPacked", 1},

  // ========== PCMPGTD-like pattern ==========
  {"vec_cmpgt_i32",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long vec_cmpgt_i32(long a, long b) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {(int)b, 0, 0, 0};\n"
   "  v4i vr = (va > vb);\n"
   "  return (long)(unsigned)vr[0];\n"
   "}\n",
   {100, 42}, "SIMDPacked", 1},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(SIMDPacked, X64SIMDPackRT,
                         ::testing::ValuesIn(kX64SIMDPack), rtTCName);
