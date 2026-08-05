//===- X64_PackedCmpUnordRTTests.cpp - CMPPS/CMPPD UNORD/ORD NaN -*- C++ -*-===//
//
// Packed FP compare with the UNORD (imm 3) and ORD (imm 7) predicates:
//
//   CMPUNORDPS/PD : lane := isNaN(a) || isNaN(b)   (all-ones if unordered)
//   CMPORDPS/PD   : lane := !isNaN(a) && !isNaN(b) (all-ones if ordered)
//
// The lifter built the unordered test from only the FIRST operand:
//   case 3: FLOAT_ISNAN(La);            // should be NaN(La) || NaN(Lb)
//   case 7: FLOAT_ISNAN(La); Neg=true;  // should be !(NaN(La) || NaN(Lb))
// So a NaN living only in the SECOND operand was invisible: the lifted code
// reported "ordered" where the hardware reports "unordered" (and vice-versa for
// ORD).  Both the SSE handler (CMPPS/CMPPD) and the VEX handler (VCMPPS/VCMPPD)
// carried the identical one-operand mistake.
//
// The pre-existing coverage (X64_BlendCmp `cmpps_lt`/`cmppd_ge`, etc.) used
// scalar `fa < fb` C comparisons that clang lowers to UCOMISS/UCOMISD — they
// never reach the packed CMPPS path at all, and none used the UNORD/ORD
// predicates or a NaN-bearing second operand: a textbook weak-test gap.
//
// UNORD/ORD are commutative, so clang is free to choose which vector becomes the
// instruction's first operand; a probe that parks the NaN in one C vector can be
// silently "rescued" when clang happens to map that vector onto the operand the
// buggy lifter DID inspect.  To pin the bug independently of operand selection,
// the discriminating probes place a NaN in a DIFFERENT lane of EACH operand so
// every lane has exactly one NaN: correct UNORD is all-ones / correct ORD is
// all-zero on every lane, whereas a one-operand test can only ever flag the
// (two) lanes whose NaN sits in the inspected operand.  Every result lane's low
// bit is folded into the return (the harness compares only the return value).
// NaNs are built at runtime (0x7FC00000 | payload) so nothing constant-folds.
// All-finite controls (correct ORD = all-ones) guard against an over-correction
// that would break the ordered path.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64PackedCmpUnordRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64PackedCmpUnordRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
#define V4F "typedef float v4f __attribute__((vector_size(16)));\n" \
            "typedef int   v4i __attribute__((vector_size(16)));\n"
#define V2D "typedef double    v2d __attribute__((vector_size(16)));\n" \
            "typedef long long v2q __attribute__((vector_size(16)));\n"

// Fold the low bit of all four single-precision mask lanes into the return.
#define FOLD4 \
  "  return (unsigned)(r[0]&1) + ((unsigned)(r[1]&1)<<1)\n" \
  "       + ((unsigned)(r[2]&1)<<2) + ((unsigned)(r[3]&1)<<3);\n}\n"

// Fold the low bit of both double-precision mask lanes into the return.
#define FOLD2 \
  "  return (unsigned long long)(r[0]&1)\n" \
  "       + (((unsigned long long)(r[1]&1))<<1);\n}\n"

// Runtime quiet-NaN (payload from b) + finite value (from a).
#define MKPS \
  "  float fa, fnan;\n" \
  "  __builtin_memcpy(&fa,&a,4);\n" \
  "  unsigned qn = 0x7FC00000u | (unsigned)(b & 0x1234);\n" \
  "  __builtin_memcpy(&fnan,&qn,4);\n"
#define MKPD \
  "  double da, dnan;\n" \
  "  __builtin_memcpy(&da,&a,8);\n" \
  "  unsigned long long qn = 0x7FF8000000000000ull | (unsigned long long)(b & 0x1234);\n" \
  "  __builtin_memcpy(&dnan,&qn,8);\n"

// va NaN in lanes 0,3 ; vb NaN in lanes 1,2 -> every lane has exactly one NaN.
#define SPLIT4 \
  "  v4f va={fnan,fa+1.0f,fa+2.0f,fnan};\n" \
  "  v4f vb={fa,fnan,fnan,fa+3.0f};\n"
