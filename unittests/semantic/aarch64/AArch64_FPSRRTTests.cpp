//===- AArch64_FPSRRTTests.cpp - FPSR/QC semantic round trips -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64FPSRRT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64FPSRRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kFPSR = {
  {"sqadd_sets_qc",
   "long sqadd_sets_qc(void) { unsigned long fpsr;\n"
   "  __asm__ volatile(\"msr fpsr, xzr\\n\"\n"
   "                   \"movi v0.16b, #0x7f\\n\"\n"
   "                   \"movi v1.16b, #1\\n\"\n"
   "                   \"sqadd v0.16b, v0.16b, v1.16b\\n\"\n"
   "                   \"mrs %0, fpsr\" : \"=r\"(fpsr) : : \"v0\", \"v1\", \"memory\");\n"
   "  return (long)fpsr; }\n",
   {}, "FPSR", 0, "-march=armv8-a+simd"},

  {"uqsub_sets_qc",
   "long uqsub_sets_qc(void) { unsigned long fpsr;\n"
   "  __asm__ volatile(\"msr fpsr, xzr\\n\"\n"
   "                   \"movi v0.16b, #0\\n\"\n"
   "                   \"movi v1.16b, #1\\n\"\n"
   "                   \"uqsub v0.16b, v0.16b, v1.16b\\n\"\n"
   "                   \"mrs %0, fpsr\" : \"=r\"(fpsr) : : \"v0\", \"v1\", \"memory\");\n"
   "  return (long)fpsr; }\n",
   {}, "FPSR", 0, "-march=armv8-a+simd"},

  {"fpsr_write_read",
   "long fpsr_write_read(long a) { unsigned long fpsr;\n"
   "  __asm__ volatile(\"msr fpsr, %1\\n\"\n"
   "                   \"mrs %0, fpsr\" : \"=r\"(fpsr) : \"r\"((unsigned long)a) : \"memory\");\n"
   "  return (long)fpsr; }\n",
   {0x08000091ULL}, "FPSR", 1, "-march=armv8-a+simd"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(FPSR, A64FPSRRT, ::testing::ValuesIn(kFPSR),
                         rtTCName);
