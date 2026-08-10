//===- X64_SSE4AdvRTTests.cpp - SSE4.1/SSSE3/advanced SIMD roundtrip ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip tests for SSE4.1/SSSE3/SSE3 instructions that historically have
// per-lane semantics bugs: HADDPS, ADDSUBPS, BLENDPS, DPPS, ROUNDSS,
// INSERTPS, PHMINPOSUW, UNPCKHPS, SHUFPS, CVTPS2PD, MOVHLPS/MOVLHPS.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SSE4AdvRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SSE4AdvRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

// ============================================================================
// HADDPS / HSUBPS — horizontal float add/sub (SSE3)
// ============================================================================
static const std::vector<RoundTripTC> kHAddSubPS = {
  {"haddps_basic",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long haddps_basic(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa,&a,4); __builtin_memcpy(&fb,&b,4);\n"
   "  v4f va = {fa, fb, 1.0f, 2.0f};\n"
   "  v4f vb = {3.0f, 4.0f, 5.0f, 6.0f};\n"
   "  v4f vr = __builtin_ia32_haddps(va, vb);\n"
   "  float r = vr[0];\n"
   "  long rv = 0; __builtin_memcpy(&rv,&r,4); return rv;\n"
   "}\n",
   {0x40400000ULL, 0x40800000ULL}, "HAddSubPS", 1, "-msse3 -mno-avx"},

  {"hsubps_basic",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long hsubps_basic(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa,&a,4); __builtin_memcpy(&fb,&b,4);\n"
   "  v4f va = {fa, fb, 10.0f, 3.0f};\n"
   "  v4f vb = {1.0f, 1.0f, 1.0f, 1.0f};\n"
   "  v4f vr = __builtin_ia32_hsubps(va, vb);\n"
   "  float r = vr[0];\n"
   "  long rv = 0; __builtin_memcpy(&rv,&r,4); return rv;\n"
   "}\n",
   {0x41200000ULL, 0x40A00000ULL}, "HAddSubPS", 1, "-msse3 -mno-avx"},

  {"haddpd_basic",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long haddpd_basic(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da,&a,8); __builtin_memcpy(&db,&b,8);\n"
   "  v2d va = {da, db};\n"
   "  v2d vr = __builtin_ia32_haddpd(va, va);\n"
   "  double r = vr[0];\n"
   "  long rv; __builtin_memcpy(&rv,&r,8); return rv;\n"
   "}\n",
   {0x4000000000000000ULL, 0x4008000000000000ULL}, "HAddSubPS", 1, "-msse3 -mno-avx"},
};

// ============================================================================
// ADDSUBPS / ADDSUBPD — alternating sub/add per lane (SSE3)
// ============================================================================
static const std::vector<RoundTripTC> kAddSubPS = {
  {"addsubps_basic",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long addsubps_basic(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa,&a,4); __builtin_memcpy(&fb,&b,4);\n"
   "  v4f va = {fa, fb, 1.0f, 2.0f};\n"
   "  v4f vb = {1.0f, 1.0f, 1.0f, 1.0f};\n"
   "  v4f vr = __builtin_ia32_addsubps(va, vb);\n"
   "  float r0 = vr[0], r1 = vr[1];\n"
   "  int i0, i1;\n"
   "  __builtin_memcpy(&i0,&r0,4); __builtin_memcpy(&i1,&r1,4);\n"
   "  return ((long)(unsigned)i1 << 32) | (unsigned)i0;\n"
   "}\n",
   {0x40A00000ULL, 0x40400000ULL}, "AddSubPS", 1, "-msse3 -mno-avx"},

  {"addsubpd_basic",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long addsubpd_basic(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da,&a,8); __builtin_memcpy(&db,&b,8);\n"
   "  v2d va = {da, db};\n"
   "  v2d vb = {1.0, 1.0};\n"
   "  v2d vr = __builtin_ia32_addsubpd(va, vb);\n"
   "  double r = vr[0];\n"
   "  long rv; __builtin_memcpy(&rv,&r,8); return rv;\n"
   "}\n",
   {0x4014000000000000ULL, 0x4008000000000000ULL}, "AddSubPS", 1, "-msse3 -mno-avx"},
};

