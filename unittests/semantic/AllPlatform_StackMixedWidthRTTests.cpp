//===- AllPlatform_StackMixedWidthRTTests.cpp - stack narrow/wide -*- C++ -*-===//
//
// Probes: i32 init on stack, i8 store to low byte, i32 reload (must not
// forward the narrow store's wide predecessor value).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64StackMixRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64StackMixRT, Verify) { roundTripAArch64(GetParam()); }

class X64StackMixRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64StackMixRT, Verify) { roundTripX64(GetParam()); }

class Arm32StackMixRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(Arm32StackMixRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kA64StackMix = {
  {"narrow_wide_a64",
   "long narrow_wide_a64(long a){ volatile unsigned m=0x11223344u;"
   " volatile unsigned char *p=(volatile unsigned char *)&m; *p=0xDDu;"
   " return (long)m; }\n",
   {0}, "StackMix", 1, "", false, "", UC_CPU_ARM64_MAX},
};

static const std::vector<RoundTripTC> kX64StackMix = {
  {"narrow_wide_x64",
   "long narrow_wide_x64(long a){ volatile unsigned m=0x11223344u;"
   " volatile unsigned char *p=(volatile unsigned char *)&m; *p=0xDDu;"
   " return (long)m; }\n",
   {0}, "StackMix"},
};

static const std::vector<RoundTripTC> kArm32StackMix = {
  {"narrow_wide_arm32",
   "unsigned long narrow_wide_arm32(unsigned long a){ volatile unsigned m=0x11223344u;"
   " volatile unsigned char *p=(volatile unsigned char *)&m; *p=0xDDu;"
   " return m; }\n",
   {0}, "StackMix", 1, "", false, "", UC_CPU_ARM_MAX},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(A64StackMix, A64StackMixRT,
                         ::testing::ValuesIn(kA64StackMix), rtTCName);
INSTANTIATE_TEST_SUITE_P(X64StackMix, X64StackMixRT,
                         ::testing::ValuesIn(kX64StackMix), rtTCName);
INSTANTIATE_TEST_SUITE_P(Arm32StackMix, Arm32StackMixRT,
                         ::testing::ValuesIn(kArm32StackMix), rtTCName);
