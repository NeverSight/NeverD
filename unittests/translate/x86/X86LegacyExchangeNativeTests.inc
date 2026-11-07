//===- X86LegacyExchangeNativeTests.inc - Original exchanges and LOCK ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// CPU register aliases, locked scalar families, failed writes and retries.
//===----------------------------------------------------------------------===//

#include "X86LegacyScalarNativeFixture.inc"

class X86LegacyExchangeNativeTest : public X86LegacyScalarNativeFixture {
protected:
  struct Input {
    unsigned Operation = 0, Width = 4;
    bool Code32 = true, Address32 = true, Locked = false;
    int Destination = 0;
    unsigned Source = 1, Case = 0, Prefix = 0, CPL = 3;
    uint32_t Value = 0x12345678, Replacement = 0x87654321;
    uint32_t Expected = 0x12345678, High = 0x22334455;
    uint32_t ExpectedHigh = 0x22334455, Flags = 0xed7;
    bool Short = false;
  };

  void check(const Input &I) {
    SCOPED_TRACE(testing::Message() << "op=" << I.Operation << "width="
                                    << I.Width << "lock=" << I.Locked);
    Atomics = X86LegacyAtomicProfile::ExclusiveRAM;
    CheckOperandPageEffects = true;
    MemoryHigh = I.Operation == 3 ? std::optional(I.High) : std::nullopt;
    const uint32_t Offset = I.Case == 7 || I.Case == 8 ? 0xc101
                            : I.Case == 16             ? 0xcfff
                                                       : 0xc100;
    std::array<uint32_t, 8> Registers{0x12345678, 0x33445566, 0x22334455,
                                      Offset,     0xd100,     Offset,
                                      0xabcdefff, Offset};
    const auto Set = [&](unsigned R, uint32_t Value) {
      if (I.Width == 1) {
        const unsigned Shift = R >= 4 ? 8 : 0;
        Registers[R % 4] =
            (Registers[R % 4] & ~(0xffu << Shift)) | ((Value & 0xffu) << Shift);
      } else {
        Registers[R] = Value;
      }
    };
    Set(I.Source, I.Replacement);
    if (I.Destination >= 0)
      Set(I.Destination, I.Value);
    if (I.Operation >= 2) {
      Registers[0] = I.Expected;
      if (I.Operation == 3)
        Registers[2] = I.ExpectedHigh;
    }
    if (I.Destination < 0)
      Registers[3] = Offset;
    std::vector<uint8_t> Bytes;
    if (I.Width != 1 && I.Width != 8 && (I.Width == 4) != I.Code32)
      Bytes.push_back(0x66);
    if (I.Address32 != I.Code32)
      Bytes.push_back(0x67);
    if (I.Prefix)
      Bytes.push_back(I.Prefix);
    if (I.Locked)
      Bytes.push_back(0xf0);
    if (I.Short) {
      Bytes.push_back(0x90 | I.Source);
    } else {
      if (I.Operation)
        Bytes.push_back(0x0f);
      Bytes.push_back(I.Operation == 3   ? 0xc7
                      : I.Operation == 2 ? 0xb0 + (I.Width != 1)
                      : I.Operation == 1 ? 0xc0 + (I.Width != 1)
                                         : 0x86 + (I.Width != 1));
      Bytes.push_back(((I.Operation == 3 ? 1 : I.Source) << 3) |
                      (I.Destination >= 0 ? 0xc0 | I.Destination
                       : I.Address32      ? 3
                                          : 7));
    }
    ASSERT_NO_FATAL_FAILURE(compare(Bytes, I.Code32, true, I.CPL, I.Case,
                                    I.Value, I.Replacement, I.Flags, 0x4000,
                                    I.High, Code, Registers));
  }
};

