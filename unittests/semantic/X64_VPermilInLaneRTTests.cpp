//===- X64_VPermilInLaneRTTests.cpp - VPERMILPS/PD imm in-lane permute -*-C++-===//
//
// AVX VPERMILPS / VPERMILPD are IN-LANE float/double permutes: each 32-bit
// (PS) or 64-bit (PD) result element is chosen from the source elements WITHIN
// the same 128-bit lane.  The immediate form:
//
//   VPERMILPS xmm,xmm,imm8 : element i := src[(imm >> (2*i)) & 3]
//   VPERMILPD xmm,xmm,imm8 : element i := src[(imm >> i) & 1]
//
// The lifter was rewritten (#342) to actually honour the control: the imm form
// now emits constant-index SUBBYTES assembled low->high with a CONCAT tree.
// Before #342 it COPYed the source and dropped the permutation entirely, so
// every non-identity control silently returned the source unpermuted.
//
// The ONLY pre-existing roundtrip coverage (X64_BlendCmp `vpermilps_reverse` /
// `vpermilpd_swap`) drives a single control each and reads exactly ONE result
// element — a classic weak-test gap: a transposed lane, a bad selector shift,
// or the wrong masking width on any of the OTHER lanes would be invisible.
//
// These probes drive a spread of controls (reverse / broadcast / identity /
// duplicate, and ALL FOUR PD imm-bit combinations) and fold EVERY result lane
// into the return value (the harness compares only the return register), so any
// mis-routed lane is observed.  The source elements are four/two DISTINCT
// runtime-seeded values so clang cannot fold the vector away.
//
// Scope note — the VARIABLE (register-control) forms (VPERMILPS/PD xmm,xmm,xmm,
// the bulk of the #342 rewrite: per-element selector extraction + dynamic
// in-lane gather) CANNOT be roundtripped here: the bundled Unicorn fork does
// not decode the VEX.128 0F38 0C/0D variable opcodes (a bare `vpermilps
// xmm,xmm,xmm` raises UC_ERR_INSN_INVALID on the ORIGINAL program), the same
// decode gap it has for 256-bit AVX (cf. #341/#342).  That path stays lift-only
// verified.  128-bit imm forms ARE decoded, so this is a pure lift-coverage
// round.  Built with `-mavx -mno-avx2` to pin the VEX.128 encodings.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VPermilInLaneRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VPermilInLaneRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
#define V4F "typedef float v4f __attribute__((vector_size(16)));\n"
#define V2D "typedef double v2d __attribute__((vector_size(16)));\n"

// Fold all four single-precision result lanes into the return value.
#define FOLD4 \
  "  unsigned o0,o1,o2,o3; float t;\n" \
  "  t=vr[0];__builtin_memcpy(&o0,&t,4); t=vr[1];__builtin_memcpy(&o1,&t,4);\n" \
  "  t=vr[2];__builtin_memcpy(&o2,&t,4); t=vr[3];__builtin_memcpy(&o3,&t,4);\n" \
  "  return (unsigned long)(o0*7u+o1*13u+o2*17u+o3*23u);\n}\n"

// Fold both double-precision result lanes into the return value.
#define FOLD2 \
  "  unsigned long o0,o1; double t;\n" \
  "  t=vr[0];__builtin_memcpy(&o0,&t,8); t=vr[1];__builtin_memcpy(&o1,&t,8);\n" \
  "  return o0*1000003ul + o1*99u;\n}\n"