// va NaN in lane 0 ; vb NaN in lane 1 -> every lane has exactly one NaN.
#define SPLIT2 \
  "  v2d va={dnan,da+1.0};\n" \
  "  v2d vb={da,dnan};\n"

#define SSE "", 1, "-msse2 -mno-avx"
#define AVX "", 1, "-mavx -mno-avx2"

static const std::vector<RoundTripTC> kCmp = {

  // ===================== SSE CMPPS / CMPPD ===================================
  {"cmpps_unord_split",
   V4F "long f(long a, long b){\n" MKPS SPLIT4
   "  v4i r=(v4i)__builtin_ia32_cmpunordps(va,vb);\n" FOLD4,
   {0x40000000ULL, 0x12345678ULL}, SSE},

  {"cmpps_ord_split",
   V4F "long f(long a, long b){\n" MKPS SPLIT4
   "  v4i r=(v4i)__builtin_ia32_cmpordps(va,vb);\n" FOLD4,
   {0x40000000ULL, 0x12345678ULL}, SSE},

  // Control: all finite -> ordered everywhere, unordered nowhere.
  {"cmpps_ord_allfinite",
   V4F "long f(long a, long b){\n" MKPS "  (void)fnan;\n"
   "  v4f va={fa,fa+1.0f,fa+2.0f,fa+3.0f};\n"
   "  v4f vb={fa,fa+1.0f,fa+2.0f,fa+3.0f};\n"
   "  v4i r=(v4i)__builtin_ia32_cmpordps(va,vb);\n" FOLD4,
   {0x40000000ULL, 0x12345678ULL}, SSE},

  {"cmppd_unord_split",
   V2D "long f(long a, long b){\n" MKPD SPLIT2
   "  v2q r=(v2q)__builtin_ia32_cmpunordpd(va,vb);\n" FOLD2,
   {0x4010000000000000ULL, 0x12345678ULL}, SSE},

  {"cmppd_ord_split",
   V2D "long f(long a, long b){\n" MKPD SPLIT2
   "  v2q r=(v2q)__builtin_ia32_cmpordpd(va,vb);\n" FOLD2,
   {0x4010000000000000ULL, 0x12345678ULL}, SSE},

  {"cmppd_ord_allfinite",
   V2D "long f(long a, long b){\n" MKPD "  (void)dnan;\n"
   "  v2d va={da,da+1.0};\n"
   "  v2d vb={da,da+1.0};\n"
   "  v2q r=(v2q)__builtin_ia32_cmpordpd(va,vb);\n" FOLD2,
   {0x4010000000000000ULL, 0x12345678ULL}, SSE},

  // ===================== VEX VCMPPS / VCMPPD (same one-operand bug) ==========
  {"vcmpps_unord_split",
   V4F "long f(long a, long b){\n" MKPS SPLIT4
   "  v4i r=(v4i)__builtin_ia32_cmpps(va,vb,3);\n" FOLD4,
   {0x40000000ULL, 0x12345678ULL}, AVX},

  {"vcmpps_ord_split",
   V4F "long f(long a, long b){\n" MKPS SPLIT4
   "  v4i r=(v4i)__builtin_ia32_cmpps(va,vb,7);\n" FOLD4,
   {0x40000000ULL, 0x12345678ULL}, AVX},

  {"vcmppd_unord_split",
   V2D "long f(long a, long b){\n" MKPD SPLIT2
   "  v2q r=(v2q)__builtin_ia32_cmppd(va,vb,3);\n" FOLD2,
   {0x4010000000000000ULL, 0x12345678ULL}, AVX},

  {"vcmppd_ord_split",
   V2D "long f(long a, long b){\n" MKPD SPLIT2
   "  v2q r=(v2q)__builtin_ia32_cmppd(va,vb,7);\n" FOLD2,
   {0x4010000000000000ULL, 0x12345678ULL}, AVX},

  // ===================== Scalar CMPSS / CMPSD (same one-operand bug) ==========
  // The scalar compare keeps a fixed operand order (Dst, Src), so inline asm
  // pins the NaN into the SECOND operand (Src) — the operand the buggy handler
  // ignored.  Clang would otherwise fold a lane-0 read into UCOMISS+SETcc and
  // never emit CMPSS, so the instruction is forced with __asm__.
  {"cmpunordss_src2nan",
   "long f(long a, long b){\n"
   "  unsigned fa=(unsigned)a, fn=0x7FC00000u|((unsigned)b&0x123), out;\n"
   "  __asm__ volatile(\"movd %1,%%xmm0\\n\\tmovd %2,%%xmm1\\n\\t\"\n"
   "    \"cmpunordss %%xmm1,%%xmm0\\n\\tmovd %%xmm0,%0\"\n"
   "    :\"=r\"(out):\"r\"(fa),\"r\"(fn):\"xmm0\",\"xmm1\");\n"
   "  return out&1;\n}\n",
   {0x40000000ULL, 0x12345678ULL}, SSE},

  {"cmpordss_src2nan",
   "long f(long a, long b){\n"
   "  unsigned fa=(unsigned)a, fn=0x7FC00000u|((unsigned)b&0x123), out;\n"
   "  __asm__ volatile(\"movd %1,%%xmm0\\n\\tmovd %2,%%xmm1\\n\\t\"\n"
   "    \"cmpordss %%xmm1,%%xmm0\\n\\tmovd %%xmm0,%0\"\n"
   "    :\"=r\"(out):\"r\"(fa),\"r\"(fn):\"xmm0\",\"xmm1\");\n"
   "  return out&1;\n}\n",
   {0x40000000ULL, 0x12345678ULL}, SSE},

  // Control: NaN in the FIRST operand (the buggy path already handled this).
  {"cmpunordss_src1nan",
   "long f(long a, long b){\n"
   "  unsigned fa=(unsigned)a, fn=0x7FC00000u|((unsigned)b&0x123), out;\n"
   "  __asm__ volatile(\"movd %2,%%xmm0\\n\\tmovd %1,%%xmm1\\n\\t\"\n"
   "    \"cmpunordss %%xmm1,%%xmm0\\n\\tmovd %%xmm0,%0\"\n"
   "    :\"=r\"(out):\"r\"(fa),\"r\"(fn):\"xmm0\",\"xmm1\");\n"
   "  return out&1;\n}\n",
   {0x40000000ULL, 0x12345678ULL}, SSE},

  {"cmpunordsd_src2nan",
   "long f(long a, long b){\n"
   "  unsigned long long da=(unsigned long long)a,\n"
   "    dn=0x7FF8000000000000ull|((unsigned long long)b&0x123), out;\n"
   "  __asm__ volatile(\"movq %1,%%xmm0\\n\\tmovq %2,%%xmm1\\n\\t\"\n"
   "    \"cmpunordsd %%xmm1,%%xmm0\\n\\tmovq %%xmm0,%0\"\n"
   "    :\"=r\"(out):\"r\"(da),\"r\"(dn):\"xmm0\",\"xmm1\");\n"
   "  return (long)(out&1);\n}\n",
   {0x4010000000000000ULL, 0x12345678ULL}, SSE},

  {"cmpordsd_src2nan",
   "long f(long a, long b){\n"
   "  unsigned long long da=(unsigned long long)a,\n"
   "    dn=0x7FF8000000000000ull|((unsigned long long)b&0x123), out;\n"
   "  __asm__ volatile(\"movq %1,%%xmm0\\n\\tmovq %2,%%xmm1\\n\\t\"\n"
   "    \"cmpordsd %%xmm1,%%xmm0\\n\\tmovq %%xmm0,%0\"\n"
   "    :\"=r\"(out):\"r\"(da),\"r\"(dn):\"xmm0\",\"xmm1\");\n"
   "  return (long)(out&1);\n}\n",
   {0x4010000000000000ULL, 0x12345678ULL}, SSE},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(PackedCmpUnord, X64PackedCmpUnordRT,
                         ::testing::ValuesIn(kCmp), rtTCName);
