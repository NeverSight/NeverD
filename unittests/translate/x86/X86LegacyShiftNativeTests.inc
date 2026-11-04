//===- X86LegacyShiftNativeTests.inc - Original protected bit motion ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Native defined shift/rotate state, explicit undefined flags and faults.
//===----------------------------------------------------------------------===//

#include "X86LegacyShiftNativeFixture.inc"

class X86LegacyShiftNativeTest : public X86LegacyShiftNativeFixture {
protected:
  struct Input {
    unsigned Group = 4, Width = 4, Form = 2, Count = 1;
    bool Code32 = true, Address32 = true;
    int Register = 0;
    unsigned Case = 0, Prefix = 0, CPL = 3;
    uint32_t Value = 0x87654321, Flags = 0xed7;
  };

  // Supply only architecturally undefined CF/AF/OF bits from an independent
  // host instruction. No KVM result, defined flags, RAM or fault is fed back.
  void bindUndefinedFlags(const Input &I, uint32_t Value, unsigned Count) {
    std::vector<uint8_t> Bytes;
    if (I.Width == 2)
      Bytes.push_back(0x66);
    const uint8_t Opcode = I.Form == 0 ? 0xd0 : I.Form == 1 ? 0xc0 : 0xd2;
    Bytes.insert(Bytes.end(),
                 {uint8_t(Opcode + (I.Width != 1)),
                  uint8_t((I.Register < 0 ? 4 : 0xc0) | I.Group << 3)});
    if (I.Register < 0)
      Bytes.push_back(0x24); // Independent [RSP] memory operand.
    if (I.Form == 1)
      Bytes.push_back(I.Count);
    uint64_t Observed;
    ASSERT_NO_FATAL_FAILURE(
        observe(Bytes, Value, 0, Count, I.Flags, I.Register < 0, Observed));
    ShiftUndefinedFlags =
        uint32_t(Observed >> 32) & kX86LegacyShiftUndefinedFlagsMask;
  }

  void check(const Input &I) {
    std::vector<uint8_t> Bytes;
    if (I.Width != 1 && (I.Width == 4) != I.Code32)
      Bytes.push_back(0x66);
    if (I.Address32 != I.Code32)
      Bytes.push_back(0x67);
    if (I.Prefix)
      Bytes.push_back(I.Prefix);
    const uint8_t Opcode = I.Form == 0 ? 0xd0 : I.Form == 1 ? 0xc0 : 0xd2;
    Bytes.push_back(Opcode + (I.Width != 1));
    Bytes.push_back((I.Group << 3) | (I.Register >= 0 ? 0xc0 | I.Register
                                      : I.Address32   ? 3
                                                      : 7));
    if (I.Form == 1)
      Bytes.push_back(I.Count);
    const uint32_t Offset = I.Case == 7 || I.Case == 8 ? 0xc101
                            : I.Case == 16             ? 0xcfff
                                                       : 0xc100;
    std::array<uint32_t, 8> Registers{
        0x12345678, 0xa5a50000 | I.Count, 0x22334455, Offset, 0xd100, Offset, 0,
        Offset};
    if (I.Register >= 0) {
      const unsigned Index = I.Width == 1 ? I.Register & 3 : I.Register;
      Registers[Index] = I.Value;
    }
    const unsigned Count = I.Form == 0   ? 1
                           : I.Form == 1 ? I.Count
                                         : uint8_t(Registers[1]);
    const uint32_t Value = I.Register < 0
                               ? memoryInputValue(I.Case, I.Prefix, I.Value)
                           : I.Width == 1 && I.Register >= 4 ? I.Value >> 8
                                                             : I.Value;
    ASSERT_NO_FATAL_FAILURE(bindUndefinedFlags(I, Value, Count));
    ASSERT_NO_FATAL_FAILURE(compare(Bytes, I.Code32, true, I.CPL, I.Case,
                                    I.Value, I.Count, I.Flags, 0x4000,
                                    0x22334455, Code, Registers));
  }
};

