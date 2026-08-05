//===- ARM32_NEONSubRegAliasRTTests.cpp - S/D/Q sub-reg aliasing -*-C++*-=====//
//
// Adversarial probes for ARM32 VFP/NEON sub-register aliasing — the trickiest of
// the three architectures because the views overlap in a tree: S0/S1 are the two
// halves of D0, and D0/D1 are the two halves of Q0.  Unlike AArch64, a scalar S
// write does NOT zero-extend; it MERGES (the sibling S register / the rest of D0
// is preserved).  If LowToMed mis-models the S<->D<->Q overlap (zero-extends a
// half, or fails to merge a half write into a wide read) the recompiled value
// diverges.  clang never emits these shapes from plain C; only inline asm with
// the NeverD optimizer ON exercises them.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32NEONSubRegRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32NEONSubRegRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kARM = {
  // Writing S0 must PRESERVE its sibling S1 (the high half of D0).
  {"s1_preserve",
   "unsigned f(unsigned a){ unsigned r, b=a^0x9E37u;\n"
   "  __asm__ volatile(\"vmov d0, %1, %1\\n\\tvmov s0, %2\\n\\tvmov %0, s1\"\n"
   "    : \"=r\"(r) : \"r\"(a), \"r\"(b) : \"d0\");\n"
   "  return r; }\n",
   {0x1234ABCDu}, "NEONSubReg32"},

  // Two independent S writes reassembled by a wide D read (half merge).
  {"d_merge",
   "unsigned f(unsigned a){ unsigned lo, hi;\n"
   "  __asm__ volatile(\"vmov s0, %2\\n\\tvmov s1, %3\\n\\tvmov %0, %1, d0\"\n"
   "    : \"=r\"(lo), \"=r\"(hi) : \"r\"(a), \"r\"(a*2654435761u) : \"d0\");\n"
   "  return lo + hi*3u; }\n",
   {0x0BADF00Du}, "NEONSubReg32"},

  // Write whole D0, overwrite only S0, read D0 back (low half changed, high kept).
  {"d_then_s0",
   "unsigned f(unsigned a){ unsigned lo, hi;\n"
   "  __asm__ volatile(\"vmov d0, %2, %3\\n\\tvmov s0, %4\\n\\tvmov %0, %1, d0\"\n"
   "    : \"=r\"(lo), \"=r\"(hi) : \"r\"(a), \"r\"(~a), \"r\"(a>>5) : \"d0\");\n"
   "  return lo ^ (hi<<1); }\n",
   {0xCAFEBEEFu}, "NEONSubReg32"},

  // Q0 = (D1:D0): write D0 and D1, read both halves of Q0 back.
  {"q_via_d",
   "unsigned f(unsigned a){ unsigned r0,r1,r2,r3;\n"
   "  __asm__ volatile(\"vmov d0, %4, %5\\n\\tvmov d1, %5, %4\\n\\t\"\n"
   "    \"vmov %0,%1, d0\\n\\tvmov %2,%3, d1\"\n"
   "    : \"=r\"(r0),\"=r\"(r1),\"=r\"(r2),\"=r\"(r3)\n"
   "    : \"r\"(a), \"r\"(a^0x55AA55AAu) : \"d0\",\"d1\");\n"
   "  return r0 + r1 + r2 + r3; }\n",
   {0x33445566u}, "NEONSubReg32"},

  // Write S registers spanning D0+D1 (Q0), read whole Q0 back as four words.
  {"q_lane_build",
   "unsigned f(unsigned a){ unsigned r0,r1,r2,r3;\n"
   "  __asm__ volatile(\"vmov s0, %4\\n\\tvmov s1, %5\\n\\t\"\n"
   "    \"vmov s2, %5\\n\\tvmov s3, %4\\n\\t\"\n"
   "    \"vmov %0,%1, d0\\n\\tvmov %2,%3, d1\"\n"
   "    : \"=r\"(r0),\"=r\"(r1),\"=r\"(r2),\"=r\"(r3)\n"
   "    : \"r\"(a), \"r\"(a+0x1000u) : \"d0\",\"d1\");\n"
   "  return (r0^r3) + (r1^r2); }\n",
   {0x778899AAu}, "NEONSubReg32"},

  // .32 lane write into D0[1] merges with an existing D0[0] (NEON lane move).
  {"lane32_merge",
   "unsigned f(unsigned a){ unsigned lo, hi;\n"
   "  __asm__ volatile(\"vmov d0, %2, %2\\n\\tvmov.32 d0[1], %3\\n\\t\"\n"
   "    \"vmov %0, %1, d0\"\n"
   "    : \"=r\"(lo), \"=r\"(hi) : \"r\"(a), \"r\"(a^0x0F0F0F0Fu) : \"d0\");\n"
   "  return lo + (hi^a); }\n",
   {0x12FE34DCu}, "NEONSubReg32"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(NEONSubReg32, ARM32NEONSubRegRT,
                         ::testing::ValuesIn(kARM), rtTCName);
