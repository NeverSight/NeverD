//===- X86LegacyMultiplyNativeTests.inc - Original protected products ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Full/truncated MUL/IMUL products, aliases, explicit flag profiles and
/// faults.
//===----------------------------------------------------------------------===//

#include "X86LegacyScalarNativeFixture.inc"

class X86LegacyMultiplyNativeTest : public X86LegacyScalarNativeFixture {
protected:
  void SetUp() override {
    X86LegacyScalarNativeFixture::SetUp();
    if (HasFatalFailure() || IsSkipped())
      return;
    const char *Bound = std::getenv("NEVERD_TEST_LEGACY_MULTIPLY_FLAGS");
    if (!Bound)
      GTEST_SKIP() << "Bind the qualified MUL/IMUL undefined-flags profile";
    if (!std::strcmp(Bound, "sign-parity"))
      Multiply = X86LegacyMultiplyProfile::ProductSignParity;
    else if (!std::strcmp(Bound, "sign-parity-zero"))
      Multiply = X86LegacyMultiplyProfile::ProductSignParityZero;
    else
      FAIL() << "Multiply flags must be sign-parity or sign-parity-zero";
  }

  static std::vector<uint8_t> instruction(bool Code32, unsigned Width,
                                          unsigned Form, uint8_t Address,
                                          uint32_t Immediate = UINT32_MAX) {
    std::vector<uint8_t> Bytes;
    if (Width != 1 && (Width == 4) != Code32)
      Bytes.push_back(0x66);
    if (Form < 2) {
      Bytes.push_back(Width == 1 ? 0xf6 : 0xf7);
      Bytes.push_back(Address | (Form ? 5 : 4) << 3);
    } else {
      if (Form == 2)
        Bytes.insert(Bytes.end(), {0x0f, 0xaf});
      else
        Bytes.push_back(Form == 3 ? 0x6b : 0x69);
      Bytes.push_back(Address);
      if (Form >= 3)
        for (unsigned I = 0; I < (Form == 3 ? 1 : Width); ++I)
          Bytes.push_back(Immediate >> (I * 8));
    }
    return Bytes;
  }
};

TEST_P(X86LegacyMultiplyNativeTest, FullProductsAndSignedOverflow) {
  for (bool Code32 : {false, true})
    for (unsigned Width : {1u, 2u, 4u}) {
      const uint32_t Mask = UINT32_MAX >> (32 - Width * 8);
      const uint32_t Sign = (Mask >> 1) + 1;
      for (unsigned Form : {0u, 1u})
        for (uint32_t Left : {0u, 1u, 2u, 7u, Sign - 1, Sign, Mask - 1, Mask})
          for (uint32_t Right :
               {0u, 1u, 2u, 7u, Sign - 1, Sign, Mask - 1, Mask})
            for (unsigned CPL = 0; CPL != 4; ++CPL)
              for (uint32_t Flags : {0x202u, 0xad7u}) {
                const uint32_t AX = Width == 1   ? 0xabcd5500 | Left
                                    : Width == 2 ? 0xabcd0000 | Left
                                                 : Left;
                ASSERT_NO_FATAL_FAILURE(
                    compare(instruction(Code32, Width, Form, 0xc1), Code32,
                            true, CPL, 0, AX, Right, Flags));
              }
    }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyMultiplyNativeTest, EveryRegisterAndImplicitResultAlias) {
  for (bool Code32 : {false, true})
    for (bool Stack32 : {false, true})
      for (unsigned Width : {1u, 2u, 4u})
        for (unsigned Form : {0u, 1u})
          for (unsigned Reg = 0; Reg != 8; ++Reg)
            for (unsigned CPL = 0; CPL != 4; ++CPL)
              ASSERT_NO_FATAL_FAILURE(
                  compare(instruction(Code32, Width, Form, 0xc0 | Reg), Code32,
                          Stack32, CPL, 0, 0xffffff84, uint32_t(-3), 0xad7));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyMultiplyNativeTest, TruncatedProductsRegistersAndImmediates) {
  for (bool Code32 : {false, true})
    for (unsigned Width : {2u, 4u})
      for (unsigned Destination = 0; Destination != 8; ++Destination)
        for (unsigned Source = 0; Source != 8; ++Source)
          for (unsigned Form : {2u, 3u, 4u})
            for (unsigned CPL = 0; CPL != 4; ++CPL)
              for (uint32_t Immediate : {UINT32_MAX, 0x80000080u})
                ASSERT_NO_FATAL_FAILURE(compare(
                    instruction(Code32, Width, Form,
                                0xc0 | Destination << 3 | Source, Immediate),
                    Code32, true, CPL, 0, 0x8000ffff, uint32_t(-3), 0xad7));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyMultiplyNativeTest, ReadonlyMemoryFaultsAndOriginalRepair) {
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (bool Stack32 : {false, true})
        for (unsigned Width : {1u, 2u, 4u})
          for (unsigned Form = 0; Form != 5; ++Form) {
            if (Width == 1 && Form >= 2)
              continue;
            for (unsigned CPL = 0; CPL != 4; ++CPL)
              for (unsigned Case :
                   {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 12u, 16u}) {
                auto Bytes = instruction(Code32, Width, Form,
                                         Address32 ? 3 : 0); // [EBX]/[BX+SI].
                if (Address32 != Code32)
                  Bytes.insert(Bytes.begin(), 0x67);
                if (Case == 6)
                  Bytes.insert(Bytes.begin(), 0x36);
                ASSERT_NO_FATAL_FAILURE(
                    compare(Bytes, Code32, Stack32, CPL, Case, 0xffffff87, 3,
                            0x202 | (Case == 7 || Case == 8 ? 0x40000 : 0)));
              }
          }
  RecordProperty("cpu_comparisons", Comparisons);
}

INSTANTIATE_TEST_SUITE_P(LegacyMultiply, X86LegacyMultiplyNativeTest,
                         testing::Values(2u, 3u));
