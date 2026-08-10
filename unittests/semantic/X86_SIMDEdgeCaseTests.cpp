//===- X86_SIMDEdgeCaseTests.cpp - x86 SIMD edge cases -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests for x86 SIMD patterns that exercise per-lane semantics:
// - Packed integer multiply (PMULLW, PMULLD)
// - Packed compare (PCMPEQB/W/D)
// - Horizontal operations (PHADDD, PHADDW)
// - Shuffle/permute (PSHUFD, PSHUFB)
// - Conversion (CVTDQ2PS, CVTPS2DQ)
// - Mixed width operations (PUNPCK, PACK)
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SIMDEdgeRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SIMDEdgeRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64SIMDEdge = {
  // ========== Packed integer multiply ==========
  {"sse_pmullw",
   "typedef short v8h __attribute__((vector_size(16)));\n"
   "long sse_pmullw(long a, long b) {\n"
   "  v8h va = {(short)a, (short)(a>>16), (short)(a>>32), (short)(a>>48),\n"
   "            1, 1, 1, 1};\n"
   "  v8h vb = {(short)b, (short)(b>>16), (short)(b>>32), (short)(b>>48),\n"
   "            1, 1, 1, 1};\n"
   "  v8h vc = va * vb;\n"
   "  return (long)vc[0] + (long)vc[1] + (long)vc[2] + (long)vc[3];\n"
   "}\n",
   {0x0003000500070009ULL, 0x0002000400060008ULL}, "SIMDEdge", 1, "-msse2"},

  {"sse_pmulld",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long sse_pmulld(long a, long b) {\n"
   "  v4i va = {(int)a, (int)(a>>16), 1, 1};\n"
   "  v4i vb = {(int)b, (int)(b>>16), 1, 1};\n"
   "  v4i vc = va * vb;\n"
   "  return (long)vc[0] + (long)vc[1];\n"
   "}\n",
   {3, 7}, "SIMDEdge", 1, "-msse4.1"},

  // ========== Packed compare ==========
  {"sse_pcmpeqd",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long sse_pcmpeqd(long a) {\n"
   "  v4i va = {10, 20, 30, 40};\n"
   "  v4i vb = {10, 99, 30, 99};\n"
   "  v4i vc = (va == vb);\n"
   "  return (long)(vc[0]) + (long)(vc[2]);\n"
   "}\n",
   {0}, "SIMDEdge", 1, "-msse2"},

  {"sse_pcmpgtd",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long sse_pcmpgtd(long a) {\n"
   "  v4i va = {10, 20, 30, 40};\n"
   "  v4i vb = {5, 25, 15, 45};\n"
   "  v4i vc = (va > vb);\n"
   "  return (long)(vc[0]) + (long)(vc[1]) + (long)(vc[2]) + (long)(vc[3]);\n"
   "}\n",
   {0}, "SIMDEdge", 1, "-msse2"},

  // ========== Packed FP operations ==========
  {"sse_addps",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long sse_addps(long a) {\n"
   "  float fa = 1.5f, fb = 2.5f, fc = 3.5f, fd = 4.5f;\n"
   "  v4f va = {fa, fb, fc, fd};\n"
   "  v4f vb = {1.0f, 2.0f, 3.0f, 4.0f};\n"
   "  v4f vc = va + vb;\n"
   "  float sum = vc[0] + vc[1] + vc[2] + vc[3];\n"
   "  long r; __builtin_memcpy(&r, &sum, 4);\n"
   "  return r;\n"
   "}\n",
   {0}, "SIMDEdge", 1, "-msse2"},

  // ========== Packed min/max ==========
  {"sse_pminsw",
   "typedef short v8h __attribute__((vector_size(16)));\n"
   "long sse_pminsw(long a) {\n"
   "  v8h va = {100, -200, 300, -400, 50, -150, 250, -350};\n"
   "  v8h vb = {50, -100, 400, -300, 100, -200, 200, -400};\n"
   "  v8h vc;\n"
   "  for (int i = 0; i < 8; ++i)\n"
   "    vc[i] = va[i] < vb[i] ? va[i] : vb[i];\n"
   "  return (long)vc[0] + (long)vc[1] + (long)vc[2] + (long)vc[3];\n"
   "}\n",
   {0}, "SIMDEdge"},

  // ========== Packed shift ==========
  {"sse_psllw",
   "typedef short v8h __attribute__((vector_size(16)));\n"
   "long sse_psllw(long a) {\n"
   "  v8h va = {1, 2, 3, 4, 5, 6, 7, 8};\n"
   "  v8h vc = va << 3;\n"
   "  return (long)vc[0] + vc[1] + vc[2] + vc[3] + vc[4] + vc[5] + vc[6] + vc[7];\n"
   "}\n",
   {0}, "SIMDEdge", 1, "-msse2"},

  // ========== Integer conversion ==========
  {"sse_cvtdq2ps",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long sse_cvtdq2ps(long a) {\n"
   "  v4i vi = {10, 20, 30, 40};\n"
   "  v4f vf = __builtin_convertvector(vi, v4f);\n"
   "  float sum = vf[0] + vf[1] + vf[2] + vf[3];\n"
   "  long r; __builtin_memcpy(&r, &sum, 4);\n"
   "  return r;\n"
   "}\n",
   {0}, "SIMDEdge", 1, "-msse2"},

  // ========== Byte/word packing ==========
  {"sse_packsswb",
   "typedef short v8h __attribute__((vector_size(16)));\n"
   "long sse_pack(long a) {\n"
   "  v8h va = {100, 200, -100, -200, 50, 150, -50, -150};\n"
   "  signed char r[8];\n"
   "  for (int i = 0; i < 8; ++i) {\n"
   "    short v = va[i];\n"
   "    if (v > 127) v = 127;\n"
   "    if (v < -128) v = -128;\n"
   "    r[i] = (signed char)v;\n"
   "  }\n"
   "  long result = 0;\n"
   "  for (int i = 0; i < 8; ++i) result += r[i];\n"
   "  return result;\n"
   "}\n",
   {0}, "SIMDEdge"},

  // ========== Mixed scalar/vector ==========
  {"sse_scalar_in_vec",
   "long sse_scalar_in_vec(long a, long b) {\n"
   "  double da = (double)a;\n"
   "  double db = (double)b;\n"
   "  double result = da * db + da - db;\n"
   "  long r;\n"
   "  __builtin_memcpy(&r, &result, 8);\n"
   "  return r;\n"
   "}\n",
   {7, 13}, "SIMDEdge"},

  // ========== Conditional patterns with SIMD-like data ==========
  {"x64_abs_i32",
   "long x64_abs_i32(long a) {\n"
   "  int x = (int)a;\n"
   "  return (long)(x < 0 ? -x : x);\n"
   "}\n",
   {(uint64_t)(int64_t)-42}, "SIMDEdge"},

  {"x64_min_max_chain",
   "long x64_min_max(long a, long b, long c) {\n"
   "  long mn = a < b ? a : b;\n"
   "  mn = mn < c ? mn : c;\n"
   "  long mx = a > b ? a : b;\n"
   "  mx = mx > c ? mx : c;\n"
   "  return mx - mn;\n"
   "}\n",
   {42, 17, 99}, "SIMDEdge"},

  // ========== PSHUFD — dword shuffle ==========
  {"sse_pshufd",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long sse_pshufd(long a) {\n"
   "  v4i v = {10, 20, 30, 40};\n"
   "  v4i s = __builtin_shufflevector(v, v, 3, 2, 1, 0);\n"
   "  return (long)s[0] + (long)s[3];\n"
   "}\n",
   {0}, "SIMDEdge", 1, "-msse2"},

  // ========== Chained SIMD: add then compare ==========
  {"sse_chain_add_cmp",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long sse_chain(long a) {\n"
   "  v4i va = {10, 20, 30, 40};\n"
   "  v4i vb = {5, 25, 15, 45};\n"
   "  v4i sum = va + vb;\n"
   "  v4i cmp = (sum > (v4i){20, 20, 20, 20});\n"
   "  return (long)(cmp[0]) + (long)(cmp[1]) + (long)(cmp[2]) + (long)(cmp[3]);\n"
   "}\n",
   {0}, "SIMDEdge", 1, "-msse2"},

  // ========== SIMD multiply then add (dot product pattern) ==========
  {"sse_dot_product",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long sse_dot(long a) {\n"
   "  v4i va = {1, 2, 3, 4};\n"
   "  v4i vb = {5, 6, 7, 8};\n"
   "  v4i vc = va * vb;\n"
   "  return (long)vc[0] + (long)vc[1] + (long)vc[2] + (long)vc[3];\n"
   "}\n",
   {0}, "SIMDEdge", 1, "-msse4.1"},

  // ========== XMM write followed by MOVQ (64-bit extract) ==========
  {"sse_movq_extract",
   "typedef long long v2q __attribute__((vector_size(16)));\n"
   "long sse_movq(long a, long b) {\n"
   "  v2q v = {a, b};\n"
   "  v2q sum = v + (v2q){1, 2};\n"
   "  return (long)sum[0];\n"
   "}\n",
   {100, 200}, "SIMDEdge", 1, "-msse2"},

  // ========== Packed AND + lane extract ==========
  {"sse_pand_extract",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long sse_pand(long a) {\n"
   "  v4i va = {0xFF, 0xFFF, 0xFFFF, 0xFFFFF};\n"
   "  v4i vb = {0xF0, 0xF00, 0xF000, 0xF0000};\n"
   "  v4i vc = va & vb;\n"
   "  return (long)vc[0] + (long)vc[1] + (long)vc[2] + (long)vc[3];\n"
   "}\n",
   {0}, "SIMDEdge", 1, "-msse2"},

  // ========== Packed OR/XOR ==========
  {"sse_por_pxor",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long sse_por_pxor(long a) {\n"
   "  v4i va = {0xAA, 0xBB, 0xCC, 0xDD};\n"
   "  v4i vb = {0x55, 0x44, 0x33, 0x22};\n"
   "  v4i vor = va | vb;\n"
   "  v4i vxor = va ^ vb;\n"
   "  return (long)vor[0] + (long)vxor[0];\n"
   "}\n",
   {0}, "SIMDEdge", 1, "-msse2"},

  // ========== Packed subtract with lane extract ==========
  {"sse_psub_lane",
   "typedef short v8h __attribute__((vector_size(16)));\n"
   "long sse_psub(long a) {\n"
   "  v8h va = {100, 200, 300, 400, 500, 600, 700, 800};\n"
   "  v8h vb = {10, 20, 30, 40, 50, 60, 70, 80};\n"
   "  v8h vc = va - vb;\n"
   "  return (long)vc[0] + (long)vc[4];\n"
   "}\n",
   {0}, "SIMDEdge", 1, "-msse2"},

  // ========== MOVD from packed result to GPR (key Bug #70 pattern) ==========
  {"sse_movd_from_packed",
   "typedef int v4i __attribute__((vector_size(16)));\n"
   "long sse_movd(long a) {\n"
   "  v4i va = {(int)a, 0, 0, 0};\n"
   "  v4i vb = {3, 0, 0, 0};\n"
   "  v4i vc = va * vb;\n"
   "  return (long)vc[0];\n"
   "}\n",
   {7}, "SIMDEdge", 1, "-msse4.1"},

  // ========== Variable shift (dynamic amount, hard for optimizer) ==========
  {"x64_var_shl",
   "long x64_var_shl(long val, long amt) {\n"
   "  return val << (amt & 63);\n"
   "}\n",
   {0xDEADBEEF, 17}, "SIMDEdge"},

  {"x64_var_shr",
   "long x64_var_shr(long val, long amt) {\n"
   "  return (unsigned long)val >> (amt & 63);\n"
   "}\n",
   {0xDEADBEEFCAFEBABEULL, 32}, "SIMDEdge"},

  {"x64_var_sar",
   "long x64_var_sar(long val, long amt) {\n"
   "  return val >> (amt & 63);\n"
   "}\n",
   {(uint64_t)(int64_t)-123456, 8}, "SIMDEdge"},

  // ========== Byte-level sub-register stress ==========
  {"x64_byte_extract_chain",
   "long x64_byte_chain(long val) {\n"
   "  unsigned char a = (unsigned char)val;\n"
   "  unsigned char b = (unsigned char)(val >> 8);\n"
   "  unsigned char c = (unsigned char)(val >> 16);\n"
   "  unsigned char d = (unsigned char)(val >> 24);\n"
   "  return (long)(a + b + c + d);\n"
   "}\n",
   {0x0102030405060708ULL}, "SIMDEdge"},

  {"x64_word_insert",
   "long x64_word_insert(long val, long w) {\n"
   "  unsigned long v = (unsigned long)val;\n"
   "  v = (v & ~0xFFFF00ULL) | (((unsigned long)w & 0xFF) << 8);\n"
   "  return (long)v;\n"
   "}\n",
   {0xAABBCCDDEEFF0011ULL, 0x42}, "SIMDEdge"},

  // ========== Rotate by variable amount ==========
  {"x64_rol_var",
   "long x64_rol_var(long val, long amt) {\n"
   "  unsigned long v = (unsigned long)val;\n"
   "  unsigned s = (unsigned)(amt & 63);\n"
   "  return (long)((v << s) | (v >> (64 - s)));\n"
   "}\n",
   {0xDEADBEEFCAFEBABEULL, 13}, "SIMDEdge"},

  // ========== Conditional with SIMD-like pattern ==========
  {"x64_sat_add_u8",
   "long x64_sat_add_u8(long a, long b) {\n"
   "  unsigned char va = (unsigned char)a, vb = (unsigned char)b;\n"
   "  unsigned r = (unsigned)va + (unsigned)vb;\n"
   "  return (long)(r > 255 ? 255 : (unsigned char)r);\n"
   "}\n",
   {200, 100}, "SIMDEdge"},

  // ========== Scalar saturating narrow (C equivalent of PACK semantics) ==========
  {"c_sat_narrow_i16_to_i8",
   "long c_sat_narrow(long a) {\n"
   "  short vals[8] = {100, 200, -100, -200, 50, -50, 127, -128};\n"
   "  long result = 0;\n"
   "  for (int i = 0; i < 8; ++i) {\n"
   "    short v = vals[i];\n"
   "    if (v > 127) v = 127;\n"
   "    if (v < -128) v = -128;\n"
   "    result += (long)(signed char)v;\n"
   "  }\n"
   "  return result;\n"
   "}\n",
   {0}, "SIMDEdge"},

  // ========== Unsigned saturating narrow ==========
  {"c_sat_narrow_u16_to_u8",
   "long c_sat_narrow_u(long a) {\n"
   "  unsigned short vals[8] = {50, 100, 255, 300, 0, 128, 200, 500};\n"
   "  long result = 0;\n"
   "  for (int i = 0; i < 8; ++i) {\n"
   "    unsigned short v = vals[i];\n"
   "    if (v > 255) v = 255;\n"
   "    result += (long)(unsigned char)v;\n"
   "  }\n"
   "  return result;\n"
   "}\n",
   {0}, "SIMDEdge"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(SIMDEdge, X64SIMDEdgeRT,
                         ::testing::ValuesIn(kX64SIMDEdge), rtTCName);
