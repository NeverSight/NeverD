//===- X86LegacyDoubleShiftNativeTests.inc - Original SHLD/SHRD ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original protected double shifts, aliases, undefined inputs and restart.
//===----------------------------------------------------------------------===//

#include "X86LegacyShiftNativeFixture.inc"

class X86LegacyDoubleShiftNativeTest : public X86LegacyShiftNativeFixture {
protected:
  struct Input {
    bool Right = false;
    unsigned Width = 4;
    bool Immediate = false;
    unsigned Count = 1;
    bool Code32 = true, Address32 = true;
    int Register = 0;
    unsigned SourceRegister = 2, Case = 0, Prefix = 0, CPL = 3;
    uint32_t Value = 0x87654321, Source = 0x01234567, Flags = 0xed7;
  };

  void check(const Input &I) {
    std::vector<uint8_t> Bytes;
    if ((I.Width == 4) != I.Code32)
      Bytes.push_back(0x66);
    if (I.Address32 != I.Code32)
      Bytes.push_back(0x67);
    if (I.Prefix)
      Bytes.push_back(I.Prefix);
    const uint8_t Opcode = (I.Right ? 0xac : 0xa4) + !I.Immediate;
    Bytes.insert(Bytes.end(), {0x0f, Opcode,
                               uint8_t((I.SourceRegister << 3) |
                                       (I.Register >= 0 ? 0xc0 | I.Register
                                        : I.Address32   ? 3
                                                        : 7))});
    if (I.Immediate)
      Bytes.push_back(I.Count);
    const uint32_t Offset = I.Case == 7 || I.Case == 8 ? 0xc101
                            : I.Case == 16             ? 0xcfff
                                                       : 0xc100;
    std::array<uint32_t, 8> Registers{
        0x12345678, 0xa5a50000 | I.Count, 0x22334455, Offset, 0xd100, Offset, 0,
        Offset};
    Registers[I.SourceRegister] = I.Source;
    if (I.Register >= 0)
      Registers[I.Register] = I.Value;
    const unsigned Count = I.Immediate ? I.Count : uint8_t(Registers[1]);
    std::vector<uint8_t> Probe;
    if (I.Width == 2)
      Probe.push_back(0x66);
    Probe.insert(Probe.end(),
                 {0x0f, Opcode, uint8_t(I.Register < 0 ? 0x34 : 0xf0)});
    if (I.Register < 0)
      Probe.push_back(0x24); // [RSP], ESI/SI; otherwise EAX/AX, ESI/SI.
    if (I.Immediate)
      Probe.push_back(I.Count);
    const uint32_t MemoryValue = memoryInputValue(I.Case, I.Prefix, I.Value);
    uint64_t Observed;
    ASSERT_NO_FATAL_FAILURE(observe(
        Probe, I.Register >= 0 ? Registers[I.Register] : MemoryValue,
        Registers[I.SourceRegister], Count, I.Flags, I.Register < 0, Observed));
    DoubleShiftUndefined = {uint16_t(Observed),
                            uint32_t(Observed >> 32) &
                                kX86LegacyArithmeticFlagsMask};
    ASSERT_NO_FATAL_FAILURE(compare(Bytes, I.Code32, true, I.CPL, I.Case,
                                    I.Value, I.Count, I.Flags, 0x4000,
                                    0x22334455, Code, Registers));
  }
};

TEST_P(X86LegacyDoubleShiftNativeTest, EveryByteCountAndOperandWidth) {
  for (bool Code32 : {false, true})
    for (unsigned Width : {2u, 4u})
      for (bool Right : {false, true})
        for (bool Immediate : {false, true})
          for (unsigned Count = 0; Count != 256; ++Count)
            ASSERT_NO_FATAL_FAILURE(
                check({.Right = Right,
                       .Width = Width,
                       .Immediate = Immediate,
                       .Count = Count,
                       .Code32 = Code32,
                       .Flags = Count & 1 ? 0xed7u : 0x202u}));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyDoubleShiftNativeTest, BoundaryValuesAndDefinedFlags) {
  for (bool Code32 : {false, true})
    for (unsigned Width : {2u, 4u}) {
      const uint32_t Mask = UINT32_MAX >> (32 - Width * 8);
      const uint32_t Sign = (Mask >> 1) + 1;
      for (bool Right : {false, true})
        for (bool Immediate : {false, true})
          for (unsigned Count : {0u, 1u, 15u, 16u, 17u, 31u, 32u, 255u})
            for (uint32_t Value : {0u, 1u, Sign, Mask})
              for (uint32_t Source : {0u, 1u, Sign, Mask})
                for (uint32_t Flags : {0x202u, 0xed7u})
                  ASSERT_NO_FATAL_FAILURE(check({.Right = Right,
                                                 .Width = Width,
                                                 .Immediate = Immediate,
                                                 .Count = Count,
                                                 .Code32 = Code32,
                                                 .Value = Value,
                                                 .Source = Source,
                                                 .Flags = Flags}));
    }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyDoubleShiftNativeTest, EveryRegisterAndSourceCountAlias) {
  for (bool Code32 : {false, true})
    for (unsigned Width : {2u, 4u})
      for (bool Right : {false, true})
        for (bool Immediate : {false, true})
          for (int Register = 0; Register != 8; ++Register)
            for (unsigned Source = 0; Source != 8; ++Source)
              for (unsigned Count : {1u, 16u, 17u, 32u})
                ASSERT_NO_FATAL_FAILURE(check({.Right = Right,
                                               .Width = Width,
                                               .Immediate = Immediate,
                                               .Count = Count,
                                               .Code32 = Code32,
                                               .Register = Register,
                                               .SourceRegister = Source,
                                               .CPL = unsigned(Register) & 3,
                                               .Value = 0x87654300 | Count,
                                               .Source = 0x01234500 | Count}));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyDoubleShiftNativeTest, MemoryZeroCountsAndOperandFaults) {
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (unsigned Width : {2u, 4u})
        for (bool Right : {false, true})
          for (bool Immediate : {false, true})
            for (unsigned Count : {0u, 1u, 16u, 17u, 32u})
              for (unsigned Case : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 16u})
                for (unsigned CPL : {0u, 3u})
                  ASSERT_NO_FATAL_FAILURE(
                      check({.Right = Right,
                             .Width = Width,
                             .Immediate = Immediate,
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

TEST_P(X86LegacyDoubleShiftNativeTest, SegmentOverridesAndOriginalPageRepair) {
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (unsigned Width : {2u, 4u})
        for (bool Right : {false, true})
          for (bool Immediate : {false, true})
            for (unsigned Count : {0u, 16u, 17u})
              for (unsigned Prefix :
                   {0u, 0x26u, 0x2eu, 0x36u, 0x3eu, 0x64u, 0x65u})
                for (unsigned Case : {0u, 3u, 12u}) {
                  if (Case == 12 && Prefix == 0x2e)
                    continue;
                  for (unsigned Source : {2u, 3u})
                    ASSERT_NO_FATAL_FAILURE(
                        check({.Right = Right,
                               .Width = Width,
                               .Immediate = Immediate,
                               .Count = Count,
                               .Code32 = Code32,
                               .Address32 = Address32,
                               .Register = -1,
                               .SourceRegister = Source,
                               .Case = Case,
                               .Prefix = Prefix,
                               .Source = Source == 3 ? 0xc100u : 0x01234567u}));
                }
  RecordProperty("cpu_comparisons", Comparisons);
}

INSTANTIATE_TEST_SUITE_P(LegacyDoubleShifts, X86LegacyDoubleShiftNativeTest,
                         testing::Values(2, 3));
