//===- X64_SimdNarrowOperandRTTests.cpp - SSE i128-gate roundtrip *- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Several x86 SSE intrinsic emitters gate their xmm operands on
// `isIntegerTy(128)` but do not widen first.  When an xmm operand is built from
// a scalar via a zero-extending gpr->xmm move (movq/movd), NeverD's value
// propagation collapses it to the narrow GPR (i64), so the gate fails and the
// INTRINSIC silently falls through to the unhandled-intrinsic 0 — every result
// is wrong.  The AES/shuffle/PCMP emitters already widen with widenToI128; this
// file pins the reachable handlers (PMOVMSKB, PHMINPOSUW, DPPS, MPSADBW and the
// packed shifts PSLL/PSRL/PSRA-by-xmm).  SHA-NI round and message helpers are
// also covered now that the bundled Unicorn backend has complete SHA-NI
// decoding and instruction semantics.
//
// Each probe builds the xmm operand(s) from the function arguments with a movq
// ((v..)(v2){a,0}) so the narrow-propagation path is exercised, then folds the
// instruction result into the return value.  All instructions are native on the
// default (Haswell) Unicorn CPU, so these are pure emitter fixes.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SimdNarrowRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SimdNarrowRT, Verify) { roundTripX64(GetParam()); }

#define VPRO                                                                    \
  "typedef char v16 __attribute__((vector_size(16)));"                         \
  "typedef short v8 __attribute__((vector_size(16)));"                         \
  "typedef int v4 __attribute__((vector_size(16)));"                           \
  "typedef long long v2 __attribute__((vector_size(16)));"                     \
  "typedef unsigned long long u2 __attribute__((vector_size(16)));"            \
  "typedef float v4f __attribute__((vector_size(16)));"