TEST_P(X86LegacyShiftNativeTest, EveryByteCountAndOperandWidth) {
  for (bool Code32 : {false, true})
    for (unsigned Width : {1u, 2u, 4u})
      for (unsigned Group : {0u, 1u, 2u, 3u, 4u, 5u, 7u})
        for (unsigned Count = 0; Count != 256; ++Count)
          ASSERT_NO_FATAL_FAILURE(
              check({.Group = Group,
                     .Width = Width,
                     .Count = Count,
                     .Code32 = Code32,
                     .Value = 0x81a5c37f,
                     .Flags = Count & 1 ? 0xed7u : 0x202u}));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyShiftNativeTest, BoundaryValuesAndAllCountEncodings) {
  for (bool Code32 : {false, true})
    for (unsigned Width : {1u, 2u, 4u}) {
      const uint32_t Mask = UINT32_MAX >> (32 - Width * 8);
      const uint32_t Sign = (Mask >> 1) + 1;
      for (unsigned Group : {0u, 1u, 2u, 3u, 4u, 5u, 7u})
        for (unsigned Form = 0; Form != 3; ++Form)
          for (unsigned Count : {0u, 1u, 8u, 9u, 16u, 17u, 31u, 32u}) {
            if (!Form && Count != 1)
              continue;
            for (uint32_t Value : {0u, 1u, Sign - 1, Sign, Mask})
              for (uint32_t Flags : {0x202u, 0xed7u})
                ASSERT_NO_FATAL_FAILURE(check({.Group = Group,
                                               .Width = Width,
                                               .Form = Form,
                                               .Count = Count,
                                               .Code32 = Code32,
                                               .Value = Value,
                                               .Flags = Flags}));
          }
    }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyShiftNativeTest, EveryRegisterIncludesCLAndHighByteAliases) {
  for (bool Code32 : {false, true})
    for (unsigned Width : {1u, 2u, 4u})
      for (unsigned Group : {0u, 1u, 2u, 3u, 4u, 5u, 7u})
        for (int Register = 0; Register != 8; ++Register)
          for (unsigned Count : {0u, 1u, 31u})
            ASSERT_NO_FATAL_FAILURE(check({.Group = Group,
                                           .Width = Width,
                                           .Count = Count,
                                           .Code32 = Code32,
                                           .Register = Register,
                                           .CPL = unsigned(Register) & 3}));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyShiftNativeTest, MemoryZeroCountsAndOperandFaults) {
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (unsigned Width : {1u, 2u, 4u})
        for (unsigned Group : {0u, 1u, 2u, 3u, 4u, 5u, 7u})
          for (unsigned Count : {0u, 1u, 17u, 32u})
            for (unsigned Case : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 16u})
              for (unsigned CPL : {0u, 3u})
                ASSERT_NO_FATAL_FAILURE(check({.Group = Group,
                                               .Width = Width,
                                               .Count = Count,
                                               .Code32 = Code32,
                                               .Address32 = Address32,
                                               .Register = -1,
                                               .Case = Case,
                                               .Prefix = Case == 6 ? 0x36u : 0,
                                               .CPL = CPL,
                                               .Flags = 0x40ed7}));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyShiftNativeTest, SegmentOverridesAndOriginalPageRepair) {
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (unsigned Width : {1u, 2u, 4u})
        for (unsigned Group : {0u, 1u, 2u, 3u, 4u, 5u, 7u})
          for (unsigned Prefix : {0u, 0x26u, 0x2eu, 0x36u, 0x3eu, 0x64u, 0x65u})
            for (unsigned Case : {0u, 3u, 12u}) {
              if (Case == 12 && Prefix == 0x2e)
                continue; // A CS write faults before the missing page.
              ASSERT_NO_FATAL_FAILURE(check({.Group = Group,
                                             .Width = Width,
                                             .Code32 = Code32,
                                             .Address32 = Address32,
                                             .Register = -1,
                                             .Case = Case,
                                             .Prefix = Prefix}));
            }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyShiftNativeTest, NonflatSSUsesItsOwnOriginalInput) {
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (unsigned Width : {1u, 2u, 4u})
        for (unsigned Group : {0u, 1u, 2u, 3u, 4u, 5u, 7u})
          for (unsigned Form : {1u, 2u})
            for (unsigned Count : {2u, 8u, 17u, 31u})
              ASSERT_NO_FATAL_FAILURE(check({.Group = Group,
                                             .Width = Width,
                                             .Form = Form,
                                             .Count = Count,
                                             .Code32 = Code32,
                                             .Address32 = Address32,
                                             .Register = -1,
                                             .Case = 3,
                                             .Prefix = 0x36}));
  RecordProperty("cpu_comparisons", Comparisons);
}

INSTANTIATE_TEST_SUITE_P(LegacyShifts, X86LegacyShiftNativeTest,
                         testing::Values(2, 3));
