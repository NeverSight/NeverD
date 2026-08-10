//===- AArch64_NEONSubRegAliasRTTests.cpp - V-reg sub-reg aliasing -*-C++*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Adversarial probes for AArch64 SIMD/FP sub-register aliasing — the NEON analog
// of the x86 RAX/AL partial-register family that bit hard historically.  The key
// AArch64 quirk: a SCALAR write to Sn (32-bit) or Dn (64-bit) ZERO-EXTENDS the
// rest of the 128-bit Vn (clears the upper bits), while an `ins Vn.x[i]` lane
// write MERGES (preserves the other lanes).  If LowToMed models a scalar S/D
// write as a merge (or a lane write as a zero-extend) the recompiled value
// diverges.  clang never emits these shapes from plain C, so only inline asm
// with the NeverD optimizer ON exercises them.  Result is moved back to a GP
// register so the integer-return harness can compare it.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64NEONSubRegRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64NEONSubRegRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kA64 = {
  // fmov Sn zero-extends: V0 starts all-ones, S0 write must clear bits [127:32].
  {"s_zext",
   "unsigned long f(unsigned long a){ unsigned long r;\n"
   "  __asm__ volatile(\"movi v0.2d, #0xffffffffffffffff\\n\\t\"\n"
   "    \"fmov s0, %w1\\n\\tfmov %x0, d0\" : \"=r\"(r) : \"r\"(a) : \"v0\");\n"
   "  return r; }\n",
   {0xAABBCCDD11223344ULL}, "NEONSubReg"},

  // fmov Dn zero-extends the UPPER 64 bits: read V0.d[1], must be 0.
  {"d_zext_hi",
   "unsigned long f(unsigned long a){ unsigned long r;\n"
   "  __asm__ volatile(\"movi v0.2d, #0xffffffffffffffff\\n\\t\"\n"
   "    \"fmov d0, %x1\\n\\tumov %0, v0.d[1]\" : \"=r\"(r) : \"r\"(a) : \"v0\");\n"
   "  return r; }\n",
   {0x1122334455667788ULL}, "NEONSubReg"},

  // ins Vn.s[1] MERGES: lane 0 preserved (0), lane 1 gets low 32 of a.
  {"ins_lane_merge",
   "unsigned long f(unsigned long a){ unsigned long r;\n"
   "  __asm__ volatile(\"movi v0.2d, #0\\n\\t\"\n"
   "    \"ins v0.s[1], %w1\\n\\tfmov %x0, d0\" : \"=r\"(r) : \"r\"(a) : \"v0\");\n"
   "  return r; }\n",
   {0xDEADBEEFCAFEF00DULL}, "NEONSubReg"},

  // ins Vn.s[0] over an all-ones D0 preserves lane 1 (upper 32 stays 0xffffffff).
  {"ins_lane0_keep",
   "unsigned long f(unsigned long a){ unsigned long r;\n"
   "  __asm__ volatile(\"movi v0.2d, #0xffffffffffffffff\\n\\t\"\n"
   "    \"ins v0.s[0], %w1\\n\\tfmov %x0, d0\" : \"=r\"(r) : \"r\"(a) : \"v0\");\n"
   "  return r; }\n",
   {0x0102030405060708ULL}, "NEONSubReg"},

  // Scalar D write then read S0 (low 32) via umov — narrow view of fresh D.
  {"d_then_s",
   "unsigned long f(unsigned long a){ unsigned long r;\n"
   "  __asm__ volatile(\"movi v0.2d, #0xffffffffffffffff\\n\\t\"\n"
   "    \"fmov d0, %x1\\n\\tumov %w0, v0.s[1]\" : \"=r\"(r) : \"r\"(a) : \"v0\");\n"
   "  return r; }\n",
   {0x99887766AABBCCDDULL}, "NEONSubReg"},

  // Build V0 with two lane writes, read each lane back and combine (lane merge
  // chain — no zero-extend should leak across the lanes).
  {"lane_build",
   "unsigned long f(unsigned long a){ unsigned long r;\n"
   "  __asm__ volatile(\"movi v0.2d, #0\\n\\t\"\n"
   "    \"ins v0.s[0], %w1\\n\\tlsr %x1, %x1, #13\\n\\tins v0.s[1], %w1\\n\\t\"\n"
   "    \"fmov %x0, d0\" : \"=r\"(r), \"+r\"(a) : : \"v0\");\n"
   "  return r; }\n",
   {0x123456789ABCDEF0ULL}, "NEONSubReg"},

  // fmov Sn (zero-extends V), then ins lane 1 (merge) — mixed zext + merge.
  {"s_zext_then_ins",
   "unsigned long f(unsigned long a){ unsigned long r;\n"
   "  __asm__ volatile(\"movi v0.2d, #0xffffffffffffffff\\n\\t\"\n"
   "    \"fmov s0, %w1\\n\\tlsr %x1,%x1,#7\\n\\tins v0.s[1], %w1\\n\\t\"\n"
   "    \"fmov %x0, d0\" : \"=r\"(r), \"+r\"(a) : : \"v0\");\n"
   "  return r; }\n",
   {0xF0E0D0C0B0A09080ULL}, "NEONSubReg"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(NEONSubReg, A64NEONSubRegRT,
                         ::testing::ValuesIn(kA64), rtTCName);