// ============================================================================
// BLENDPS / BLENDPD — SSE4.1 immediate blend
// ============================================================================
static const std::vector<RoundTripTC> kBlendPS = {
  {"blendps_imm",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long blendps_imm(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa,&a,4); __builtin_memcpy(&fb,&b,4);\n"
   "  v4f va = {fa, 2.0f, 3.0f, 4.0f};\n"
   "  v4f vb = {fb, 20.0f, 30.0f, 40.0f};\n"
   "  v4f vr = __builtin_ia32_blendps(va, vb, 0x5);\n"
   "  float r = vr[0];\n"
   "  long rv = 0; __builtin_memcpy(&rv,&r,4); return rv;\n"
   "}\n",
   {0x40A00000ULL, 0x42C80000ULL}, "BlendPS", 1, "-msse4.1 -mno-avx"},

  {"blendpd_imm",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long blendpd_imm(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da,&a,8); __builtin_memcpy(&db,&b,8);\n"
   "  v2d va = {da, 2.0};\n"
   "  v2d vb = {db, 20.0};\n"
   "  v2d vr = __builtin_ia32_blendpd(va, vb, 0x2);\n"
   "  double r = vr[0];\n"
   "  long rv; __builtin_memcpy(&rv,&r,8); return rv;\n"
   "}\n",
   {0x4014000000000000ULL, 0x4059000000000000ULL}, "BlendPS", 1, "-msse4.1 -mno-avx"},
};

