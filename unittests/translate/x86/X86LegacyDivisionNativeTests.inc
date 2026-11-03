//===- X86LegacyDivisionNativeTests.inc - Original protected divides ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent DIV/IDIV CPU comparisons, operand faults and original retry.
//===----------------------------------------------------------------------===//

#include "X86LegacyScalarNativeFixture.inc"

class X86LegacyDivisionNativeTest : public X86LegacyScalarNativeFixture {
protected:
  void SetUp() override {
    X86LegacyScalarNativeFixture::SetUp();
    if (HasFatalFailure() || IsSkipped())
      return;
    const char *Bound = std::getenv("NEVERD_TEST_LEGACY_DIVIDE_FLAGS");
    if (!Bound || std::strcmp(Bound, "preserve"))
      GTEST_SKIP() << "Bind the qualified DIV/IDIV undefined-flags choice";
  }

  static std::vector<uint8_t> instruction(bool Code32, unsigned Width,
                                          bool Signed, uint8_t Address) {
    std::vector<uint8_t> Bytes;
    if (Width != 1 && (Width == 4) != Code32)
      Bytes.push_back(0x66);
    Bytes.push_back(Width == 1 ? 0xf6 : 0xf7);
    Bytes.push_back(Address | (Signed ? 7 : 6) << 3);
    return Bytes;
  }
};

TEST_P(X86LegacyDivisionNativeTest,
       FullDividendsQuotientsAndPreservedFlagChoice) {
  for (bool Code32 : {false, true})
    for (unsigned Width : {1u, 2u, 4u}) {
      const uint32_t Mask = UINT32_MAX >> (32 - Width * 8);
      const uint32_t Sign = (Mask >> 1) + 1;
      for (bool Signed : {false, true})
        for (uint32_t Low : {0u, 1u, Sign - 1, Sign, Mask - 1, Mask})
          for (uint32_t High : {0u, 1u, Sign, Mask})
            for (uint32_t Divisor :
                 {0u, 1u, 2u, 3u, Sign - 1, Sign, Mask - 1, Mask})
              for (unsigned CPL = 0; CPL != 4; ++CPL)
                for (uint32_t Flags : {0x202u, 0xad7u}) {
                  const uint32_t AX = Width == 1 ? 0xabcd0000 | High << 8 | Low
                                      : Width == 2 ? 0xabcd0000 | Low
                                                   : Low;
                  const uint32_t DX = Width == 2 ? 0x76540000 | High : High;
                  ASSERT_NO_FATAL_FAILURE(
                      compare(instruction(Code32, Width, Signed, 0xc1), Code32,
                              true, CPL, 0, AX, Divisor, Flags, 0x4000, DX));
                }
    }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyDivisionNativeTest, EveryRegisterAndImplicitDividendAlias) {
  for (bool Code32 : {false, true})
    for (bool Stack32 : {false, true})
      for (unsigned Width : {1u, 2u, 4u})
        for (bool Signed : {false, true})
          for (unsigned Reg = 0; Reg != 8; ++Reg)
            for (unsigned CPL = 0; CPL != 4; ++CPL)
              ASSERT_NO_FATAL_FAILURE(
                  compare(instruction(Code32, Width, Signed, 0xc0 | Reg),
                          Code32, Stack32, CPL, 0, 0xffffff84, uint32_t(-3),
                          0xad7, 0x4000, UINT32_MAX));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyDivisionNativeTest,
       ReadonlySegmentsPagingAndAlignmentPrecedeDE) {
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (bool Stack32 : {false, true})
        for (unsigned Width : {1u, 2u, 4u})
          for (bool Signed : {false, true})
            for (unsigned CPL = 0; CPL != 4; ++CPL)
              for (unsigned Case : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 16u})
                for (uint32_t Divisor : {0u, 7u}) {
                  auto Bytes = instruction(Code32, Width, Signed,
                                           Address32 ? 3 : 0); // [EBX]/[BX+SI].
                  if (Address32 != Code32)
                    Bytes.insert(Bytes.begin(), 0x67);
                  if (Case == 6)
                    Bytes.insert(Bytes.begin(), 0x36);
                  ASSERT_NO_FATAL_FAILURE(
                      compare(Bytes, Code32, Stack32, CPL, Case, Divisor, 3,
                              0x202 | (Case == 7 || Case == 8 ? 0x40000 : 0),
                              0x4000, 0));
                }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyDivisionNativeTest,
       OriginalPageAndDivideHandlersReturnToRetry) {
  for (bool Code32 : {false, true})
    for (bool Stack32 : {false, true})
      for (unsigned Width : {1u, 2u, 4u})
        for (bool Signed : {false, true})
          for (unsigned CPL = 0; CPL != 4; ++CPL) {
            ASSERT_NO_FATAL_FAILURE(
                compare(instruction(Code32, Width, Signed, Code32 ? 3 : 0),
                        Code32, Stack32, CPL, 12, 7, 3, 0x202, 0x4000, 0));
            ASSERT_NO_FATAL_FAILURE(
                compare(instruction(Code32, Width, Signed, 0xc1), Code32,
                        Stack32, CPL, 18, 10, 0, 0x202, 0x4000, 0));
          }
  RecordProperty("cpu_comparisons", Comparisons);
}

INSTANTIATE_TEST_SUITE_P(LegacyDivision, X86LegacyDivisionNativeTest,
                         testing::Values(2u, 3u));
