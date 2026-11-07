//===- X86LegacyBitNativeTests.inc - Original protected bit operations ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent CPU bit strings, scans, operand boundaries and original retry.
//===----------------------------------------------------------------------===//

#include "X86LegacyScalarNativeObserverFixture.inc"

class X86LegacyBitNativeTest : public X86LegacyScalarNativeObserverFixture {
protected:
  struct BitInput {
    unsigned Operation = 0, Width = 4;
    bool Immediate = false;
    uint32_t Index = 1;
    bool Code32 = true, Address32 = true;
    int Register = 0;
    unsigned SourceRegister = 1, Case = 0, Prefix = 0, CPL = 3;
    uint32_t Value = 0x87654321, Flags = 0xed7;
    bool CompensateBase = true;
  };

  struct ScanInput {
    bool Reverse = false;
    unsigned Width = 4;
    bool Code32 = true, Address32 = true;
    unsigned Destination = 0;
    int Source = 1;
    unsigned Case = 0, Prefix = 0, CPL = 3;
    uint32_t Value = 0x87654321, Original = 0x12345678, Flags = 0xed7;
  };

  void checkBit(const BitInput &I) {
    SCOPED_TRACE(testing::Message() << "operation=" << I.Operation
                                    << "index=" << std::hex << I.Index);
    std::vector<uint8_t> Bytes;
    if ((I.Width == 4) != I.Code32)
      Bytes.push_back(0x66);
    if (I.Address32 != I.Code32)
      Bytes.push_back(0x67);
    if (I.Prefix)
      Bytes.push_back(I.Prefix);
    Bytes.insert(
        Bytes.end(),
        {0x0f, uint8_t(I.Immediate ? 0xba : 0xa3 + 8 * I.Operation),
         uint8_t(((I.Immediate ? 4 + I.Operation : I.SourceRegister) << 3) |
                 (I.Register >= 0 ? 0xc0 | I.Register
                  : I.Address32   ? 3
                                  : 7))});
    if (I.Immediate)
      Bytes.push_back(I.Index);
    const uint32_t Offset = I.Case == 7 || I.Case == 8 ? 0xc101
                            : I.Case == 16             ? 0xcfff
                                                       : 0xc100;
    std::array<uint32_t, 8> Registers{
        0x12345678, 0x33445566, 0x22334455, Offset, 0xd100, Offset, 0, Offset};
    if (I.Register < 0 && !I.Immediate && I.CompensateBase) {
      // Position the base so the selected word is the initialized operand.
      // Use signed division/remainder, independently of the executor's shift.
      const int64_t Index = I.Width == 2 ? int16_t(I.Index) : int32_t(I.Index);
      const int64_t Bits = I.Width * 8;
      const int64_t Word = Index / Bits - (Index % Bits < 0);
      Registers[3] = Offset - Word * I.Width;
    }
    if (!I.Immediate)
      Registers[I.SourceRegister] = I.Index;
    if (I.Register >= 0)
      Registers[I.Register] = I.Value;
    // An explicit measured target choice, not an architectural preservation
    // rule. Neither CF nor any output from the original KVM run is supplied.
    BitTestUndefinedFlags = I.Flags & kX86LegacyBitTestUndefinedFlagsMask;
    ASSERT_NO_FATAL_FAILURE(compare(Bytes, I.Code32, true, I.CPL, I.Case,
                                    I.Value, I.Index, I.Flags, 0x4000,
                                    0x22334455, Code, Registers));
  }

