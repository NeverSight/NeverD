//===- AArch64_RmifRTTests.cpp - RMIF rotate+mask insert NZCV ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for the AArch64 FlagM instruction RMIF (FEAT_FlagM,
// ARMv8.4): "rotate, mask insert flags".  It rotates Xn right by an immediate
// #shift, then inserts the low four bits of the rotated value into NZCV under
// a 4-bit immediate #mask (bit3->N, bit2->Z, bit1->C, bit0->V), leaving the
// flags not selected by the mask unchanged:
//
//   tmp = ROR(Xn, shift);
//   if mask<3> then N = tmp<3>;  if mask<2> then Z = tmp<2>;
//   if mask<1> then C = tmp<1>;  if mask<0> then V = tmp<0>;
//
// The old lifter emitted a bare opaque `A64_Rmif` intrinsic that dropped all
// three operands and set no flags whatsoever (the rotate, mask, and insert
// were entirely lost; the recompiled flags stayed at their entry values).
//
// Probes use *only* RMIF (no preceding CMP) to set the modelled flags, so the
// MedFlags "fold condition back to the nearest CMP" pass has no comparison to
// (mis)match; the flags are read back with `cset mi/eq/cs/vs` folded into
// distinct return bits.  Preservation is exercised by a second RMIF with a
// partial/zero mask.  Select Unicorn's MAX CPU explicitly so FlagM2 (and thus
// FlagM) is present even though Unicorn's default is an A72 model.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64RmifRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64RmifRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kA64 = {
  // rmif a,#0,#15: N=a3, Z=a2, C=a1, V=a0.  a=0xA (1010) -> N1 Z0 C1 V0 => 5.
  {"rmif_all",
   "long f(long a){unsigned int n,z,c,v;"
   "__asm__ volatile(\"rmif %4, #0, #15\\n\\t\""
   "\"cset %w0, mi\\n\\t\"\"cset %w1, eq\\n\\t\"\"cset %w2, cs\\n\\t\"\"cset %w3, vs\""
   ":\"=&r\"(n),\"=&r\"(z),\"=&r\"(c),\"=&r\"(v):\"r\"(a):\"cc\");"
   "return (long)n|((long)z<<1)|((long)c<<2)|((long)v<<3);}\n",
   {0xAULL}, "Rmif", 0, "-march=armv8.4-a", false, "",
   UC_CPU_ARM64_MAX},

  // rmif a,#4,#15: low4 of ROR(a,4) = a[7:4].  a=0x60 -> a[7:4]=0110 ->
  // N0 Z1 C1 V0 => 6.
  {"rmif_shift4",
   "long f(long a){unsigned int n,z,c,v;"
   "__asm__ volatile(\"rmif %4, #4, #15\\n\\t\""
   "\"cset %w0, mi\\n\\t\"\"cset %w1, eq\\n\\t\"\"cset %w2, cs\\n\\t\"\"cset %w3, vs\""
   ":\"=&r\"(n),\"=&r\"(z),\"=&r\"(c),\"=&r\"(v):\"r\"(a):\"cc\");"
   "return (long)n|((long)z<<1)|((long)c<<2)|((long)v<<3);}\n",
   {0x60ULL}, "Rmif", 0, "-march=armv8.4-a", false, "",
   UC_CPU_ARM64_MAX},

  // Mask isolation: rmif a,#0,#15 sets all (a=0x8 -> N1 Z0 C0 V0), then
  // rmif b,#0,#2 sets only C from b1 (b=0x2 -> C=1); N preserved from a.
  // => N1 Z0 C1 V0 = 1|0|4|0 = 5.  (All flags controlled by RMIF, no reliance
  // on the uncontrolled entry NZCV state.)
  {"rmif_mask_c",
   "long f(long a,long b){unsigned int n,z,c,v;"
   "__asm__ volatile(\"rmif %4, #0, #15\\n\\t\"\"rmif %5, #0, #2\\n\\t\""
   "\"cset %w0, mi\\n\\t\"\"cset %w1, eq\\n\\t\"\"cset %w2, cs\\n\\t\"\"cset %w3, vs\""
   ":\"=&r\"(n),\"=&r\"(z),\"=&r\"(c),\"=&r\"(v):\"r\"(a),\"r\"(b):\"cc\");"
   "return (long)n|((long)z<<1)|((long)c<<2)|((long)v<<3);}\n",
   {0x8ULL, 0x2ULL}, "Rmif", 0, "-march=armv8.4-a", false, "",
   UC_CPU_ARM64_MAX},

  // Preserve: rmif a,#0,#15 sets all (a=0xF -> all 1), then rmif b,#0,#8 sets
  // only N from b3 (b=0 -> N=0); Z/C/V preserved as 1 => 0|2|4|8 = 14.
  {"rmif_preserve_n",
   "long f(long a,long b){unsigned int n,z,c,v;"
   "__asm__ volatile(\"rmif %4, #0, #15\\n\\t\"\"rmif %5, #0, #8\\n\\t\""
   "\"cset %w0, mi\\n\\t\"\"cset %w1, eq\\n\\t\"\"cset %w2, cs\\n\\t\"\"cset %w3, vs\""
   ":\"=&r\"(n),\"=&r\"(z),\"=&r\"(c),\"=&r\"(v):\"r\"(a),\"r\"(b):\"cc\");"
   "return (long)n|((long)z<<1)|((long)c<<2)|((long)v<<3);}\n",
   {0xFULL, 0x0ULL}, "Rmif", 0, "-march=armv8.4-a", false, "",
   UC_CPU_ARM64_MAX},

  // Mask = 0 (no-op): rmif a,#0,#15 sets all (a=0x5 -> N0 Z1 C0 V1), then
  // rmif b,#0,#0 changes nothing => 0|2|0|8 = 10.
  {"rmif_mask0_noop",
   "long f(long a,long b){unsigned int n,z,c,v;"
   "__asm__ volatile(\"rmif %4, #0, #15\\n\\t\"\"rmif %5, #0, #0\\n\\t\""
   "\"cset %w0, mi\\n\\t\"\"cset %w1, eq\\n\\t\"\"cset %w2, cs\\n\\t\"\"cset %w3, vs\""
   ":\"=&r\"(n),\"=&r\"(z),\"=&r\"(c),\"=&r\"(v):\"r\"(a),\"r\"(b):\"cc\");"
   "return (long)n|((long)z<<1)|((long)c<<2)|((long)v<<3);}\n",
   {0x5ULL, 0x0ULL}, "Rmif", 0, "-march=armv8.4-a", false, "",
   UC_CPU_ARM64_MAX},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(Rmif, AArch64RmifRT, ::testing::ValuesIn(kA64),
                         rtTCName);
