//===- X86LegacySystemSelectorNativeTests.inc - Original LDTR/TR ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Cache publication, privilege, descriptor faults and whole busy writes.
//===----------------------------------------------------------------------===//

#include "X86LegacyScalarNativeFixture.inc"

class X86LegacySystemSelectorNativeTest : public X86LegacyScalarNativeFixture {
protected:
  void SetUp() override {
    X86LegacyScalarNativeFixture::SetUp();
    SystemSelectors = X86LegacySystemSelectorProfile::ExclusiveTable;
    CheckOperandPageEffects = true;
  }

  void check(unsigned Form, uint64_t Descriptor = 0x0000890058000067ULL,
             unsigned CPL = 0, uint16_t Selector = 0x103b, bool Code32 = true,
             bool Operand32 = true, bool Address32 = true, int Register = 6,
             unsigned Case = 0, unsigned TableCase = 0, bool UMIP = false,
             unsigned Prefix = 0) {
    SCOPED_TRACE(testing::Message()
                 << "form=" << Form << " descriptor=" << std::hex << Descriptor
                 << " selector=" << Selector << " table=" << TableCase
                 << " umip=" << UMIP);
    const auto Bytes = neverd::test::legacySystemSelectorBytes(
        Form, Code32, Operand32, Address32, Register, Prefix);
    const uint32_t Offset = Case == 7 || Case == 8 ? 0xc101
                            : Case == 16           ? 0xcfff
                                                   : 0xc100;
    std::array<uint32_t, 8> Registers{
        0x89abcdef, 0x10203040, 0x55667788, Offset, 0xd100, Offset, 0, Offset};
    if (Register >= 0)
      Registers[Register] = 0xabcd0000 | Selector;
    const unsigned PW = GetParam() == 2 ? 4 : 8;
    const uint32_t PT = GetParam() == 2 ? 0x11000 : 0x12000;
    ExtraMemoryChecks = {{0x8038, 8}, {0x8ffc, 8}, {0x11044, 8}};
    ASSERT_NO_FATAL_FAILURE(compare(
        Bytes, Code32, true, CPL, Case, Selector, 0,
        0xed7 | (Case == 7 || Case == 8 ? 0x40000 : 0), 0x4000, 0, Code,
        Registers, [&](kvm_sregs &Special, kvm_regs &) {
          Special.cr4 |= UMIP ? 1u << 11 : 0;
          if (RepairPage)
            put(PT + (Offset >> 12) * PW, (Offset & ~4095u) | 7, PW);
          Special.gdt.limit = TableCase == 1 ? 0x103e : 0x103f;
          Special.ldt = {};
          Special.ldt.base = 0x9000;
          Special.ldt.limit = 0x1234;
          Special.ldt.selector = 48;
          Special.ldt.type = 2;
          Special.ldt.present = 1;
          Special.ldt.unusable = TableCase == 2;
          if (TableCase >= 5 && TableCase <= 8) {
            Special.gdt.base = TableCase == 8 ? 0x1000c : 0x7fc4;
            for (unsigned Index : {8u, 16u, 24u, 32u})
              put(Special.gdt.base + Index, get(0x7000 + Index, 8));
          }
          const uint32_t Address = Special.gdt.base + (Selector & 0xfff8);
          if ((Selector & 0xfff8) >= 0x1000)
            put(Address, Descriptor);
          const unsigned Page = Address >> 12;
          if (TableCase == 3 || TableCase == 6)
            put(PT + Page * PW, Page * 4096 | 6, PW);
          if (TableCase == 4)
            put(PT + Page * PW, Page * 4096 | 1, PW);
          if (TableCase == 5 || TableCase == 7)
            put(PT + (Page + 1) * PW, (Page + 1) * 4096 | 1, PW);
          if (TableCase == 7)
            put(PT + Page * PW, Page * 4096 | 1, PW);
        }));
  }
};