  void checkScan(const ScanInput &I) {
    std::vector<uint8_t> Bytes;
    if ((I.Width == 4) != I.Code32)
      Bytes.push_back(0x66);
    if (I.Address32 != I.Code32)
      Bytes.push_back(0x67);
    if (I.Prefix)
      Bytes.push_back(I.Prefix);
    Bytes.insert(Bytes.end(), {0x0f, uint8_t(I.Reverse ? 0xbd : 0xbc),
                               uint8_t((I.Destination << 3) |
                                       (I.Source >= 0 ? 0xc0 | I.Source
                                        : I.Address32 ? 3
                                                      : 7))});
    const uint32_t Offset = I.Case == 7 || I.Case == 8 ? 0xc101
                            : I.Case == 16             ? 0xcfff
                                                       : 0xc100;
    std::array<uint32_t, 8> Registers{
        0x12345678, 0x33445566, 0x22334455, Offset, 0xd100, Offset, 0, Offset};
    Registers[I.Destination] = I.Original;
    if (I.Source >= 0)
      Registers[I.Source] = I.Value;
    // The memory form uses an independent stack source and retains its EAX
    // destination. Capture before packing, then restore the original flags.
    const bool Memory = I.Source < 0;
    std::vector<uint8_t> Probe{0x9c, 0x89, 0xf8};
    if (Memory)
      Probe.push_back(0x56);
    Probe.insert(Probe.end(), {0x51, 0x9d});
    if (I.Width == 2)
      Probe.push_back(0x66);
    Probe.insert(Probe.end(), {0x0f, uint8_t(I.Reverse ? 0xbd : 0xbc),
                               uint8_t(Memory ? 0x04 : 0xc6)});
    if (Memory)
      Probe.insert(Probe.end(), {0x24, 0x48, 0x8d, 0x64, 0x24, 8});
    Probe.insert(Probe.end(), {0x9c, 0x5a, 0x48, 0xc1, 0xe2, 0x20, 0x48, 0x09,
                               0xd0, 0x9d, 0xc3});
    const uint32_t Source =
        Memory
            ? (I.Case == 3 && (I.Prefix == 0x36 || I.Prefix == 0x2e) ? 0
                                                                     : I.Value)
            : Registers[I.Source];
    uint64_t Observed;
    ASSERT_NO_FATAL_FAILURE(observeCode(Probe, Registers[I.Destination], Source,
                                        0, I.Flags, Observed));
    BitScanChoices = {uint32_t(Observed), uint32_t(Observed >> 32) &
                                              kX86LegacyBitScanChoiceFlagsMask};
    ASSERT_NO_FATAL_FAILURE(compare(Bytes, I.Code32, true, I.CPL, I.Case,
                                    I.Value, 0, I.Flags, 0x4000, 0x22334455,
                                    Code, Registers));
  }
};