static const std::vector<RoundTripTC> kX64 = {

  // ============================ VPERMILPS — imm form (all lanes folded) ======
  // Data is {fa, fa+1, fa+2, fa+3} (four distinct runtime values), so any lane
  // mis-routing changes the folded result.

  {"vpermilps_imm_reverse",   // 0x1B -> {src3,src2,src1,src0}
   V4F
   "long f(long a){\n"
   "  float fa; __builtin_memcpy(&fa,&a,4);\n"
   "  v4f va={fa,fa+1.0f,fa+2.0f,fa+3.0f};\n"
   "  v4f vr=__builtin_ia32_vpermilps(va,0x1B);\n"
   FOLD4,
   {0x40A00000ULL}, "VPermil", 1, "-mavx -mno-avx2"},

  {"vpermilps_imm_bcast0",    // 0x00 -> {src0,src0,src0,src0}
   V4F
   "long f(long a){\n"
   "  float fa; __builtin_memcpy(&fa,&a,4);\n"
   "  v4f va={fa,fa+1.0f,fa+2.0f,fa+3.0f};\n"
   "  v4f vr=__builtin_ia32_vpermilps(va,0x00);\n"
   FOLD4,
   {0x42C80000ULL}, "VPermil", 1, "-mavx -mno-avx2"},

  {"vpermilps_imm_identity",  // 0xE4 -> {src0,src1,src2,src3}
   V4F
   "long f(long a){\n"
   "  float fa; __builtin_memcpy(&fa,&a,4);\n"
   "  v4f va={fa,fa+1.0f,fa+2.0f,fa+3.0f};\n"
   "  v4f vr=__builtin_ia32_vpermilps(va,0xE4);\n"
   FOLD4,
   {0x3F800000ULL}, "VPermil", 1, "-mavx -mno-avx2"},

  {"vpermilps_imm_dup_evens", // 0xA0 -> {src0,src0,src2,src2}
   V4F
   "long f(long a){\n"
   "  float fa; __builtin_memcpy(&fa,&a,4);\n"
   "  v4f va={fa,fa+1.0f,fa+2.0f,fa+3.0f};\n"
   "  v4f vr=__builtin_ia32_vpermilps(va,0xA0);\n"
   FOLD4,
   {0x41200000ULL}, "VPermil", 1, "-mavx -mno-avx2"},

  {"vpermilps_imm_dup_odds",  // 0xF5 -> {src1,src1,src3,src3}
   V4F
   "long f(long a){\n"
   "  float fa; __builtin_memcpy(&fa,&a,4);\n"
   "  v4f va={fa,fa+1.0f,fa+2.0f,fa+3.0f};\n"
   "  v4f vr=__builtin_ia32_vpermilps(va,0xF5);\n"
   FOLD4,
   {0x42F60000ULL}, "VPermil", 1, "-mavx -mno-avx2"},

  // ============================ VPERMILPD — imm form (all four bit combos) ====
  {"vpermilpd_imm_00",        // 0x0 -> {src0,src0}
   V2D
   "long f(long a, long b){\n"
   "  double da,db; __builtin_memcpy(&da,&a,8); __builtin_memcpy(&db,&b,8);\n"
   "  v2d va={da,db};\n"
   "  v2d vr=__builtin_ia32_vpermilpd(va,0x0);\n"
   FOLD2,
   {0x4010000000000000ULL, 0x4008000000000000ULL}, "VPermil", 1, "-mavx -mno-avx2"},

  {"vpermilpd_imm_01",        // 0x1 -> {src1,src0} (swap)
   V2D
   "long f(long a, long b){\n"
   "  double da,db; __builtin_memcpy(&da,&a,8); __builtin_memcpy(&db,&b,8);\n"
   "  v2d va={da,db};\n"
   "  v2d vr=__builtin_ia32_vpermilpd(va,0x1);\n"
   FOLD2,
   {0x4021000000000000ULL, 0x4037000000000000ULL}, "VPermil", 1, "-mavx -mno-avx2"},

  {"vpermilpd_imm_02",        // 0x2 -> {src0,src1} (identity)
   V2D
   "long f(long a, long b){\n"
   "  double da,db; __builtin_memcpy(&da,&a,8); __builtin_memcpy(&db,&b,8);\n"
   "  v2d va={da,db};\n"
   "  v2d vr=__builtin_ia32_vpermilpd(va,0x2);\n"
   FOLD2,
   {0x4047000000000000ULL, 0x405EC00000000000ULL}, "VPermil", 1, "-mavx -mno-avx2"},

  {"vpermilpd_imm_03",        // 0x3 -> {src1,src1}
   V2D
   "long f(long a, long b){\n"
   "  double da,db; __builtin_memcpy(&da,&a,8); __builtin_memcpy(&db,&b,8);\n"
   "  v2d va={da,db};\n"
   "  v2d vr=__builtin_ia32_vpermilpd(va,0x3);\n"
   FOLD2,
   {0x4000000000000000ULL, 0x4052000000000000ULL}, "VPermil", 1, "-mavx -mno-avx2"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(VPermil, X64VPermilInLaneRT,
                         ::testing::ValuesIn(kX64), rtTCName);