TEST_P(X86LegacySystemSelectorNativeTest, EveryTypePrivilegeAndPresence) {
  for (unsigned Form : {2u, 3u})
    for (unsigned Access = 0; Access != 256; ++Access)
      for (unsigned CPL : {0u, 1u, 2u, 3u})
        for (unsigned RPL = 0; RPL != 4; ++RPL) {
          ASSERT_NO_FATAL_FAILURE(
              check(Form, 0x0000000058000067ULL | (uint64_t(Access) << 40), CPL,
                    0x1038 | RPL));
          if (HasFailure())
            return;
        }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacySystemSelectorNativeTest, EveryFlagAndLimitNibble) {
  for (unsigned Form : {2u, 3u})
    for (unsigned Flags = 0; Flags != 256; ++Flags) {
      ASSERT_NO_FATAL_FAILURE(
          check(Form, 0x0000004567891234ULL |
                          (uint64_t(Form == 2 ? 0x82 : 0x89) << 40) |
                          (uint64_t(Flags) << 48)));
      if (HasFailure())
        return;
    }
  if (GetParam() == 2) {
    // The low descriptor dword is also the legacy PTE mapping that table.
    // Its A/D updates must not disappear under the later busy operation.
    for (unsigned Form : {2u, 3u}) {
      ASSERT_NO_FATAL_FAILURE(
          check(Form, Form == 2 ? 0x0000820000011007ULL : 0x0000890000011007ULL,
                0, 0x103b, true, true, true, 6, 0, 8));
      if (HasFailure())
        return;
    }
  }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacySystemSelectorNativeTest, WidthsRegistersAndMemory) {
  for (unsigned Form = 0; Form != 4; ++Form)
    for (bool Code32 : {false, true})
      for (bool Operand32 : {false, true})
        for (bool Address32 : {false, true})
          for (int Register : {-1, 0, 1, 2, 3, 4, 5, 6, 7}) {
            ASSERT_NO_FATAL_FAILURE(check(
                Form, Form == 2 ? 0x0000820058000067ULL : 0x0000890058000067ULL,
                0, 0x103b, Code32, Operand32, Address32, Register));
            if (HasFailure())
              return;
          }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacySystemSelectorNativeTest,
       NullLocalBoundsReadOnlyAndSplitTables) {
  for (unsigned Form : {2u, 3u})
    for (uint16_t Selector : {0u, 1u, 2u, 3u, 4u, 7u, 0x103bu, 0x103fu})
      for (unsigned TableCase = 0; TableCase != 8; ++TableCase) {
        ASSERT_NO_FATAL_FAILURE(check(
            Form, Form == 2 ? 0x0000820058000067ULL : 0x0000890058000067ULL, 0,
            Selector, true, true, true, 6, 0, TableCase));
        if (HasFailure())
          return;
      }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacySystemSelectorNativeTest,
       OperandFaultsUMIPAndPrivilegePriority) {
  for (unsigned Form = 0; Form != 4; ++Form)
    for (unsigned CPL : {0u, 3u})
      for (bool UMIP : {false, true})
        for (bool Address32 : {false, true})
          for (unsigned Case : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 16u})
            for (unsigned Prefix : {0u, 0x2eu, 0x36u, 0x64u}) {
              ASSERT_NO_FATAL_FAILURE(check(Form,
                                            Form == 2 ? 0x0000820058000067ULL
                                                      : 0x0000890058000067ULL,
                                            CPL, 0x103b, true, true, Address32,
                                            -1, Case, 0, UMIP, Prefix));
              if (HasFailure())
                return;
            }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacySystemSelectorNativeTest, TableFaultRepairAndOriginalRetry) {
  RepairPage = 0x8000;
  for (unsigned Form : {2u, 3u})
    for (bool Code32 : {false, true})
      for (bool Operand32 : {false, true})
        for (bool Address32 : {false, true})
          for (int Register : {-1, 6})
            for (unsigned TableCase : {3u, 4u}) {
              if (Form == 2 && TableCase == 4)
                continue;
              ASSERT_NO_FATAL_FAILURE(check(
                  Form,
                  Form == 2 ? 0x0000820058000067ULL : 0x0000890058000067ULL, 0,
                  0x103b, Code32, Operand32, Address32, Register, 12,
                  TableCase));
              if (HasFailure())
                return;
            }
  RecordProperty("cpu_comparisons", Comparisons);
}

INSTANTIATE_TEST_SUITE_P(LegacySystemSelectors,
                         X86LegacySystemSelectorNativeTest,
                         testing::Values(2u, 3u));