TEST_P(X86LegacyBitNativeTest, EveryImmediateAndRegisterBitIndex) {
  for (bool Code32 : {false, true})
    for (unsigned Width : {2u, 4u})
      for (unsigned Operation = 0; Operation != 4; ++Operation)
        for (bool Immediate : {false, true})
          for (unsigned Index = 0; Index != 256; ++Index)
            ASSERT_NO_FATAL_FAILURE(
                checkBit({.Operation = Operation,
                          .Width = Width,
                          .Immediate = Immediate,
                          .Index = Index,
                          .Code32 = Code32,
                          .Flags = Index & 1 ? 0xed7u : 0x202u}));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyBitNativeTest, EveryRegisterAndIndexAlias) {
  for (unsigned Width : {2u, 4u})
    for (unsigned Operation = 0; Operation != 4; ++Operation)
      for (int Register = 0; Register != 8; ++Register)
        for (unsigned IndexRegister = 0; IndexRegister != 8; ++IndexRegister)
          for (uint32_t Index : {0u, 15u, 31u, 0xffffffffu})
            ASSERT_NO_FATAL_FAILURE(checkBit({.Operation = Operation,
                                              .Width = Width,
                                              .Index = Index,
                                              .Code32 = bool(Register & 1),
                                              .Register = Register,
                                              .SourceRegister = IndexRegister,
                                              .CPL = IndexRegister & 3,
                                              .Value = 0x8000801f}));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyBitNativeTest, SignedMemoryIndicesAndAddressWrap) {
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (unsigned Width : {2u, 4u})
        for (unsigned Operation = 0; Operation != 4; ++Operation)
          for (uint32_t Index :
               {0u, 15u, 16u, 31u, 32u, 65u, 0xffffffbfu, 0xffffffdfu,
                0xfffffff0u, 0xffffffffu, 0x8000u, 0x7fffu, 0x80000000u,
                0x7fffffffu, 0x80000u, 0xfff80000u})
            for (unsigned Case : {0u, 3u})
              ASSERT_NO_FATAL_FAILURE(checkBit({.Operation = Operation,
                                                .Width = Width,
                                                .Index = Index,
                                                .Code32 = Code32,
                                                .Address32 = Address32,
                                                .Register = -1,
                                                .Case = Case,
                                                .Value = 0xffffffff}));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyBitNativeTest, MemoryImmediateHighBitsAndBaseIndexAlias) {
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (unsigned Width : {2u, 4u})
        for (unsigned Operation = 0; Operation != 4; ++Operation) {
          for (unsigned Index = 0; Index != 256; ++Index)
            ASSERT_NO_FATAL_FAILURE(checkBit({.Operation = Operation,
                                              .Width = Width,
                                              .Immediate = true,
                                              .Index = Index,
                                              .Code32 = Code32,
                                              .Address32 = Address32,
                                              .Register = -1}));
          ASSERT_NO_FATAL_FAILURE(checkBit({.Operation = Operation,
                                            .Width = Width,
                                            .Index = 0xaba8,
                                            .Code32 = Code32,
                                            .Address32 = Address32,
                                            .Register = -1,
                                            .SourceRegister = 3,
                                            .CompensateBase = false}));
        }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyBitNativeTest, ReadWriteFaultsAndOriginalPageRepair) {
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (unsigned Width : {2u, 4u})
        for (unsigned Operation = 0; Operation != 4; ++Operation)
          for (bool Immediate : {false, true})
            for (unsigned Case : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 12u, 16u})
              for (unsigned CPL : {0u, 3u})
                ASSERT_NO_FATAL_FAILURE(
                    checkBit({.Operation = Operation,
                              .Width = Width,
                              .Immediate = Immediate,
                              .Index = Immediate ? 255u : 0xffffffffu,
                              .Code32 = Code32,
                              .Address32 = Address32,
                              .Register = -1,
                              .Case = Case,
                              .Prefix = Case == 6 ? 0x36u : 0,
                              .CPL = CPL,
                              .Flags = 0x40ed7}));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyBitNativeTest, ScansEveryBitZeroAndRegisterAlias) {
  for (bool Code32 : {false, true})
    for (unsigned Width : {2u, 4u})
      for (bool Reverse : {false, true}) {
        for (unsigned Bit = 0; Bit != Width * 8; ++Bit)
          for (uint32_t Value : {uint32_t(1) << Bit, (uint32_t(1) << Bit) | 1,
                                 ~(uint32_t(1) << Bit)})
            for (uint32_t Flags : {0x202u, 0xed7u})
              ASSERT_NO_FATAL_FAILURE(checkScan({.Reverse = Reverse,
                                                 .Width = Width,
                                                 .Code32 = Code32,
                                                 .Value = Value,
                                                 .Flags = Flags}));
        for (unsigned Destination = 0; Destination != 8; ++Destination)
          for (int Source = 0; Source != 8; ++Source)
            for (uint32_t Value : {0u, 0x80008000u, 0xffffffffu})
              ASSERT_NO_FATAL_FAILURE(checkScan({.Reverse = Reverse,
                                                 .Width = Width,
                                                 .Code32 = Code32,
                                                 .Destination = Destination,
                                                 .Source = Source,
                                                 .CPL = Destination & 3,
                                                 .Value = Value}));
      }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyBitNativeTest, ScansMemoryZeroFaultsAndOriginalPageRepair) {
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (unsigned Width : {2u, 4u})
        for (bool Reverse : {false, true})
          for (uint32_t Value : {0u, 1u, 0x80008000u, 0xffffffffu})
            for (unsigned Case : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 12u, 16u})
              for (unsigned CPL : {0u, 3u})
                ASSERT_NO_FATAL_FAILURE(
                    checkScan({.Reverse = Reverse,
                               .Width = Width,
                               .Code32 = Code32,
                               .Address32 = Address32,
                               .Source = -1,
                               .Case = Case,
                               .Prefix = Case == 6 ? 0x36u : 0,
                               .CPL = CPL,
                               .Value = Value,
                               .Flags = 0x40ed7}));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyBitNativeTest, MemorySegmentOverrides) {
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (unsigned Width : {2u, 4u})
        for (unsigned Prefix : {0u, 0x26u, 0x2eu, 0x36u, 0x3eu, 0x64u, 0x65u})
          for (unsigned Case : {0u, 3u}) {
            for (unsigned Operation = 0; Operation != 4; ++Operation)
              ASSERT_NO_FATAL_FAILURE(checkBit({.Operation = Operation,
                                                .Width = Width,
                                                .Index = 0xfffffff1,
                                                .Code32 = Code32,
                                                .Address32 = Address32,
                                                .Register = -1,
                                                .Case = Case,
                                                .Prefix = Prefix}));
            for (bool Reverse : {false, true})
              ASSERT_NO_FATAL_FAILURE(checkScan({.Reverse = Reverse,
                                                 .Width = Width,
                                                 .Code32 = Code32,
                                                 .Address32 = Address32,
                                                 .Source = -1,
                                                 .Case = Case,
                                                 .Prefix = Prefix}));
          }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyBitNativeTest, ScanDestinationCanBeItsMemoryAddressRegister) {
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (unsigned Width : {2u, 4u})
        for (bool Reverse : {false, true})
          for (unsigned Destination = 0; Destination != 8; ++Destination)
            for (uint32_t Value : {0u, 1u, 0x80008000u})
              ASSERT_NO_FATAL_FAILURE(
                  checkScan({.Reverse = Reverse,
                             .Width = Width,
                             .Code32 = Code32,
                             .Address32 = Address32,
                             .Destination = Destination,
                             .Source = -1,
                             .Value = Value,
                             .Original = Address32 ? 0xc100u : 0xabcdc100u}));
  RecordProperty("cpu_comparisons", Comparisons);
}

INSTANTIATE_TEST_SUITE_P(LegacyBits, X86LegacyBitNativeTest,
                         testing::Values(2, 3));
