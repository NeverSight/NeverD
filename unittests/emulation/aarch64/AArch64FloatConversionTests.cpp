//===- AArch64FloatConversionTests.cpp - Exact narrowing and state
//---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Portable expected bits plus direct ARM64 FCVT comparisons when available.
//===----------------------------------------------------------------------===//
#include "AArch64FloatConversionCases.h"
#include "arch/aarch64/AArch64FloatConversion.h"
#include "gtest/gtest.h"

namespace neverd::emulation {
namespace {
TEST(AArch64FloatConversion, ArchitecturalResultsAndRaisedExceptions) {
  for (const auto &Case : test::FloatNarrowingCases) {
    SCOPED_TRACE(Case.Input);
    SCOPED_TRACE(Case.Control);
    auto Result = narrowAArch64Double(Case.Input, Case.Control);
    ASSERT_TRUE(Result);
    EXPECT_EQ(Result->Bits, Case.Output);
    EXPECT_EQ(Result->Raised, Case.Raised);
  }
}

TEST(AArch64FloatConversion, RejectsEveryUnmodeledControlBit) {
  for (unsigned Bit = 0; Bit != 64; ++Bit) {
    if (Bit >= 22 && Bit <= 25)
      continue;
    EXPECT_FALSE(narrowAArch64Double(0x3ff0000000000000, uint64_t(1) << Bit))
        << Bit;
  }
}

#if defined(__aarch64__) && defined(__GNUC__)
AArch64FloatConversion nativeNarrow(uint64_t Bits, uint64_t Control) {
  uint64_t OldControl, OldStatus, Status;
  uint32_t Result;
  // One block ensures the host's FP controls/status are restored before any
  // C++ assertion or library call. No host floating C expression is involved.
  asm volatile("mrs %0, fpcr\n"
               "mrs %1, fpsr\n"
               "msr fpcr, %4\n"
               "msr fpsr, xzr\n"
               "fmov d0, %5\n"
               "fcvt s0, d0\n"
               "fmov %w2, s0\n"
               "mrs %3, fpsr\n"
               "msr fpcr, %0\n"
               "msr fpsr, %1"
               : "=&r"(OldControl), "=&r"(OldStatus), "=&r"(Result),
                 "=&r"(Status)
               : "r"(Control), "r"(Bits)
               : "v0", "memory");
  return {Result, uint32_t(Status)};
}
#endif

TEST(AArch64FloatConversion, MatchesNativeFCVTForEveryBaselineControl) {
#if defined(__aarch64__) && defined(__GNUC__)
  uint64_t Random = 0x8c7e39d562b4a10f;
  for (unsigned Profile = 0; Profile != 16; ++Profile) {
    const uint64_t Control = uint64_t(Profile) << 22;
    SCOPED_TRACE(Control);
    auto Check = [&](uint64_t Input) {
      const auto Expected = nativeNarrow(Input, Control);
      const auto Result = narrowAArch64Double(Input, Control);
      ASSERT_TRUE(Result);
      ASSERT_EQ(Result->Bits, Expected.Bits) << Input;
      ASSERT_EQ(Result->Raised, Expected.Raised) << Input;
    };
    for (const auto &Case : test::FloatNarrowingCases) {
      Check(Case.Input);
      if (HasFatalFailure())
        return;
    }
    for (unsigned I = 0; I != 4096; ++I) {
      Random ^= Random << 13;
      Random ^= Random >> 7;
      Random ^= Random << 17;
      Check(Random);
      if (HasFatalFailure())
        return;
    }
  }
#else
  GTEST_SKIP() << "Direct ARM64 FCVT oracle requires an AArch64 GNU-asm host";
#endif
}
} // namespace
} // namespace neverd::emulation
