//===- AArch64_XAFlagRTTests.cpp - XAFLAG/AXFLAG flag convert ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for the AArch64 FlagM2 condition-flag conversion
// instructions XAFLAG and AXFLAG (FEAT_FlagM2, ARMv8.5).  They convert NZCV
// between the Arm and the "alternative" (JavaScript-style) floating-point
// comparison encodings, depending only on C and Z (AXFLAG also on V):
//
//   AXFLAG (Arm -> alternative):
//     N = 0;  Z = Z OR V;  C = C AND NOT V;  V = 0
//   XAFLAG (alternative -> Arm):
//     N = NOT C AND NOT Z;  Z = C AND Z;  C = C OR Z;  V = NOT C AND Z
//
// The old lifter emitted a bare opaque `A64_Xaflag`/`A64_Axflag` intrinsic.
// Since both take no operands, the bare inline-asm assembles fine — but it
// mutates the *hardware* NZCV, which is disconnected from NeverD's modelled
// flag registers (NFLAG/ZFLAG/CFLAG/VFLAG).  So in the recompiled image the
// transform never reaches the modelled flags: a flag producer (`cmp`) and
// consumer (`cset`) that both go through the model see the *untransformed*
// flags.  The fix models the conversion directly with boolean ops.
//
// Each probe sets a known NZCV with `cmp` (a modelled producer), runs
// xaflag/axflag, then reads N/Z/C/V back with `cset mi/eq/cs/vs` (modelled
// consumers) folded into distinct return bits.  Select Unicorn's MAX CPU
// explicitly so FlagM2 is present; Unicorn's default is an A72 model.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64XAFlagRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64XAFlagRT, Verify) { roundTripAArch64(GetParam()); }

// cmp x0,x1 sets NZCV from (a - b); the named op transforms it; cset reads
// N(mi)/Z(eq)/C(cs)/V(vs) back into bits 0..3.
#define AXFLAG_FN \
  "long f(long a,long b){unsigned int n,z,c,v;" \
  "__asm__ volatile(\"cmp %4,%5\\n\\t\"\"axflag\\n\\t\"" \
  "\"cset %w0, mi\\n\\t\"\"cset %w1, eq\\n\\t\"\"cset %w2, cs\\n\\t\"\"cset %w3, vs\"" \
  ":\"=&r\"(n),\"=&r\"(z),\"=&r\"(c),\"=&r\"(v):\"r\"(a),\"r\"(b):\"cc\");" \
  "return (long)n|((long)z<<1)|((long)c<<2)|((long)v<<3);}\n"

#define XAFLAG_FN \
  "long f(long a,long b){unsigned int n,z,c,v;" \
  "__asm__ volatile(\"cmp %4,%5\\n\\t\"\"xaflag\\n\\t\"" \
  "\"cset %w0, mi\\n\\t\"\"cset %w1, eq\\n\\t\"\"cset %w2, cs\\n\\t\"\"cset %w3, vs\"" \
  ":\"=&r\"(n),\"=&r\"(z),\"=&r\"(c),\"=&r\"(v):\"r\"(a),\"r\"(b):\"cc\");" \
  "return (long)n|((long)z<<1)|((long)c<<2)|((long)v<<3);}\n"

// clang-format off
static const std::vector<RoundTripTC> kA64 = {
  // ---- AXFLAG: N=0, Z=Z|V, C=C&~V, V=0 ----
  // cmp(INT64_MIN, 1): N=0,Z=0,C=1,V=1.  -> N0,Z=0|1=1,C=1&0=0,V=0 => 0b0010=2.
  // (old/buggy: untransformed 0b0100=4)
  {"ax_v1",    AXFLAG_FN, {0x8000000000000000ULL, 1ULL}, "XAFlag", 0,
   "-march=armv8.5-a", false, "", UC_CPU_ARM64_MAX},
  // cmp(-1, 1): N=1,Z=0,C=1,V=0.  -> N0,Z=0,C=1&1=1,V=0 => 0b0100=4.  (old: 5)
  {"ax_n1c1",  AXFLAG_FN, {0xFFFFFFFFFFFFFFFFULL, 1ULL}, "XAFlag", 0,
   "-march=armv8.5-a", false, "", UC_CPU_ARM64_MAX},
  // Control: cmp(7,7): N=0,Z=1,C=1,V=0.  V=0 so transform == identity here
  // (N already 0) => 0b0110=6, matches old.  Passes RED and GREEN.
  {"ax_z1c1",  AXFLAG_FN, {7ULL, 7ULL}, "XAFlag", 0,
   "-march=armv8.5-a", false, "", UC_CPU_ARM64_MAX},

  // ---- XAFLAG: N=!C&!Z, Z=C&Z, C=C|Z, V=!C&Z ----
  // cmp(-1, 1): N=1,Z=0,C=1,V=0. -> N=!1&!0=0,Z=0,C=1,V=0 => 0b0100=4. (old:5)
  {"xa_c1z0",  XAFLAG_FN, {0xFFFFFFFFFFFFFFFFULL, 1ULL}, "XAFlag", 0,
   "-march=armv8.5-a", false, "", UC_CPU_ARM64_MAX},
  // cmp(0, INT64_MIN+1): N=0,Z=0,C=0,V=0. -> N=!0&!0=1,Z=0,C=0,V=0 => 1. (old:0)
  {"xa_c0z0",  XAFLAG_FN, {0ULL, 0x8000000000000001ULL}, "XAFlag", 0,
   "-march=armv8.5-a", false, "", UC_CPU_ARM64_MAX},
  // Control: cmp(3,3): N=0,Z=1,C=1,V=0. -> N=0,Z=1,C=1,V=0 => 0b0110=6.
  // Matches old (untransformed also 0b0110=6).  Passes RED and GREEN.
  {"xa_c1z1",  XAFLAG_FN, {3ULL, 3ULL}, "XAFlag", 0,
   "-march=armv8.5-a", false, "", UC_CPU_ARM64_MAX},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(XAFlag, AArch64XAFlagRT, ::testing::ValuesIn(kA64),
                         rtTCName);
