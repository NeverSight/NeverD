//===- X86LegacyDescriptorNativeTests.inc - Original table queries -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original CPU descriptor visibility, read footprint and precise faults.
//===----------------------------------------------------------------------===//

#include "X86LegacyScalarNativeFixture.inc"

class X86LegacyDescriptorNativeTest : public X86LegacyScalarNativeFixture {
protected:
  void SetUp() override {
    X86LegacyScalarNativeFixture::SetUp();
    DescriptorQueries = X86LegacyDescriptorProfile::TableImage;
    CheckOperandPageEffects = true;
  }

  void check(unsigned Form, uint64_t Descriptor = 0x00cff2000000ffffULL,
             unsigned CPL = 3, uint16_t Selector = 0x103b, bool Code32 = true,
             bool Operand32 = true, bool Address32 = true,
             unsigned Destination = 0, int Source = 6, unsigned Case = 0,
             unsigned TableCase = 0, unsigned Prefix = 0) {
    SCOPED_TRACE(testing::Message()
                 << "form=" << Form << " descriptor=" << std::hex << Descriptor
                 << " selector=" << Selector << " table=" << TableCase);
    const auto Bytes = neverd::test::legacyDescriptorBytes(
        Form, Code32, Operand32, Address32, Destination, Source, Prefix);
    const uint32_t Offset = Case == 7 || Case == 8 ? 0xc101
                            : Case == 16           ? 0xcfff
                                                   : 0xc100;
    std::array<uint32_t, 8> Registers{
        0x89abcdef, 0x10203040, 0x55667788, Offset, 0xd100, Offset, 0, Offset};
    if (Source >= 0)
      Registers[Source] = 0xabcd0000 | Selector;
    const unsigned PW = GetParam() == 2 ? 4 : 8;
    const uint32_t PT = GetParam() == 2 ? 0x11000 : 0x12000;
    ExtraMemoryChecks = {{0x8038, 8}, {0xa038, 8}, {0x8ffc, 8}};
    ASSERT_NO_FATAL_FAILURE(
        compare(Bytes, Code32, true, CPL, Case, Selector, 0,
                0xed7 | (Case == 7 || Case == 8 ? 0x40000 : 0), 0x4000, 0, Code,
                Registers, [&](kvm_sregs &Special, kvm_regs &) {
                  Special.gdt.limit = TableCase == 1 ? 0x103e : 0x103f;
                  Special.ldt = {};
                  Special.ldt.base = 0x9000;
                  Special.ldt.limit = TableCase == 1 ? 0x103e : 0x103f;
                  Special.ldt.selector = 48;
                  Special.ldt.type = 2;
                  Special.ldt.present = 1;
                  Special.ldt.unusable = TableCase == 2;
                  if (TableCase >= 5 && TableCase <= 7) {
                    Special.gdt.base = 0x7fc4;
                    Special.ldt.base = 0x7fc4;
                    for (unsigned Index : {8u, 16u, 24u, 32u})
                      put(0x7fc4 + Index, get(0x7000 + Index, 8));
                  }
                  const uint32_t Address =
                      (Selector & 4 ? Special.ldt.base : Special.gdt.base) +
                      (Selector & 0xfff8);
                  put(Address, Descriptor);
                  const unsigned Page = Address >> 12;
                  if (TableCase == 3 || TableCase == 6)
                    put(PT + Page * PW, Page * 4096 | 6, PW);
                  if (TableCase == 4)
                    put(PT + Page * PW, Page * 4096 | 1, PW);
                  if (TableCase == 5 || TableCase == 7)
                    put(PT + (Page + 1) * PW, (Page + 1) * 4096 | 6, PW);
                  if (TableCase == 7)
                    put(PT + Page * PW, Page * 4096 | 6, PW);
                }));
  }
};

TEST_P(X86LegacyDescriptorNativeTest, EveryTypePrivilegeAndPresence) {
  for (unsigned Form = 0; Form != 4; ++Form)
    for (unsigned Access = 0; Access != 256; ++Access)
      for (unsigned CPL : {0u, 1u, 2u, 3u})
        for (unsigned RPL = 0; RPL != 4; ++RPL) {
          const uint64_t D = 0x00da004567891234ULL | (uint64_t(Access) << 40);
          ASSERT_NO_FATAL_FAILURE(
              check(Form, D, CPL, 0x1038 | RPL, true, Access & 1));
          if (HasFailure())
            return;
        }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyDescriptorNativeTest, EveryDescriptorFlagAndLimitNibble) {
  for (unsigned Form = 0; Form != 4; ++Form)
    for (unsigned Attributes = 0; Attributes != 256; ++Attributes)
      for (bool Operand32 : {false, true}) {
        const uint64_t D = 0x0000724567891234ULL | (uint64_t(Attributes) << 48);
        ASSERT_NO_FATAL_FAILURE(check(Form, D, 3, 0x103b, false, Operand32));
        if (HasFailure())
          return;
      }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyDescriptorNativeTest,
       CodeOperandAddressWidthsAndRegisterAliases) {
  for (unsigned Form = 0; Form != 4; ++Form)
    for (bool Code32 : {false, true})
      for (bool Operand32 : {false, true})
        for (bool Address32 : {false, true})
          for (unsigned Destination = 0; Destination != 8; ++Destination)
            for (int Source : {-1, 0, 1, 2, 3, 4, 5, 6, 7}) {
              ASSERT_NO_FATAL_FAILURE(check(Form, 0x006af20000001234ULL, 3,
                                            0x103b, Code32, Operand32,
                                            Address32, Destination, Source));
              if (HasFailure())
                return;
            }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyDescriptorNativeTest,
       NullLDTBoundsAndFaultableDescriptorFootprint) {
  for (unsigned Form = 0; Form != 4; ++Form)
    for (bool Code32 : {false, true})
      for (unsigned CPL : {0u, 3u})
        for (uint16_t Selector : {0u, 1u, 2u, 3u, 4u, 7u, 0x103bu, 0x103fu})
          for (unsigned TableCase = 0; TableCase != 8; ++TableCase) {
            // Null selectors do not query their table. Keep GDT[0] away from
            // exception descriptors when constructing a deliberately split row.
            ASSERT_NO_FATAL_FAILURE(check(Form, 0x00af720000001234ULL, CPL,
                                          Selector, Code32, true, true, 0, 6, 0,
                                          TableCase));
            if (HasFailure())
              return;
          }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyDescriptorNativeTest, SelectorSegmentsPagesAlignmentAndRetry) {
  for (unsigned Form = 0; Form != 4; ++Form)
    for (bool Code32 : {false, true})
      for (bool Operand32 : {false, true})
        for (bool Address32 : {false, true})
          for (unsigned CPL : {0u, 3u})
            for (unsigned Case : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 12u, 16u})
              for (unsigned Prefix : {0u, 0x2eu, 0x36u, 0x64u}) {
                ASSERT_NO_FATAL_FAILURE(
                    check(Form, 0x00cff2000000ffffULL, CPL, 0x103b, Code32,
                          Operand32, Address32, 0, -1, Case, 0, Prefix));
                if (HasFailure())
                  return;
              }
  RecordProperty("cpu_comparisons", Comparisons);
}

INSTANTIATE_TEST_SUITE_P(LegacyDescriptors, X86LegacyDescriptorNativeTest,
                         testing::Values(2u, 3u));
