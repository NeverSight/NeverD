//===- X64_SimdNarrowOperandRTTests.cpp - SSE i128-gate roundtrip *- C++ -*-===//
//
// Several x86 SSE intrinsic emitters gate their xmm operands on
// `isIntegerTy(128)` but do not widen first.  When an xmm operand is built from
// a scalar via a zero-extending gpr->xmm move (movq/movd), NeverD's value
// propagation collapses it to the narrow GPR (i64), so the gate fails and the
// INTRINSIC silently falls through to the unhandled-intrinsic 0 — every result
// is wrong.  The AES/shuffle/PCMP emitters already widen with widenToI128; this
// file pins the reachable handlers (PMOVMSKB, PHMINPOSUW, DPPS, MPSADBW and the
// packed shifts PSLL/PSRL/PSRA-by-xmm).  The same widenToI128 fix is applied to
// the SHA new-message/round helpers (emitShaIntrinsic) defensively, but SHA-NI
// cannot be roundtrip-validated here: Unicorn's TCG has no SHA-NI decode/helper
// (documented in the Unicorn unsupported-instructions doc, parallel to GFNI).
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
  "typedef float v4f __attribute__((vector_size(16)));"

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
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(SimdNarrow, X64SimdNarrowRT, ::testing::ValuesIn(kX64),
                         rtTCName);
