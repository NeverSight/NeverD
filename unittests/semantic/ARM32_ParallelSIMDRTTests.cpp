//===- ARM32_ParallelSIMDRTTests.cpp - GE-setting parallel SIMD + SEL ------===//
//
// Roundtrip probes for the ARMv6 GE-setting parallel (SIMD-in-GPR) add/sub
// instructions and SEL.  SADD8/UADD8/SSUB8/.../SADD16/.../SASX set the four
// APSR.GE[3:0] lane flags; SEL then picks each result byte from the first or
// second source according to its GE flag.  The lifter had no GE-flag model: the
// GE-setting ops dropped the flags and SEL emitted an unhandled intrinsic that
// always returned 0, so every `uadd8;sel`-style sequence was wrong.
//
// Driven via inline asm so the exact opcode is exercised.  ARM32 only (these are
// AArch32 media instructions with no AArch64/x86 equivalent).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32ParallelRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32ParallelRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kArm32 = {
  // UADD8 sets GE[i] = carry of byte i; SEL picks Rn.byte where GE, else Rm.byte.
  {"arm_uadd8_sel",
   "unsigned arm_uadd8_sel(unsigned a, unsigned b){ unsigned r;"
   " __asm__ volatile(\"uadd8 r4,%1,%2\\nsel %0,%1,%2\\n\" : \"=&r\"(r) : \"r\"(a),\"r\"(b) : \"r4\",\"cc\");"
   " return r; }\n",
   {0x807F01FEu, 0x807F0102u}, "Parallel", 1, ""},
  // SADD8 sets GE[i] = (signed sum byte i >= 0).
  {"arm_sadd8_sel",
   "unsigned arm_sadd8_sel(unsigned a, unsigned b){ unsigned r;"
   " __asm__ volatile(\"sadd8 r4,%1,%2\\nsel %0,%1,%2\\n\" : \"=&r\"(r) : \"r\"(a),\"r\"(b) : \"r4\",\"cc\");"
   " return r; }\n",
   {0x7F800140u, 0x017F0220u}, "Parallel", 1, ""},
  // USUB8 sets GE[i] = (Rn.byte >= Rm.byte) (no borrow).
  {"arm_usub8_sel",
   "unsigned arm_usub8_sel(unsigned a, unsigned b){ unsigned r;"
   " __asm__ volatile(\"usub8 r4,%1,%2\\nsel %0,%1,%2\\n\" : \"=&r\"(r) : \"r\"(a),\"r\"(b) : \"r4\",\"cc\");"
   " return r; }\n",
   {0x10FF2030u, 0x20102040u}, "Parallel", 1, ""},
  // SSUB8 sets GE[i] = (signed diff byte i >= 0).
  {"arm_ssub8_sel",
   "unsigned arm_ssub8_sel(unsigned a, unsigned b){ unsigned r;"
   " __asm__ volatile(\"ssub8 r4,%1,%2\\nsel %0,%1,%2\\n\" : \"=&r\"(r) : \"r\"(a),\"r\"(b) : \"r4\",\"cc\");"
   " return r; }\n",
   {0x7F00807Fu, 0x017F0110u}, "Parallel", 1, ""},
  // UADD16 sets GE[1:0]/GE[3:2] per halfword carry.
  {"arm_uadd16_sel",
   "unsigned arm_uadd16_sel(unsigned a, unsigned b){ unsigned r;"
   " __asm__ volatile(\"uadd16 r4,%1,%2\\nsel %0,%1,%2\\n\" : \"=&r\"(r) : \"r\"(a),\"r\"(b) : \"r4\",\"cc\");"
   " return r; }\n",
   {0xFFFF0001u, 0x00020001u}, "Parallel", 1, ""},
  // SADD16 sets GE per halfword (signed sum >= 0).
  {"arm_sadd16_sel",
   "unsigned arm_sadd16_sel(unsigned a, unsigned b){ unsigned r;"
   " __asm__ volatile(\"sadd16 r4,%1,%2\\nsel %0,%1,%2\\n\" : \"=&r\"(r) : \"r\"(a),\"r\"(b) : \"r4\",\"cc\");"
   " return r; }\n",
   {0x80000100u, 0x01007FFFu}, "Parallel", 1, ""},
  // SASX (exchange): low lane sub, high lane add; GE follows each lane.
  {"arm_sasx_sel",
   "unsigned arm_sasx_sel(unsigned a, unsigned b){ unsigned r;"
   " __asm__ volatile(\"sasx r4,%1,%2\\nsel %0,%1,%2\\n\" : \"=&r\"(r) : \"r\"(a),\"r\"(b) : \"r4\",\"cc\");"
   " return r; }\n",
   {0x00207FFFu, 0x7FFF0010u}, "Parallel", 1, ""},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(Parallel, ARM32ParallelRT, ::testing::ValuesIn(kArm32),
                         rtTCName);