// ============================================================================
// ROUNDSS / ROUNDSD / ROUNDPS — SSE4.1 rounding
// ============================================================================
static const std::vector<RoundTripTC> kRoundSS = {
  {"roundss_floor",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long roundss_floor(long a) {\n"
   "  float fa; __builtin_memcpy(&fa,&a,4);\n"
   "  v4f va = {fa, 0,0,0};\n"
   "  v4f vr = __builtin_ia32_roundss(va, va, 0x1);\n"
   "  float r = vr[0];\n"
   "  long rv = 0; __builtin_memcpy(&rv,&r,4); return rv;\n"
   "}\n",
   {0x40533333ULL}, "RoundSS", 1, "-msse4.1 -mno-avx"},

  {"roundsd_ceil",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long roundsd_ceil(long a) {\n"
   "  double da; __builtin_memcpy(&da,&a,8);\n"
   "  v2d va = {da, 0};\n"
   "  v2d vr = __builtin_ia32_roundsd(va, va, 0x2);\n"
   "  double r = vr[0];\n"
   "  long rv; __builtin_memcpy(&rv,&r,8); return rv;\n"
   "}\n",
   {0x4002666666666666ULL}, "RoundSS", 1, "-msse4.1 -mno-avx"},

  {"roundss_trunc",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long roundss_trunc(long a) {\n"
   "  float fa; __builtin_memcpy(&fa,&a,4);\n"
   "  v4f va = {fa, 0,0,0};\n"
   "  v4f vr = __builtin_ia32_roundss(va, va, 0x3);\n"
   "  float r = vr[0];\n"
   "  long rv = 0; __builtin_memcpy(&rv,&r,4); return rv;\n"
   "}\n",
   {0xC0533333ULL}, "RoundSS", 1, "-msse4.1 -mno-avx"},
};

// ============================================================================
// DPPS / DPPD — SSE4.1 dot product
// ============================================================================
static const std::vector<RoundTripTC> kDPPS = {
  {"dpps_basic",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long dpps_basic(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa,&a,4); __builtin_memcpy(&fb,&b,4);\n"
   "  v4f va = {fa, 0, 0, 0};\n"
   "  v4f vb = {fb, 0, 0, 0};\n"
   "  v4f vr = __builtin_ia32_dpps(va, vb, 0x11);\n"
   "  float r = vr[0];\n"
   "  long rv = 0; __builtin_memcpy(&rv,&r,4); return rv;\n"
   "}\n",
   {0x40400000ULL, 0x40800000ULL}, "DPPS", 1, "-msse4.1 -mno-avx"},

  {"dppd_basic",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long dppd_basic(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da,&a,8); __builtin_memcpy(&db,&b,8);\n"
   "  v2d va = {da, 0};\n"
   "  v2d vb = {db, 0};\n"
   "  v2d vr = __builtin_ia32_dppd(va, vb, 0x11);\n"
   "  double r = vr[0];\n"
   "  long rv; __builtin_memcpy(&rv,&r,8); return rv;\n"
   "}\n",
   {0x4000000000000000ULL, 0x4008000000000000ULL}, "DPPS", 1, "-msse4.1 -mno-avx"},
};

// ============================================================================
// INSERTPS / EXTRACTPS — SSE4.1 insert/extract packed single
// ============================================================================
static const std::vector<RoundTripTC> kInsertExtractPS = {
  {"insertps_lane1",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long insertps_lane1(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa,&a,4); __builtin_memcpy(&fb,&b,4);\n"
   "  v4f va = {fa, 0.0f, 0.0f, 0.0f};\n"
   "  v4f vb = {fb, 0.0f, 0.0f, 0.0f};\n"
   "  v4f vr = __builtin_ia32_insertps128(va, vb, 0x10);\n"
   "  float r = vr[1];\n"
   "  long rv = 0; __builtin_memcpy(&rv,&r,4); return rv;\n"
   "}\n",
   {0x40A00000ULL, 0x42C80000ULL}, "InsertExtractPS", 1, "-msse4.1 -mno-avx"},

  {"extractps_lane2",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long extractps_lane2(long a) {\n"
   "  float fa; __builtin_memcpy(&fa,&a,4);\n"
   "  v4f va = {1.0f, 2.0f, fa, 4.0f};\n"
   "  float r = va[2];\n"
   "  long rv = 0; __builtin_memcpy(&rv,&r,4); return rv;\n"
   "}\n",
   {0x40A00000ULL}, "InsertExtractPS", 1, "-msse4.1 -mno-avx"},
};

// ============================================================================
// UNPCKHPS / UNPCKLPS / SHUFPS — interleave/shuffle
// ============================================================================
static const std::vector<RoundTripTC> kUnpackShuffle = {
  {"unpcklps_basic",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long unpcklps_basic(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa,&a,4); __builtin_memcpy(&fb,&b,4);\n"
   "  v4f va = {fa, 2.0f, 3.0f, 4.0f};\n"
   "  v4f vb = {fb, 20.0f, 30.0f, 40.0f};\n"
   "  v4f vr = __builtin_shufflevector(va, vb, 0, 4, 1, 5);\n"
   "  float r = vr[0];\n"
   "  long rv = 0; __builtin_memcpy(&rv,&r,4); return rv;\n"
   "}\n",
   {0x40A00000ULL, 0x42C80000ULL}, "UnpackShuffle", 1, "-mno-avx"},

  {"unpckhps_basic",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long unpckhps_basic(long a, long b) {\n"
   "  float fa, fb;\n"
   "  __builtin_memcpy(&fa,&a,4); __builtin_memcpy(&fb,&b,4);\n"
   "  v4f va = {1.0f, 2.0f, fa, 4.0f};\n"
   "  v4f vb = {10.0f, 20.0f, fb, 40.0f};\n"
   "  v4f vr = __builtin_shufflevector(va, vb, 2, 6, 3, 7);\n"
   "  float r = vr[0];\n"
   "  long rv = 0; __builtin_memcpy(&rv,&r,4); return rv;\n"
   "}\n",
   {0x40400000ULL, 0x40800000ULL}, "UnpackShuffle", 1, "-mno-avx"},

  {"shufps_basic",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long shufps_basic(long a) {\n"
   "  float fa; __builtin_memcpy(&fa,&a,4);\n"
   "  v4f va = {fa, 2.0f, 3.0f, 4.0f};\n"
   "  v4f vr = __builtin_shufflevector(va, va, 3, 2, 1, 0);\n"
   "  float r = vr[3];\n"
   "  long rv = 0; __builtin_memcpy(&rv,&r,4); return rv;\n"
   "}\n",
   {0x40A00000ULL}, "UnpackShuffle", 1, "-mno-avx"},

  {"shufpd_basic",
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long shufpd_basic(long a, long b) {\n"
   "  double da, db;\n"
   "  __builtin_memcpy(&da,&a,8); __builtin_memcpy(&db,&b,8);\n"
   "  v2d va = {da, 1.0};\n"
   "  v2d vb = {db, 2.0};\n"
   "  v2d vr = __builtin_shufflevector(va, vb, 1, 2);\n"
   "  double r = vr[0];\n"
   "  long rv; __builtin_memcpy(&rv,&r,8); return rv;\n"
   "}\n",
   {0x4000000000000000ULL, 0x4008000000000000ULL}, "UnpackShuffle", 1, "-mno-avx"},
};

// ============================================================================
// CVTPS2PD / CVTPD2PS — packed float precision conversions
// ============================================================================
static const std::vector<RoundTripTC> kPackedConv = {
  {"cvtps2pd_basic",
   "long cvtps2pd_basic(long a) {\n"
   "  float fa; __builtin_memcpy(&fa,&a,4);\n"
   "  double r = (double)fa;\n"
   "  long rv; __builtin_memcpy(&rv,&r,8); return rv;\n"
   "}\n",
   {0x40400000ULL}, "PackedConv", 1, "-mno-avx"},

  {"cvtpd2ps_basic",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "typedef double v2d __attribute__((vector_size(16)));\n"
   "long cvtpd2ps_basic(long a) {\n"
   "  double da; __builtin_memcpy(&da,&a,8);\n"
   "  v2d va = {da, 0};\n"
   "  v4f vr = __builtin_ia32_cvtpd2ps(va);\n"
   "  float r = vr[0];\n"
   "  long rv = 0; __builtin_memcpy(&rv,&r,4); return rv;\n"
   "}\n",
   {0x4014000000000000ULL}, "PackedConv", 1, "-mno-avx"},

  {"cvtdq2pd_basic",
   "long cvtdq2pd_basic(long a) {\n"
   "  int ia = (int)a;\n"
   "  double r = (double)ia;\n"
   "  long rv; __builtin_memcpy(&rv,&r,8); return rv;\n"
   "}\n",
   {42}, "PackedConv", 1, "-mno-avx"},
};

// ============================================================================
// MOVHLPS / MOVLHPS — move high/low packed singles
// ============================================================================
static const std::vector<RoundTripTC> kMovHLPS = {
  {"movhlps_basic",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long movhlps_basic(long a) {\n"
   "  float fa; __builtin_memcpy(&fa,&a,4);\n"
   "  v4f va = {1.0f, 2.0f, fa, 4.0f};\n"
   "  v4f vb = {10.0f, 20.0f, 30.0f, 40.0f};\n"
   "  v4f vr = __builtin_shufflevector(vb, va, 2, 3, 0, 1);\n"
   "  float r = vr[1];\n"
   "  long rv = 0; __builtin_memcpy(&rv,&r,4); return rv;\n"
   "}\n",
   {0x40400000ULL}, "MovHLPS", 1, "-mno-avx"},

  {"movlhps_basic",
   "typedef float v4f __attribute__((vector_size(16)));\n"
   "long movlhps_basic(long a) {\n"
   "  float fa; __builtin_memcpy(&fa,&a,4);\n"
   "  v4f va = {fa, 2.0f, 3.0f, 4.0f};\n"
   "  v4f vb = {10.0f, 20.0f, 30.0f, 40.0f};\n"
   "  v4f vr = __builtin_shufflevector(va, vb, 0, 1, 4, 5);\n"
   "  float r = vr[2];\n"
   "  long rv = 0; __builtin_memcpy(&rv,&r,4); return rv;\n"
   "}\n",
   {0x40A00000ULL}, "MovHLPS", 1, "-mno-avx"},
};

// ============================================================================
// CRC32 — SSE4.2 accumulate (Castagnoli).  Lifts to @llvm.x86.sse42.crc32.*;
// codegen now propagates +sse4.2,+crc32 (CodeGenX86::detectTargetFeaturesX86)
// so the backend selects the hardware `crc32` instead of a libcall.  Each width
// (byte/word/dword/qword) plus a chained mix; folds to one return.
// ============================================================================
static const std::vector<RoundTripTC> kCRC32 = {
  {"crc32_byte",
   "long crc32_byte(long a){\n"
   "  unsigned acc=(unsigned)a;\n"
   "  for(int i=0;i<8;i++) acc=__builtin_ia32_crc32qi(acc,(unsigned char)(a>>(i*8)));\n"
   "  return (long)acc; }\n",
   {0x1122334455667788ULL}, "CRC32", 1, "-msse4.2 -mno-avx"},

  {"crc32_word",
   "long crc32_word(long a){\n"
   "  unsigned acc=(unsigned)a;\n"
   "  for(int i=0;i<4;i++) acc=__builtin_ia32_crc32hi(acc,(unsigned short)(a>>(i*16)));\n"
   "  return (long)acc; }\n",
   {0xABCDEF0123456789ULL}, "CRC32", 1, "-msse4.2 -mno-avx"},

  {"crc32_dword",
   "long crc32_dword(long a){\n"
   "  unsigned acc=(unsigned)a;\n"
   "  acc=__builtin_ia32_crc32si(acc,(unsigned)a);\n"
   "  acc=__builtin_ia32_crc32si(acc,(unsigned)(a>>32));\n"
   "  return (long)acc; }\n",
   {0xDEADBEEFCAFEF00DULL}, "CRC32", 1, "-msse4.2 -mno-avx"},

  {"crc32_qword",
   "long crc32_qword(long a){\n"
   "  unsigned long long acc=(unsigned)a;\n"
   "  acc=__builtin_ia32_crc32di(acc,(unsigned long long)a);\n"
   "  acc=__builtin_ia32_crc32di(acc,(unsigned long long)a*2654435761ull);\n"
   "  return (long)(unsigned)acc; }\n",
   {0x0123456789ABCDEFULL}, "CRC32", 1, "-msse4.2 -mno-avx"},

  // Chain all four widths so a single missing feature/lowering surfaces.
  {"crc32_mixed",
   "long crc32_mixed(long a){\n"
   "  unsigned acc=(unsigned)a;\n"
   "  acc=__builtin_ia32_crc32qi(acc,(unsigned char)(a>>3));\n"
   "  acc=__builtin_ia32_crc32hi(acc,(unsigned short)(a>>5));\n"
   "  acc=__builtin_ia32_crc32si(acc,(unsigned)(a>>7));\n"
   "  unsigned long long acc64=acc;\n"
   "  acc64=__builtin_ia32_crc32di(acc64,(unsigned long long)a*0x9e3779b97f4a7c15ull);\n"
   "  return (long)(unsigned)(acc64 ^ (acc64>>32)); }\n",
   {0x71C2D3E4F5061728ULL}, "CRC32", 1, "-msse4.2 -mno-avx"},
};

// ============================================================================
// PABSB / PABSW / PABSD — packed absolute value (SSSE3)
// ============================================================================
static const std::vector<RoundTripTC> kPABS = {
  {"pabsd_c_expr",
   "long pabsd_c_expr(long a) {\n"
   "  int v = (int)a;\n"
   "  return (long)(unsigned)(v < 0 ? -v : v);\n"
   "}\n",
   {(unsigned long long)(int)-42}, "PABS", 1},

  {"pabs_chain",
   "long pabs_chain(long a, long b) {\n"
   "  int va = (int)a, vb = (int)b;\n"
   "  int ra = va < 0 ? -va : va;\n"
   "  int rb = vb < 0 ? -vb : vb;\n"
   "  return (long)(unsigned)(ra + rb);\n"
   "}\n",
   {(unsigned long long)(int)-10, (unsigned long long)(int)-20}, "PABS", 1},
};

// ============================================================================
// MPSADBW — multiple packed sum of absolute differences (SSE4.1).  The imm8
// control byte selects the source block/offset and must round-trip.
// ============================================================================
static const std::vector<RoundTripTC> kMpsadbw = {
  {"mpsadbw_imm0",
   "typedef char v16c __attribute__((vector_size(16)));\n"
   "typedef short v8s __attribute__((vector_size(16)));\n"
   "long mpsadbw_imm0(long a) {\n"
   "  v16c x = {(char)a,10,20,30,40,50,60,70,80,90,100,110,120,1,2,3};\n"
   "  v16c y = {3,5,7,9,1,2,3,4,5,6,7,8,9,10,11,12};\n"
   "  v8s r = __builtin_ia32_mpsadbw128(x, y, 0);\n"
   "  return (long)(unsigned short)(r[0] + r[1] + r[4] + r[7]);\n"
   "}\n",
   {0x25}, "Mpsadbw", 1, "-msse4.1 -mno-avx"},

  {"mpsadbw_imm5",
   "typedef char v16c __attribute__((vector_size(16)));\n"
   "typedef short v8s __attribute__((vector_size(16)));\n"
   "long mpsadbw_imm5(long a) {\n"
   "  v16c x = {(char)a,10,20,30,40,50,60,70,80,90,100,110,120,1,2,3};\n"
   "  v16c y = {3,5,7,9,1,2,3,4,5,6,7,8,9,10,11,12};\n"
   "  v8s r = __builtin_ia32_mpsadbw128(x, y, 5);\n"  // src2 block 1, src1 offset 1
   "  return (long)(unsigned short)(r[0] + r[2] + r[5] + r[7]);\n"
   "}\n",
   {0x40}, "Mpsadbw", 1, "-msse4.1 -mno-avx"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(Mpsadbw, X64SSE4AdvRT,
                         ::testing::ValuesIn(kMpsadbw), rtTCName);
INSTANTIATE_TEST_SUITE_P(HAddSubPS, X64SSE4AdvRT,
                         ::testing::ValuesIn(kHAddSubPS), rtTCName);
INSTANTIATE_TEST_SUITE_P(AddSubPS, X64SSE4AdvRT,
                         ::testing::ValuesIn(kAddSubPS), rtTCName);
INSTANTIATE_TEST_SUITE_P(BlendPS, X64SSE4AdvRT,
                         ::testing::ValuesIn(kBlendPS), rtTCName);
INSTANTIATE_TEST_SUITE_P(RoundSS, X64SSE4AdvRT,
                         ::testing::ValuesIn(kRoundSS), rtTCName);
INSTANTIATE_TEST_SUITE_P(DPPS, X64SSE4AdvRT,
                         ::testing::ValuesIn(kDPPS), rtTCName);
INSTANTIATE_TEST_SUITE_P(InsExtPS, X64SSE4AdvRT,
                         ::testing::ValuesIn(kInsertExtractPS), rtTCName);
INSTANTIATE_TEST_SUITE_P(UnpackShuf, X64SSE4AdvRT,
                         ::testing::ValuesIn(kUnpackShuffle), rtTCName);
INSTANTIATE_TEST_SUITE_P(PackedConv, X64SSE4AdvRT,
                         ::testing::ValuesIn(kPackedConv), rtTCName);
INSTANTIATE_TEST_SUITE_P(MovHLPS, X64SSE4AdvRT,
                         ::testing::ValuesIn(kMovHLPS), rtTCName);
INSTANTIATE_TEST_SUITE_P(CRC32, X64SSE4AdvRT,
                         ::testing::ValuesIn(kCRC32), rtTCName);
INSTANTIATE_TEST_SUITE_P(PABS, X64SSE4AdvRT,
                         ::testing::ValuesIn(kPABS), rtTCName);