#define FOLDSHA                                                                 \
  "u2 q=(u2)r;return (long)(q[0]*1000003ULL+q[1]);}"

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // PMOVMSKB: byte-mask of sign bits -> GPR.
  {"pmovmskb",
   VPRO
   "long f(long a){v16 x=(v16)(v2){a,0};return __builtin_ia32_pmovmskb128(x);}\n",
   {0x80FF007F88010203LL}, "SimdNarrow", 1, "-msse4.2"},

  // PHMINPOSUW: horizontal min of 8 u16 lanes + its index -> low lanes.
  {"phminposuw",
   VPRO
   "long f(long a){v8 x=(v8)(v2){a,0};"
   "v8 r=__builtin_ia32_phminposuw128(x);return ((v2)r)[0];}\n",
   {0x0005000300080001LL}, "SimdNarrow", 1, "-msse4.2"},

  // MPSADBW: sum-of-absolute-differences windows (imm selects offsets).
  {"mpsadbw",
   VPRO
   "long f(long a,long b){v16 x=(v16)(v2){a,0},y=(v16)(v2){b,0};"
   "v16 r=(v16)__builtin_ia32_mpsadbw128(x,y,0);return ((v2)r)[0];}\n",
   {0x1020304050607080LL, 0x0102030405060708LL}, "SimdNarrow", 1, "-msse4.2"},

  // DPPS: packed-single dot product (imm 0xF1 = all 4 lanes, result -> lane 0).
  {"dpps",
   VPRO
   "long f(long a,long b){v4f x=(v4f)(v2){a,0},y=(v4f)(v2){b,0};"
   "v4 r=(v4)__builtin_ia32_dpps(x,y,0xF1);return r[0];}\n",
   {0x3F8000003F800000LL, 0x4000000040400000LL}, "SimdNarrow", 1, "-msse4.2"},

  // PSLLD by xmm count (count = low 64 bits of the second operand).
  {"pslld_xmm",
   VPRO
   "long f(long a,long b){v4 x=(v4)(v2){a,0},n=(v4)(v2){b,0};"
   "v4 r=__builtin_ia32_pslld128(x,n);return ((v2)r)[0];}\n",
   {0x0000000100000002LL, 4}, "SimdNarrow", 1, "-msse4.2"},

  // PSRLW by xmm count.
  {"psrlw_xmm",
   VPRO
   "long f(long a,long b){v8 x=(v8)(v2){a,0},n=(v8)(v2){b,0};"
   "v8 r=__builtin_ia32_psrlw128(x,n);return ((v2)r)[0];}\n",
   {0x8000400020001000LL, 2}, "SimdNarrow", 1, "-msse4.2"},

  // PSRAD by xmm count (arithmetic, sign-preserving).
  {"psrad_xmm",
   VPRO
   "long f(long a,long b){v4 x=(v4)(v2){a,0},n=(v4)(v2){b,0};"
   "v4 r=__builtin_ia32_psrad128(x,n);return ((v2)r)[0];}\n",
   {0x80000000FFFFFFF0LL, 3}, "SimdNarrow", 1, "-msse4.2"},

  // PSLLQ by xmm count.
  {"psllq_xmm",
   VPRO
   "long f(long a,long b){v2 x=(v2){a,0},n=(v2){b,0};"
   "v2 r=__builtin_ia32_psllq128(x,n);return r[0];}\n",
   {0x0000000000000003LL, 5}, "SimdNarrow", 1, "-msse4.2"},

  // SHA-NI: every result dword is folded through both qwords.  Inputs are
  // assembled from runtime arguments so the instruction cannot be folded out.
  {"sha1rnds4",
   VPRO
   "long f(long a,long b,long c,long d){v4 x=(v4)(v2){a,b},y=(v4)(v2){c,d};"
   "v4 r=__builtin_ia32_sha1rnds4(x,y,2);" FOLDSHA,
   {0x89ABCDEF01234567ULL, 0x76543210FEDCBA98ULL,
    0x4B5A69780F1E2D3CULL, 0xC3D2E1F08796A5B4ULL},
   "SimdNarrow", 1, "-msha -msse4.2", false, "", UC_CPU_X86_DENVERTON},

  {"sha1nexte",
   VPRO
   "long f(long a,long b,long c,long d){v4 x=(v4)(v2){a,b},y=(v4)(v2){c,d};"
   "v4 r=__builtin_ia32_sha1nexte(x,y);" FOLDSHA,
   {0x89ABCDEF01234567ULL, 0x76543210FEDCBA98ULL,
    0x4B5A69780F1E2D3CULL, 0xC3D2E1F08796A5B4ULL},
   "SimdNarrow", 1, "-msha -msse4.2", false, "", UC_CPU_X86_DENVERTON},

  {"sha1msg1",
   VPRO
   "long f(long a,long b,long c,long d){v4 x=(v4)(v2){a,b},y=(v4)(v2){c,d};"
   "v4 r=__builtin_ia32_sha1msg1(x,y);" FOLDSHA,
   {0x89ABCDEF01234567ULL, 0x76543210FEDCBA98ULL,
    0x4B5A69780F1E2D3CULL, 0xC3D2E1F08796A5B4ULL},
   "SimdNarrow", 1, "-msha -msse4.2", false, "", UC_CPU_X86_DENVERTON},

  {"sha1msg2",
   VPRO
   "long f(long a,long b,long c,long d){v4 x=(v4)(v2){a,b},y=(v4)(v2){c,d};"
   "v4 r=__builtin_ia32_sha1msg2(x,y);" FOLDSHA,
   {0x89ABCDEF01234567ULL, 0x76543210FEDCBA98ULL,
    0x4B5A69780F1E2D3CULL, 0xC3D2E1F08796A5B4ULL},
   "SimdNarrow", 1, "-msha -msse4.2", false, "", UC_CPU_X86_DENVERTON},

  {"sha256rnds2",
   VPRO
   "long f(long a,long b,long c,long d,long e,long g){"
   "v4 x=(v4)(v2){a,b},y=(v4)(v2){c,d},z=(v4)(v2){e,g};"
   "v4 r=__builtin_ia32_sha256rnds2(x,y,z);" FOLDSHA,
   {0x89ABCDEF01234567ULL, 0x76543210FEDCBA98ULL,
    0x4B5A69780F1E2D3CULL, 0xC3D2E1F08796A5B4ULL,
    0x5060708010203040ULL, 0},
   "SimdNarrow", 1, "-msha -msse4.2", false, "", UC_CPU_X86_DENVERTON},

  {"sha256msg1",
   VPRO
   "long f(long a,long b,long c,long d){v4 x=(v4)(v2){a,b},y=(v4)(v2){c,d};"
   "v4 r=__builtin_ia32_sha256msg1(x,y);" FOLDSHA,
   {0x89ABCDEF01234567ULL, 0x76543210FEDCBA98ULL,
    0x4B5A69780F1E2D3CULL, 0xC3D2E1F08796A5B4ULL},
   "SimdNarrow", 1, "-msha -msse4.2", false, "", UC_CPU_X86_DENVERTON},

  {"sha256msg2",
   VPRO
   "long f(long a,long b,long c,long d){v4 x=(v4)(v2){a,b},y=(v4)(v2){c,d};"
   "v4 r=__builtin_ia32_sha256msg2(x,y);" FOLDSHA,
   {0x89ABCDEF01234567ULL, 0x76543210FEDCBA98ULL,
    0x4B5A69780F1E2D3CULL, 0xC3D2E1F08796A5B4ULL},
   "SimdNarrow", 1, "-msha -msse4.2", false, "", UC_CPU_X86_DENVERTON},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(SimdNarrow, X64SimdNarrowRT, ::testing::ValuesIn(kX64),
                         rtTCName);
