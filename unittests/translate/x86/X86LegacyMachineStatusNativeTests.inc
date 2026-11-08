//===- X86LegacyMachineStatusNativeTests.inc - Original protected CR0 -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original SMSW/LMSW/CLTS against the model, including full control banks.
//===----------------------------------------------------------------------===//

#include "X86LegacyScalarNativeFixture.inc"

class X86LegacyMachineStatusNativeTest : public X86LegacyScalarNativeFixture {
protected:
  void SetUp() override {
    X86LegacyScalarNativeFixture::SetUp();
    MachineStatus = X86LegacyMachineStatusProfile::FullCR0;
    CheckOperandPageEffects = true;
  }

  void check(unsigned Form, uint32_t InitialCR0 = 0x8005001f,
             uint32_t Input = 0xdeadfffc, unsigned CPL = 0, bool Code32 = true,
             bool Operand32 = true, bool Address32 = true, int Register = 6,
             unsigned Case = 0, bool UMIP = false, unsigned Prefix = 0) {
    SCOPED_TRACE(testing::Message()
                 << "form=" << Form << " cr0=" << std::hex << InitialCR0
                 << " input=" << Input << " umip=" << UMIP);
    const auto Bytes = neverd::test::legacyMachineStatusBytes(
        Form, Code32, Operand32, Address32, Register, Prefix);
    const uint32_t Offset = Case == 7 || Case == 8 ? 0xc101
                            : Case == 16           ? 0xcfff
                                                   : 0xc100;
    std::array<uint32_t, 8> Registers{
        0x89abcdef, 0x10203040, 0x55667788, Offset, 0xd100, Offset, 0, Offset};
    if (Register >= 0)
      Registers[Register] = Input;
    ASSERT_NO_FATAL_FAILURE(
        compare(Bytes, Code32, true, CPL, Case, Input, 0,
                0xed7 | (Case == 7 || Case == 8 ? 0x40000 : 0), 0x4000, 0, Code,
                Registers, [&](kvm_sregs &Special, kvm_regs &) {
                  Special.cr0 = InitialCR0;
                  Special.cr4 |= UMIP ? 1u << 11 : 0;
                }));
  }
};

TEST_P(X86LegacyMachineStatusNativeTest, EveryLowBitAndIgnoredSourceBit) {
  for (unsigned Initial = 0; Initial != 8; ++Initial)
    for (unsigned Input = 0; Input != 16; ++Input)
      for (int Register : {-1, 6}) {
        ASSERT_NO_FATAL_FAILURE(check(1, 0x80050011 | (Initial << 1),
                                      0xfffffff0 | Input, 0, true, true, true,
                                      Register));
        if (HasFailure())
          return;
      }
  for (unsigned Bit = 4; Bit != 32; ++Bit)
    for (int Register : {-1, 6}) {
      ASSERT_NO_FATAL_FAILURE(
          check(1, 0x8005001f, 1u << Bit, 0, true, true, true, Register));
      if (HasFailure())
        return;
    }
  // Independently vary preserved CR0 fields, including WP/AM/NE and CD/NW.
  for (uint32_t CR0 : {0x80000011u, 0x80010013u, 0x80040015u, 0x80050039u,
                       0xc005003fu, 0xe005003fu})
    for (unsigned Form = 0; Form != 3; ++Form) {
      ASSERT_NO_FATAL_FAILURE(check(Form, CR0));
      if (HasFailure())
        return;
    }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyMachineStatusNativeTest, WidthsRegistersAndMemory) {
  for (unsigned Form = 0; Form != 3; ++Form)
    for (bool Code32 : {false, true})
      for (bool Operand32 : {false, true})
        for (bool Address32 : {false, true})
          for (int Register : {-1, 0, 1, 2, 3, 4, 5, 6, 7}) {
            ASSERT_NO_FATAL_FAILURE(check(Form, 0x8005001f, 0xdeadfff4, 0,
                                          Code32, Operand32, Address32,
                                          Register));
            if (HasFailure())
              return;
          }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyMachineStatusNativeTest, FaultsAndPrivilegePriority) {
  for (unsigned Form = 0; Form != 3; ++Form)
    for (unsigned CPL : {0u, 1u, 2u, 3u})
      for (bool UMIP : {false, true})
        for (bool Address32 : {false, true})
          for (unsigned Case : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 16u})
            for (unsigned Prefix : {0u, 0x2eu, 0x36u, 0x64u}) {
              ASSERT_NO_FATAL_FAILURE(check(Form, 0x8005001f, 0xdeadfff4, CPL,
                                            true, true, Address32, -1, Case,
                                            UMIP, Prefix));
              if (HasFailure())
                return;
            }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyMachineStatusNativeTest, OriginalFaultRepairAndRetry) {
  for (unsigned Form : {0u, 1u})
    for (bool Code32 : {false, true})
      for (bool Operand32 : {false, true})
        for (bool Address32 : {false, true}) {
          ASSERT_NO_FATAL_FAILURE(check(Form, 0x8005001f, 0xdeadfff4, 0, Code32,
                                        Operand32, Address32, -1, 12));
          if (HasFailure())
            return;
        }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyMachineStatusNativeTest, AccessControlFlagsAndPrivilege) {
  for (unsigned Form : {3u, 4u})
    for (unsigned CPL = 0; CPL != 4; ++CPL)
      for (bool Code32 : {false, true})
        for (bool Operand32 : {false, true})
          for (bool Address32 : {false, true})
            for (bool AC : {false, true}) {
              const auto Bytes = neverd::test::legacyMachineStatusBytes(
                  Form, Code32, Operand32, Address32);
              ASSERT_NO_FATAL_FAILURE(compare(Bytes, Code32, true, CPL, 0,
                                              0x12345678, 0x80,
                                              0xed7 | (AC ? 0x40000 : 0)));
              if (HasFailure())
                return;
            }
  RecordProperty("cpu_comparisons", Comparisons);
}

INSTANTIATE_TEST_SUITE_P(LegacyMachineStatus, X86LegacyMachineStatusNativeTest,
                         testing::Values(2, 3));
