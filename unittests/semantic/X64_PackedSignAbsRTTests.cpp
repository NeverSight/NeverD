//===- X64_PackedSignAbsRTTests.cpp - SSSE3 PABS/PSIGN lane RT --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// SSSE3 per-lane sign manipulation, both built on a sign-driven per-lane SELECT:
//
//   PABS{B,W,D}   dst[i] = |src[i]|                       (abs)
//   PSIGN{B,W,D}  dst[i] = sign[i] < 0 ? -data[i]
//                        : sign[i] == 0 ? 0
//                        :                data[i]          (apply sign-of)
//
// Two correctness corners that weak coverage tends to hide:
//
//   1. abs(INT_MIN) == INT_MIN.  Two's-complement negation of the most-negative
//      lane value (0x80 / 0x8000 / 0x80000000) is itself, so a lane that picks
//      `-x` for x<0 must still return 0x80.. there.  A test using only small
//      magnitudes never exercises the overflow lane.
//
//   2. PSIGN's THREE-way result.  The `sign == 0 -> 0` arm is easy to drop (a
//      two-way `sign<0 ? -x : x` looks plausible but leaks x when sign==0).  Each
//      probe drives sign lanes that are <0, ==0 and >0 simultaneously.
//
// Both handlers assemble the 128-bit result two 64-bit halves at a time
// (BuildHalf(0)/BuildHalf(8) + CONCAT), so every probe folds ALL lanes (16 bytes /
// 8 words / 4 dwords) through a rolling hash — a transposed lane, a wrong
// half-merge, or a bad SUBBYTES offset on any lane changes the digest.
//
// Despite both being plain SSSE3, neither PABS nor PSIGN had a dedicated
// roundtrip test (only incidental use inside AllPlatform vector algorithms).  The
// source/sign vectors are seeded from runtime arguments via `_mm_set_epi64x`, so
// clang materializes them with register moves (no rodata constant pool — this
// deliberately sidesteps the separate rodata-vector-load limitation and isolates
// the handlers).  `-ffreestanding` lets <tmmintrin.h> compile under the harness's
// -nostdlib.  SSSE3 is in qemu64's CPUID, so PABS/PSIGN decode on the default
// Unicorn x86-64 CPU; the oracle is original-Unicorn vs lifted-Unicorn.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64PackedSignAbsRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64PackedSignAbsRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
#define HDR "#include <tmmintrin.h>\n"

// PABS: source vector from {a,b}; fold all N lanes (read back as UTYPE).
#define PABS_FN(VTYPE, N, UTYPE, INTR) \
  HDR \
  "typedef " #VTYPE " vt __attribute__((vector_size(16)));\n" \
  "unsigned long f(unsigned long a, unsigned long b){\n" \
  "  __m128i v=_mm_set_epi64x((long long)b,(long long)a);\n" \
  "  vt r=(vt)" #INTR "(v);\n" \
  "  unsigned long h=0; for(int i=0;i<" #N ";i++) h=h*131u+(" #UTYPE ")r[i];\n" \
  "  return h;}\n"

// PSIGN: data vector from {a,b}, sign vector from {c,d}; fold all N lanes.
#define PSIGN_FN(VTYPE, N, UTYPE, INTR) \
  HDR \
  "typedef " #VTYPE " vt __attribute__((vector_size(16)));\n" \
  "unsigned long f(unsigned long a, unsigned long b,\n" \
  "               unsigned long c, unsigned long d){\n" \
  "  __m128i x=_mm_set_epi64x((long long)b,(long long)a);\n" \
  "  __m128i y=_mm_set_epi64x((long long)d,(long long)c);\n" \
  "  vt r=(vt)" #INTR "(x,y);\n" \
  "  unsigned long h=0; for(int i=0;i<" #N ";i++) h=h*131u+(" #UTYPE ")r[i];\n" \
  "  return h;}\n"

static const std::vector<RoundTripTC> kX64 = {

  // ===== PABSB — 16 byte lanes; 0x80 lane must abs to 0x80. =====
  {"pabsb_corner", PABS_FN(signed char,16,unsigned char,_mm_abs_epi8),
   {0xC040FE017F00FF80ULL, 0xCC3355AA80027E81ULL}, "PackedSignAbs", 1,
   "-mssse3 -ffreestanding"},
  {"pabsb_mix",    PABS_FN(signed char,16,unsigned char,_mm_abs_epi8),
   {0x0102037F80FE8100ULL, 0x7F7E0180FFFE0040ULL}, "PackedSignAbs", 1,
   "-mssse3 -ffreestanding"},

  // ===== PABSW — 8 word lanes; 0x8000 lane must abs to 0x8000. =====
  {"pabsw_corner", PABS_FN(short,8,unsigned short,_mm_abs_epi16),
   {0x7FFF0000FFFF8000ULL, 0xC000400000018001ULL}, "PackedSignAbs", 1,
   "-mssse3 -ffreestanding"},

  // ===== PABSD — 4 dword lanes; 0x80000000 lane must abs to 0x80000000. =====
  {"pabsd_corner", PABS_FN(int,4,unsigned,_mm_abs_epi32),
   {0xFFFFFFFF80000000ULL, 0x7FFFFFFF00000000ULL}, "PackedSignAbs", 1,
   "-mssse3 -ffreestanding"},

  // ===== PSIGNB — sign lanes span <0 / ==0 / >0; data includes 0x80. =====
  {"psignb_corner", PSIGN_FN(signed char,16,unsigned char,_mm_sign_epi8),
   {0xC040FE017F00FF80ULL, 0xCC3355AA80027E81ULL,
    0x40FF0001807F00FFULL, 0x804000FF01807F00ULL}, "PackedSignAbs", 1,
   "-mssse3 -ffreestanding"},
  {"psignb_mix",    PSIGN_FN(signed char,16,unsigned char,_mm_sign_epi8),
   {0x0102037F80FE8100ULL, 0x7F7E0180FFFE0040ULL,
    0x00FF7F8000017FFEULL, 0xFF000180FF7F0080ULL}, "PackedSignAbs", 1,
   "-mssse3 -ffreestanding"},

  // ===== PSIGNW — word-granular sign select. =====
  {"psignw_corner", PSIGN_FN(short,8,unsigned short,_mm_sign_epi16),
   {0x7FFF0000FFFF8000ULL, 0xC000400000018001ULL,
    0x0001FFFF00008000ULL, 0x7FFF800000010000ULL}, "PackedSignAbs", 1,
   "-mssse3 -ffreestanding"},

  // ===== PSIGND — dword-granular sign select. =====
  {"psignd_corner", PSIGN_FN(int,4,unsigned,_mm_sign_epi32),
   {0xFFFFFFFF80000000ULL, 0x7FFFFFFF00000000ULL,
    0x0000000080000000ULL, 0xFFFFFFFF00000001ULL}, "PackedSignAbs", 1,
   "-mssse3 -ffreestanding"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(PackedSignAbs, X64PackedSignAbsRT,
                         ::testing::ValuesIn(kX64), rtTCName);
