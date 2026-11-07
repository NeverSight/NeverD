//===- X86LegacyBMINativeTests.inc - Original protected BMI oracle -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compare original VEX bytes, all register effects and fault delivery.
//===----------------------------------------------------------------------===//

#include "X86LegacyScalarNativeObserverFixture.inc"

class X86LegacyBMINativeTest : public X86LegacyScalarNativeObserverFixture {
protected:
  void SetUp() override {
    X86LegacyScalarNativeObserverFixture::SetUp();
    if (HasFatalFailure() || IsSkipped())
      return;
    const auto *CPUID =
        reinterpret_cast<const kvm_cpuid2 *>(CPUIDStorage.data());
    X86LegacyIntegerExtensions Features;
    for (unsigned I = 0; I != CPUID->nent; ++I) {
      const auto &E = CPUID->entries[I];
      if (E.function == 7 && E.index == 0) {
        Features.BMI1 = E.ebx & (1u << 3);
        Features.BMI2 = E.ebx & (1u << 8);
      }
    }
    if (!Features.BMI1 || !Features.BMI2)
      GTEST_SKIP() << "Original BMI oracle requires BMI1/BMI2";
    IntegerExtensions = Features;
  }

  void check(unsigned Form, bool Code32, bool Address32, unsigned Destination,
             unsigned VRegister, int Source, uint32_t Value, uint32_t Control,
             uint32_t Flags, unsigned Case = 0, unsigned Prefix = 0,
             unsigned CPL = 3, unsigned Ignored = 0) {
    const auto &F = neverd::test::LegacyBMIForms[Form];
    SCOPED_TRACE(testing::Message()
                 << F.Name << " dst=" << Destination << " vreg=" << VRegister
                 << " src=" << Source << " ignored=" << Ignored);
    auto Bytes = neverd::test::legacyBMIBytes(
        Form, Destination, VRegister, Source, Address32, uint8_t(Control));
    if (Ignored & 1)
      Bytes[2] |= 0x80; // W ignored outside 64-bit mode.
    if (Ignored & 2)
      Bytes[1] ^= 0x20; // B ignored outside 64-bit mode.
    if (Ignored & 4)
      Bytes[2] ^= 0x40; // High vvvv ignored for a register operand.
    if (Code32 != Address32)
      Bytes.insert(Bytes.begin(), 0x67);
    if (Prefix)
      Bytes.insert(Bytes.begin(), Prefix);
    const uint32_t Offset = Case == 7 || Case == 8 ? 0xc101
                            : Case == 16           ? 0xcfff
                                                   : 0xc100;
    std::array<uint32_t, 8> Registers{
        0x87654321, 0x33445566, 0xfedcba98, Offset, 0xd100, Offset, 0, Offset};
    Registers[VRegister] = Control;
    if (Source >= 0)
      Registers[Source] = Value;
    if (F.UndefinedMask) {
      // Separate host execution supplies undefined bits only. Original KVM
      // values, defined flags, accesses and exceptions never seed the model.
      std::vector<uint8_t> Probe{0x9c, 0x51, 0x9d};
      const auto Operation = neverd::test::legacyBMIBytes(Form, 0, 7, 6);
      Probe.insert(Probe.end(), Operation.begin(), Operation.end());
      Probe.insert(Probe.end(), {0x9c, 0x5a, 0x48, 0xc1, 0xe2, 0x20, 0x48, 0x09,
                                 0xd0, 0x9d, 0xc3});
      const uint32_t Input = Source >= 0 ? Registers[Source]
                             : Case == 3 && (Prefix == 0x36 || Prefix == 0x2e)
                                 ? 0
                                 : Value;
      uint64_t Observed;
      ASSERT_NO_FATAL_FAILURE(
          observeCode(Probe, Registers[VRegister], Input, 0, Flags, Observed));
      BMIUndefinedFlags = uint32_t(Observed >> 32) & F.UndefinedMask;
    } else {
      BMIUndefinedFlags.reset();
    }
    ASSERT_NO_FATAL_FAILURE(compare(Bytes, Code32, true, CPL, Case, Value, 0,
                                    Flags, 0x4000, 0, Code, Registers));
  }
};

TEST_P(X86LegacyBMINativeTest, ValuesControlsAliasesAndFlagPreservation) {
  for (bool Code32 : {false, true})
    for (unsigned Form = 0; Form != neverd::test::LegacyBMIForms.size(); ++Form)
      for (uint32_t Value :
           {0u, 1u, 0x80000000u, 0x80000001u, 0xffffffffu, 0x12345678u})
        for (uint32_t Control : {0u, 1u, 31u, 32u, 33u, 255u, 0x201fu, 0x0100u,
                                 0xffffu, 0xa5a52000u})
          for (int Source : {0, 1, 6, -1}) {
            ASSERT_NO_FATAL_FAILURE(check(Form, Code32, true, 0, 1, Source,
                                          Value, Control,
                                          Control & 1 ? 0xed7 : 0x202));
            if (HasFailure())
              return;
          }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyBMINativeTest, AllRegistersAndMultiplyDestinationOrder) {
  for (bool Code32 : {false, true})
    for (unsigned Form = 0; Form != neverd::test::LegacyBMIForms.size(); ++Form)
      for (unsigned Destination = 0; Destination != 8; ++Destination)
        for (unsigned VRegister = 0; VRegister != 8; ++VRegister)
          for (unsigned Source : {Destination, VRegister, 2u}) {
            ASSERT_NO_FATAL_FAILURE(check(Form, Code32, true, Destination,
                                          VRegister, Source, 0x80000001,
                                          0x12342007, 0xed7));
            if (HasFailure())
              return;
          }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyBMINativeTest, IgnoredWBaseAndHighRegisterBits) {
  for (bool Code32 : {false, true})
    for (unsigned Form = 0; Form != neverd::test::LegacyBMIForms.size(); ++Form)
      for (unsigned Ignored = 0; Ignored != (Form == 9 ? 4u : 8u); ++Ignored)
        for (int Source : {0, 1, -1}) {
          ASSERT_NO_FATAL_FAILURE(check(Form, Code32, Code32, 0, 1, Source,
                                        0x80000001, 0x201f, 0xed7, 0, 0, 3,
                                        Ignored));
          if (HasFailure())
            return;
        }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyBMINativeTest, SegmentsPagesAlignmentAndOriginalRepair) {
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (unsigned CPL : {0u, 3u})
        for (unsigned Form = 0; Form != neverd::test::LegacyBMIForms.size();
             ++Form)
          for (unsigned Case : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 12u, 16u})
            for (unsigned Prefix : {0u, 0x2eu, 0x36u, 0x64u}) {
              ASSERT_NO_FATAL_FAILURE(
                  check(Form, Code32, Address32, 0, 1, -1, 0x12345678, 0x2010,
                        0xed7 | (Case == 7 || Case == 8 ? 0x40000 : 0), Case,
                        Prefix, CPL));
              if (HasFailure())
                return;
            }
  RecordProperty("cpu_comparisons", Comparisons);
}

INSTANTIATE_TEST_SUITE_P(LegacyBMI, X86LegacyBMINativeTest,
                         testing::Values(2u, 3u));