TEST_P(X86LegacyExchangeNativeTest, EveryRegisterLaneAndOverlap) {
  for (bool Code32 : {false, true})
    for (unsigned Width : {1u, 2u, 4u})
      for (unsigned Operation = 0; Operation != 3; ++Operation)
        for (int Destination = 0; Destination != 8; ++Destination)
          for (unsigned Source = 0; Source != 8; ++Source)
            for (bool Equal : {false, true})
              ASSERT_NO_FATAL_FAILURE(
                  check({.Operation = Operation,
                         .Width = Width,
                         .Code32 = Code32,
                         .Destination = Destination,
                         .Source = Source,
                         .CPL = Source & 3,
                         .Expected = Equal ? 0x12345678u : 0xfedcba98u}));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyExchangeNativeTest, AccumulatorShortForms) {
  for (bool Code32 : {false, true})
    for (unsigned Width : {2u, 4u})
      for (unsigned Source = 0; Source != 8; ++Source)
        for (uint32_t Value : {0u, 0xffffffffu, 0x80008000u})
          ASSERT_NO_FATAL_FAILURE(check({.Width = Width,
                                         .Code32 = Code32,
                                         .Source = Source,
                                         .Value = Value,
                                         .Short = true}));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyExchangeNativeTest, CarryOverflowAndComparisonFlags) {
  for (unsigned Width : {1u, 2u, 4u})
    for (unsigned Operation : {1u, 2u})
      for (uint32_t A : {0u, 1u, 0x7fu, 0x80u, 0xffu, 0x7fffu, 0x8000u, 0xffffu,
                         0x7fffffffu, 0x80000000u, 0xffffffffu})
        for (uint32_t B : {0u, 1u, 0x7fu, 0x80u, 0xffu, 0x7fffu, 0x8000u,
                           0xffffu, 0x7fffffffu, 0x80000000u, 0xffffffffu})
          for (uint32_t Flags : {0x202u, 0xed7u})
            ASSERT_NO_FATAL_FAILURE(check({.Operation = Operation,
                                           .Width = Width,
                                           .Destination = 2,
                                           .Value = A,
                                           .Replacement = B,
                                           .Expected = B,
                                           .Flags = Flags}));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyExchangeNativeTest, MemoryWidthsSegmentsAndSourceBaseOverlap) {
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (unsigned Width : {1u, 2u, 4u})
        for (unsigned Operation = 0; Operation != 3; ++Operation)
          for (bool Locked : {false, true})
            for (unsigned Prefix :
                 {0u, 0x26u, 0x2eu, 0x36u, 0x3eu, 0x64u, 0x65u})
              for (unsigned Source : {0u, 3u, 4u, 7u})
                ASSERT_NO_FATAL_FAILURE(check({.Operation = Operation,
                                               .Width = Width,
                                               .Code32 = Code32,
                                               .Address32 = Address32,
                                               .Locked = Locked,
                                               .Destination = -1,
                                               .Source = Source,
                                               .Case = 3,
                                               .Prefix = Prefix}));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyExchangeNativeTest, MemoryFaultsAndOriginalHandlerRetry) {
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (unsigned Operation = 0; Operation != 4; ++Operation)
        for (unsigned Width : {1u, 2u, 4u, 8u}) {
          if ((Operation == 3) != (Width == 8))
            continue;
          for (bool Locked : {false, true})
            for (unsigned Case : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 12u, 16u})
              for (unsigned CPL : {0u, 3u})
                for (bool Equal : {false, true})
                  ASSERT_NO_FATAL_FAILURE(
                      check({.Operation = Operation,
                             .Width = Width,
                             .Code32 = Code32,
                             .Address32 = Address32,
                             .Locked = Locked,
                             .Destination = -1,
                             .Case = Case,
                             .Prefix = Case == 6 ? 0x36u : 0,
                             .CPL = CPL,
                             .Expected = Equal ? 0x12345678u : 0x87654321u,
                             .Flags = 0x40ed7}));
        }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyExchangeNativeTest, PairedComparisonsUseBothHalves) {
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (bool Locked : {false, true})
        for (uint32_t Low : {0u, 0xffffffffu, 0x12345678u})
          for (uint32_t High : {0u, 0xffffffffu, 0x87654321u})
            for (uint32_t ExpectedLow : {0u, 0xffffffffu, 0x12345678u})
              for (uint32_t ExpectedHigh : {0u, 0xffffffffu, 0x87654321u})
                for (uint32_t Flags : {0x202u, 0xed7u})
                  ASSERT_NO_FATAL_FAILURE(check({.Operation = 3,
                                                 .Width = 8,
                                                 .Code32 = Code32,
                                                 .Address32 = Address32,
                                                 .Locked = Locked,
                                                 .Destination = -1,
                                                 .Value = Low,
                                                 .Expected = ExpectedLow,
                                                 .High = High,
                                                 .ExpectedHigh = ExpectedHigh,
                                                 .Flags = Flags}));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyExchangeNativeTest, EveryLockedArithmeticAndBitFamily) {
  Atomics = X86LegacyAtomicProfile::ExclusiveRAM;
  CheckOperandPageEffects = true;
  for (bool Code32 : {false, true})
    for (unsigned Width : {1u, 2u, 4u})
      for (unsigned Family = 0; Family != 14; ++Family) {
        if (Family >= 11 && Width == 1)
          continue;
        for (unsigned Case : {0u, 1u, 2u, 3u, 4u, 5u, 7u, 8u, 12u, 16u})
          for (uint32_t Flags : {0x40202u, 0x40ed7u}) {
            std::vector<uint8_t> Bytes{0xf0};
            if (Width != 1 && (Width == 4) != Code32)
              Bytes.push_back(0x66);
            if (!Code32)
              Bytes.push_back(0x67);
            if (Family < 7) {
              constexpr unsigned Opcodes[]{0x00, 0x08, 0x10, 0x18,
                                           0x20, 0x28, 0x30};
              Bytes.insert(Bytes.end(),
                           {uint8_t(Opcodes[Family] + (Width != 1)), 0x0b});
            } else if (Family < 11) {
              const bool Unary = Family >= 9;
              Bytes.insert(Bytes.end(),
                           {uint8_t((Unary ? 0xf6 : 0xfe) + (Width != 1)),
                            uint8_t(3 | (Family - 7) << 3)});
            } else {
              Bytes.insert(Bytes.end(),
                           {0x0f, uint8_t(0xab + (Family - 11) * 8), 0x0b});
              BitTestUndefinedFlags =
                  Flags & kX86LegacyBitTestUndefinedFlagsMask;
            }
            ASSERT_NO_FATAL_FAILURE(
                compare(Bytes, Code32, true, 3, Case, 0x80008080, 1, Flags));
          }
      }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyExchangeNativeTest,
       PairedImplicitBasesAndIgnoredOperandPrefix) {
  Atomics = X86LegacyAtomicProfile::ExclusiveRAM;
  CheckOperandPageEffects = true;
  for (bool Code32 : {false, true})
    for (unsigned Base : {0u, 2u, 3u, 4u, 5u})
      for (bool Override : {false, true})
        for (bool Locked : {false, true})
          for (bool Equal : {false, true}) {
            std::array<uint32_t, 8> Registers{
                0x12345678, 0x87654321, 0xaabbccdd, 0x11223344,
                0xd100,     0xc100,     0,          0};
            Registers[Base] = 0xc100;
            MemoryHigh = Registers[2];
            std::vector<uint8_t> Bytes;
            if (Locked)
              Bytes.push_back(0xf0);
            if (!Code32)
              Bytes.push_back(0x67);
            if (Override)
              Bytes.push_back(0x66);
            Bytes.insert(Bytes.end(),
                         {0x0f, 0xc7, uint8_t(Base == 5 ? 0x4d : 8 | Base)});
            if (Base == 4)
              Bytes.push_back(0x24);
            if (Base == 5)
              Bytes.push_back(0);
            ASSERT_NO_FATAL_FAILURE(compare(Bytes, Code32, true, 3, 0,
                                            Registers[0] ^ unsigned(!Equal), 0,
                                            0xed7, 0x4000, 0, Code, Registers));
          }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyExchangeNativeTest, LockedImmediateArithmeticAndBitForms) {
  Atomics = X86LegacyAtomicProfile::ExclusiveRAM;
  CheckOperandPageEffects = true;
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (unsigned Width : {1u, 2u, 4u})
        for (unsigned Group = 0; Group != 10; ++Group) {
          if (Group >= 7 && Width == 1)
            continue;
          for (uint8_t Immediate : {0, 1, 15, 16, 31, 32, 127, 128, 255})
            for (bool Short : {false, true}) {
              if (Short && (Width == 1 || Group >= 7))
                continue;
              std::vector<uint8_t> Bytes;
              if (Address32 != Code32)
                Bytes.push_back(0x67);
              if (Width != 1 && (Width == 4) != Code32)
                Bytes.push_back(0x66);
              Bytes.push_back(0xf0);
              if (Group >= 7)
                Bytes.push_back(0x0f);
              Bytes.push_back(Group >= 7   ? 0xba
                              : Width == 1 ? 0x80
                              : Short      ? 0x83
                                           : 0x81);
              Bytes.push_back(((Group >= 7 ? Group - 2 : Group) << 3) |
                              (Address32 ? 3 : 7));
              Bytes.push_back(Immediate);
              if (Group < 7 && !Short)
                for (unsigned I = 1; I != Width; ++I)
                  Bytes.push_back(0x80);
              BitTestUndefinedFlags = 0x894;
              ASSERT_NO_FATAL_FAILURE(
                  compare(Bytes, Code32, true, 3, 0, 0x80008080, 0, 0xed7));
            }
        }
  RecordProperty("cpu_comparisons", Comparisons);
}

INSTANTIATE_TEST_SUITE_P(LegacyExchanges, X86LegacyExchangeNativeTest,
                         testing::Values(2u, 3u));
